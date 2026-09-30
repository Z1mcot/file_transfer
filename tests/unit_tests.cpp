#include "file_transfer/crc32.hpp"
#include "file_transfer/cli.hpp"
#include "file_transfer/file_store.hpp"
#include "file_transfer/filename_generator.hpp"
#include "file_transfer/i_file_store.hpp"
#include "file_transfer/protocol.hpp"
#include "file_transfer/tcp_transport.hpp"
#include "file_transfer/transfer.hpp"
#include "file_transfer/transport.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <regex>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class PartialMemoryTransport final : public file_transfer::ITransport {
public:
    explicit PartialMemoryTransport(std::size_t max_transfer = 3U) : max_transfer_(max_transfer) {}
    PartialMemoryTransport(std::vector<std::byte> input, std::size_t max_transfer)
        : max_transfer_(max_transfer), bytes_(std::move(input)) {}

    std::size_t read_some(std::span<std::byte> buffer) override {
        const std::size_t available = bytes_.size() - read_offset_;
        const std::size_t count = std::min({buffer.size(), available, max_transfer_});
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(read_offset_),
                    static_cast<std::ptrdiff_t>(count), buffer.begin());
        read_offset_ += count;
        return count;
    }

    std::size_t write_some(std::span<const std::byte> buffer) override {
        const std::size_t count = std::min(buffer.size(), max_transfer_);
        bytes_.insert(bytes_.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(count));
        return count;
    }

    void cancel() noexcept override {}

    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return bytes_; }
    void reset_read() noexcept { read_offset_ = 0; }

private:
    std::size_t max_transfer_;
    std::size_t read_offset_ = 0;
    std::vector<std::byte> bytes_;
};

class FailingStore final : public file_transfer::IFileStore {
public:
    class Staged final : public file_transfer::IStagedFile {
    public:
        explicit Staged(FailingStore& owner) : owner_(owner) {}
        void write(std::span<const std::byte>) override {
            ++owner_.writes;
            throw std::runtime_error("injected disk write failure");
        }
        std::filesystem::path commit() override {
            ++owner_.commits;
            return "not-published.hex";
        }
        void discard() override {
            ++owner_.discards;
            if (owner_.fail_discard) {
                throw std::runtime_error("injected staging cleanup failure");
            }
        }
    private:
        FailingStore& owner_;
    };

    std::unique_ptr<file_transfer::IStagedFile> create_staged_file() override {
        return std::make_unique<Staged>(*this);
    }

    int writes = 0;
    int commits = 0;
    int discards = 0;
    bool fail_discard = false;
};

class RecordingStore final : public file_transfer::IFileStore {
public:
    class Staged final : public file_transfer::IStagedFile {
    public:
        explicit Staged(RecordingStore& owner) : owner_(owner) {}
        void write(std::span<const std::byte> bytes) override {
            owner_.bytes.insert(owner_.bytes.end(), bytes.begin(), bytes.end());
        }
        std::filesystem::path commit() override {
            ++owner_.commits;
            return "recorded.hex";
        }
        void discard() override {}
    private:
        RecordingStore& owner_;
    };

    std::unique_ptr<file_transfer::IStagedFile> create_staged_file() override {
        return std::make_unique<Staged>(*this);
    }

    std::vector<std::byte> bytes;
    int commits = 0;
};

class PublishedCleanupWarningStore final : public file_transfer::IFileStore {
public:
    class Staged final : public file_transfer::IStagedFile {
    public:
        explicit Staged(PublishedCleanupWarningStore& owner) : owner_(owner) {}
        void write(std::span<const std::byte>) override {}
        std::filesystem::path commit() override {
            owner_.published = true;
            try {
                discard();
            } catch (const std::runtime_error&) {
                owner_.cleanup_warning = true;
            }
            return "durable.hex";
        }
        void discard() override {
            if (owner_.published) {
                throw std::runtime_error("injected post-publication cleanup failure");
            }
        }
    private:
        PublishedCleanupWarningStore& owner_;
    };

    std::unique_ptr<file_transfer::IStagedFile> create_staged_file() override {
        return std::make_unique<Staged>(*this);
    }

    bool published = false;
    bool cleanup_warning = false;
};

class FailingResponseTransport final : public file_transfer::ITransport {
public:
    FailingResponseTransport(std::vector<std::byte> input, std::size_t fail_after)
        : input_(std::move(input)), fail_after_(fail_after) {}

    std::size_t read_some(std::span<std::byte> buffer) override {
        const std::size_t available = input_.size() - read_offset_;
        const std::size_t count = std::min({buffer.size(), available, std::size_t{7}});
        std::copy_n(input_.begin() + static_cast<std::ptrdiff_t>(read_offset_),
                    static_cast<std::ptrdiff_t>(count), buffer.begin());
        read_offset_ += count;
        return count;
    }

    std::size_t write_some(std::span<const std::byte> buffer) override {
        ++write_attempts_;
        if (written_.size() >= fail_after_) {
            throw std::runtime_error("injected response write failure");
        }
        const std::size_t count = std::min(buffer.size(), fail_after_ - written_.size());
        written_.insert(written_.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(count));
        return count;
    }

    void cancel() noexcept override {}
    [[nodiscard]] std::size_t write_attempts() const noexcept { return write_attempts_; }
    [[nodiscard]] const std::vector<std::byte>& written() const noexcept { return written_; }

private:
    std::vector<std::byte> input_;
    std::size_t fail_after_;
    std::size_t read_offset_ = 0;
    std::size_t write_attempts_ = 0;
    std::vector<std::byte> written_;
};

void test_crc32() {
    check(file_transfer::crc32(std::string_view{}) == 0U, "CRC32 empty vector");
    check(file_transfer::crc32("123456789") == 0xCBF43926U, "CRC32 standard vector");

    file_transfer::Crc32 split;
    split.update(std::span<const std::byte>(reinterpret_cast<const std::byte*>("1234"), 4U));
    split.update(std::span<const std::byte>(reinterpret_cast<const std::byte*>("56789"), 5U));
    check(split.value() == file_transfer::crc32("123456789"), "incremental CRC32");
}

void test_protocol_round_trip_and_endian() {
    using namespace file_transfer::protocol;
    PartialMemoryTransport transport(2U);
    const Hello hello{0x0102030405060708ULL, 0xA1B2C3D4U, 0x00010000U};
    const auto hello_bytes = encode_hello(hello);
    check(hello_bytes.size() == 16U, "HELLO encoded size");
    check(std::to_integer<unsigned>(hello_bytes[0]) == 1U &&
          std::to_integer<unsigned>(hello_bytes[7]) == 8U &&
          std::to_integer<unsigned>(hello_bytes[8]) == 0xA1U,
          "HELLO network byte order");
    write_message(transport, MessageType::hello, hello_bytes);

    const auto& wire = transport.bytes();
    check(std::to_integer<unsigned>(wire[0]) == 0x46U &&
          std::to_integer<unsigned>(wire[1]) == 0x54U &&
          std::to_integer<unsigned>(wire[2]) == 0x52U &&
          std::to_integer<unsigned>(wire[3]) == 0x4EU,
          "frame magic byte order");
    transport.reset_read();
    const Message message = read_message(transport);
    check(message.type == MessageType::hello, "HELLO message type");
    const Hello decoded = decode_hello(message.payload);
    check(decoded.file_size == hello.file_size && decoded.file_crc32 == hello.file_crc32 &&
          decoded.chunk_size == hello.chunk_size, "HELLO round trip");

    const Finish finish{0xFFFFFFFFFFFFFFFFULL, 0x0102030405060708ULL, 0xFFFFFFFFU};
    const Finish decoded_finish = decode_finish(encode_finish(finish));
    check(decoded_finish.total_bytes == finish.total_bytes &&
          decoded_finish.total_chunks == finish.total_chunks &&
          decoded_finish.file_crc32 == finish.file_crc32, "FINISH uint64 boundaries");

    const Result result{0U, "accepted"};
    const Result decoded_result = decode_result(encode_result(result));
    check(decoded_result.code == result.code && decoded_result.message == result.message,
          "RESULT round trip");
}

void test_partial_io_and_eof() {
    using namespace file_transfer;
    PartialMemoryTransport transport(1U);
    const std::vector<std::byte> source{std::byte{0}, std::byte{1}, std::byte{0xFF}};
    write_all(transport, source);
    transport.reset_read();
    std::vector<std::byte> result(source.size());
    read_exact(transport, result);
    check(result == source, "partial I/O preserves bytes");

    bool eof_seen = false;
    try {
        std::array<std::byte, 1> extra{};
        read_exact(transport, extra);
    } catch (const std::runtime_error&) {
        eof_seen = true;
    }
    check(eof_seen, "read_exact reports EOF");
}

void test_tcp_receive_timeout_configuration() {
    int sockets[2]{};
    check(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "socketpair for TCP transport test");
    {
        file_transfer::TcpTransport transport(sockets[0]);
        timeval timeout{};
        socklen_t timeout_size = sizeof(timeout);
        check(::getsockopt(sockets[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, &timeout_size) == 0,
              "read TCP receive timeout");
        check(timeout.tv_sec == 30 && timeout.tv_usec == 0, "TCP receive inactivity timeout");
        transport.cancel();
    }
    ::close(sockets[1]);
}

void test_cli_parsing() {
    using namespace file_transfer;
    const std::array<std::string_view, 2> server_args{"-s", "--port"};
        bool invalid_port = false;
    try {
        (void)parse_cli(server_args);
    } catch (const std::invalid_argument&) {
            invalid_port = true;
    }
        check(invalid_port, "CLI missing port value");

    const std::array<std::string_view, 3> good_server{"-s", "--port", "0"};
    const auto server = std::get<ServerOptions>(parse_cli(good_server));
    check(server.port == 0U, "CLI server ephemeral port");

        const std::array<std::string_view, 1> default_server_args{"-s"};
        check(std::get<ServerOptions>(parse_cli(default_server_args)).port == 5000U,
            "CLI default server port");

    const std::array<std::string_view, 5> good_client{"-c", "sample.bin", "--host", "localhost", "--port"};
    bool missing_client_port = false;
    try {
        (void)parse_cli(good_client);
    } catch (const std::invalid_argument&) {
        missing_client_port = true;
    }
    check(missing_client_port, "CLI missing client port value");

    const std::array<std::string_view, 6> full_client{
        "-c", "sample.bin", "--host", "localhost", "--port", "65535"};
    const auto client = std::get<ClientOptions>(parse_cli(full_client));
    check(client.host == "localhost" && client.port == 65535U &&
          client.file == std::filesystem::path("sample.bin"), "CLI client options");
        const std::array<std::string_view, 2> default_client_args{"-c", "sample.bin"};
        const auto default_client = std::get<ClientOptions>(parse_cli(default_client_args));
        check(default_client.host == "127.0.0.1" && default_client.port == 5000U,
            "CLI default client endpoint");

    bool cyrillic_flag = false;
    const std::array<std::string_view, 2> cyrillic_client{"-\xD1\x81", "sample.bin"};
    try {
        (void)parse_cli(cyrillic_client);
    } catch (const std::invalid_argument& error) {
        cyrillic_flag = std::string(error.what()).find("Cyrillic") != std::string::npos;
    }
    check(cyrillic_flag, "CLI explains Cyrillic client flag");

    bool missing_mode = false;
    try {
        (void)parse_cli(std::span<const std::string_view>{});
    } catch (const std::invalid_argument&) {
        missing_mode = true;
    }
    check(missing_mode, "CLI rejects missing mode");

    bool invalid_numeric_port = false;
    const std::array<std::string_view, 4> bad_client_port{"-c", "sample.bin", "--port", "70000"};
    try {
        (void)parse_cli(bad_client_port);
    } catch (const std::invalid_argument&) {
        invalid_numeric_port = true;
    }
    check(invalid_numeric_port, "CLI rejects out-of-range port");
}

void test_filename_generation() {
    file_transfer::FilenameGenerator generator;
    const std::regex expected("^[0-9]{8}_[0-9]{6}_[0-9]{6}\\.hex$");
    const auto first = generator.next().string();
    const auto second = generator.next().string();
    check(std::regex_match(first, expected), "output filename format");
    check(first != second, "output filenames are unique");
}

void test_storage_write_failure() {
    using namespace file_transfer;
    using namespace file_transfer::protocol;
    const std::array<std::byte, 1> byte{std::byte{'x'}};
    const std::uint32_t checksum = crc32(byte);
    PartialMemoryTransport request(2U);
    const auto hello = encode_hello({1U, checksum, maximum_chunk_size});
    const auto data = encode_data(0U, checksum, byte);
    const auto finish = encode_finish({1U, 1U, checksum});
    write_message(request, MessageType::hello, hello);
    write_message(request, MessageType::data, data);
    write_message(request, MessageType::finish, finish);

    PartialMemoryTransport peer(request.bytes(), 2U);
    FailingStore store;
    bool failed = false;
    try {
        (void)receive_file(peer, store, {});
    } catch (const std::runtime_error& error) {
        failed = std::string(error.what()).find("injected disk write failure") != std::string::npos;
    }
    check(failed && store.writes == 1 && store.commits == 0,
          "storage failure prevents publishing the file");

    PartialMemoryTransport cleanup_peer(request.bytes(), 2U);
    FailingStore cleanup_store;
    cleanup_store.fail_discard = true;
    bool cleanup_reported = false;
    try {
        (void)receive_file(cleanup_peer, cleanup_store, {});
    } catch (const std::runtime_error& error) {
        cleanup_reported = std::string(error.what()).find("staging cleanup failed") != std::string::npos;
    }
    check(cleanup_reported && cleanup_store.discards == 1,
          "staging cleanup failure is reported to the transfer caller");
}

void test_corrupt_data_block_is_not_committed() {
    using namespace file_transfer;
    using namespace file_transfer::protocol;
    const std::array<std::byte, 3> data{std::byte{0x10}, std::byte{0x20}, std::byte{0x30}};
    const std::uint32_t checksum = crc32(data);
    PartialMemoryTransport request(5U);
    const auto hello = encode_hello({data.size(), checksum, maximum_chunk_size});
    const auto corrupt_data = encode_data(0U, checksum ^ 1U, data);
    write_message(request, MessageType::hello, hello);
    write_message(request, MessageType::data, corrupt_data);

    PartialMemoryTransport peer(request.bytes(), 5U);
    RecordingStore store;
    bool rejected = false;
    try {
        (void)receive_file(peer, store, {});
    } catch (const std::runtime_error& error) {
        rejected = std::string(error.what()).find("DATA block CRC32 mismatch") != std::string::npos;
    }
    check(rejected && store.commits == 0 && store.bytes.empty(),
          "corrupted DATA is rejected before bytes are written or committed");
}

void test_partial_success_ack_does_not_append_failure_frame() {
    using namespace file_transfer;
    using namespace file_transfer::protocol;
    const std::array<std::byte, 1> data{std::byte{0x42}};
    const std::uint32_t checksum = crc32(data);
    PartialMemoryTransport request(32U);
    write_message(request, MessageType::hello, encode_hello({1U, checksum, maximum_chunk_size}));
    write_message(request, MessageType::data, encode_data(0U, checksum, data));
    write_message(request, MessageType::finish, encode_finish({1U, 1U, checksum}));

    FailingResponseTransport transport(request.bytes(), 15U);
    RecordingStore store;
    bool reported_published_file = false;
    try {
        (void)receive_file(transport, store, {});
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        reported_published_file = message.find("recorded.hex") != std::string::npos &&
                                  message.find("acknowledgement failed") != std::string::npos;
    }
    check(reported_published_file && store.commits == 1 &&
          transport.written().size() == 15U && transport.write_attempts() == 3U,
          "partial success ACK reports published path without appending another frame");
}

void test_published_file_with_cleanup_warning_is_success() {
    using namespace file_transfer;
    using namespace file_transfer::protocol;
    PartialMemoryTransport request(32U);
    write_message(request, MessageType::hello,
                  encode_hello({0U, crc32(std::span<const std::byte>{}), maximum_chunk_size}));
    write_message(request, MessageType::finish, encode_finish({0U, 0U, 0U}));

    FailingResponseTransport transport(request.bytes(), std::numeric_limits<std::size_t>::max());
    PublishedCleanupWarningStore store;
    const ReceivedFile received = receive_file(transport, store, {});
    PartialMemoryTransport response(transport.written(), 3U);
    const Message message = read_message(response);
    const Result result = decode_result(message.payload);
    check(store.published && store.cleanup_warning && received.path == "durable.hex" &&
          message.type == MessageType::result && result.code == 0U,
          "published file with staging cleanup warning returns success");
}

void run(const char* name, void (*test)()) {
    test();
    std::cout << "[PASS] " << name << '\n';
}

} // namespace

int main() {
    try {
        run("CRC32", test_crc32);
        run("protocol round trip and endian", test_protocol_round_trip_and_endian);
        run("partial I/O and EOF", test_partial_io_and_eof);
        run("TCP receive timeout", test_tcp_receive_timeout_configuration);
        run("CLI parsing", test_cli_parsing);
        run("filename generation", test_filename_generation);
        run("storage write failure", test_storage_write_failure);
        run("corrupt data block", test_corrupt_data_block_is_not_committed);
        run("partial success ACK", test_partial_success_ack_does_not_append_failure_frame);
        run("published file cleanup warning", test_published_file_with_cleanup_warning_is_success);
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
    return 0;
}
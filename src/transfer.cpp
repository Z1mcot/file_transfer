#include "file_transfer/transfer.hpp"

#include "file_transfer/crc32.hpp"

#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

namespace file_transfer {
namespace {

constexpr std::size_t buffer_size = protocol::maximum_chunk_size;

void pread_exact(int descriptor, std::span<std::byte> buffer, std::uint64_t offset) {
    std::size_t completed = 0;
    while (completed < buffer.size()) {
        const std::uint64_t current = offset + static_cast<std::uint64_t>(completed);
        const auto file_offset = static_cast<off_t>(current);
        const ssize_t count = ::pread(descriptor, buffer.data() + completed, buffer.size() - completed,
                                      file_offset);
        if (count > 0) {
            completed += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count == 0) {
            throw std::runtime_error("input file ended before its declared size");
        }
        throw std::system_error(errno, std::generic_category(), "read input file");
    }
}

void send_result(ITransport& transport, const protocol::Result& result) {
    const std::vector<std::byte> payload = protocol::encode_result(result);
    protocol::write_message(transport, protocol::MessageType::result, payload);
}

} // namespace

FileMetadata calculate_file_metadata(int descriptor) {
    struct stat information {};
    if (::fstat(descriptor, &information) < 0) {
        throw std::system_error(errno, std::generic_category(), "stat input file");
    }
    if (!S_ISREG(information.st_mode) || information.st_size < 0) {
        throw std::runtime_error("input path is not a regular file");
    }

    const auto size = static_cast<std::uint64_t>(information.st_size);
    Crc32 checksum;
    std::array<std::byte, buffer_size> buffer{};
    std::uint64_t offset = 0;
    while (offset < size) {
        const std::size_t requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), size - offset));
        pread_exact(descriptor, std::span<std::byte>(buffer.data(), requested), offset);
        checksum.update(std::span<const std::byte>(buffer.data(), requested));
        offset += static_cast<std::uint64_t>(requested);
    }
    return {size, checksum.value()};
}

void send_file(ITransport& transport, int descriptor, const FileMetadata& metadata) {
    const std::uint32_t chunk_size = protocol::maximum_chunk_size;
    const auto hello = protocol::encode_hello({metadata.size, metadata.crc32, chunk_size});
    protocol::write_message(transport, protocol::MessageType::hello, hello);

    std::uint64_t offset = 0;
    std::uint64_t sequence = 0;
    while (offset < metadata.size) {
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(chunk_size, metadata.size - offset));
        std::vector<std::byte> chunk(count);
        pread_exact(descriptor, chunk, offset);
        const std::uint32_t checksum = crc32(chunk);
        const auto payload = protocol::encode_data(sequence, checksum, chunk);
        protocol::write_message(transport, protocol::MessageType::data, payload);
        offset += static_cast<std::uint64_t>(count);
        ++sequence;
    }

    const auto finish = protocol::encode_finish({metadata.size, sequence, metadata.crc32});
    protocol::write_message(transport, protocol::MessageType::finish, finish);
    const protocol::Message response = protocol::read_message(transport);
    if (response.type != protocol::MessageType::result) {
        throw std::runtime_error("server sent an unexpected response");
    }
    const protocol::Result result = protocol::decode_result(response.payload);
    if (result.code != 0U) {
        throw std::runtime_error("server rejected transfer: " + result.message);
    }
}

ReceivedFile receive_file(ITransport& transport, IFileStore& store,
                          const TransferStarted& on_started) {
    bool can_reply = false;
    std::unique_ptr<IStagedFile> staged;
    std::filesystem::path committed_path;
    try {
        const protocol::Message hello_message = protocol::read_message(transport);
        if (hello_message.type != protocol::MessageType::hello) {
            throw std::runtime_error("expected HELLO message");
        }
        const protocol::Hello hello = protocol::decode_hello(hello_message.payload);
        can_reply = true;
        if (hello.chunk_size == 0U || hello.chunk_size > protocol::maximum_chunk_size) {
            throw std::runtime_error("invalid negotiated chunk size");
        }
        if (on_started) {
            on_started(hello.file_size);
        }

        staged = store.create_staged_file();
        Crc32 whole_file_checksum;
        std::uint64_t received_bytes = 0;
        std::uint64_t received_chunks = 0;
        while (received_bytes < hello.file_size) {
            const protocol::Message data_message = protocol::read_message(transport);
            if (data_message.type != protocol::MessageType::data) {
                throw std::runtime_error("expected DATA message");
            }
            const protocol::DataView data = protocol::decode_data(data_message.payload);
            const std::uint64_t remaining = hello.file_size - received_bytes;
            const auto expected_size = static_cast<std::size_t>(
                std::min<std::uint64_t>(hello.chunk_size, remaining));
            if (data.sequence != received_chunks) {
                throw std::runtime_error("unexpected DATA sequence number");
            }
            if (data.bytes.size() != expected_size) {
                throw std::runtime_error("unexpected DATA block size");
            }
            if (crc32(data.bytes) != data.payload_crc32) {
                throw std::runtime_error("DATA block CRC32 mismatch");
            }
            staged->write(data.bytes);
            whole_file_checksum.update(data.bytes);
            received_bytes += static_cast<std::uint64_t>(data.bytes.size());
            ++received_chunks;
        }

        const protocol::Message finish_message = protocol::read_message(transport);
        if (finish_message.type != protocol::MessageType::finish) {
            throw std::runtime_error("expected FINISH message");
        }
        const protocol::Finish finish = protocol::decode_finish(finish_message.payload);
        const std::uint32_t actual_crc = whole_file_checksum.value();
        if (finish.total_bytes != received_bytes || finish.total_chunks != received_chunks) {
            throw std::runtime_error("FINISH size or chunk count mismatch");
        }
        if (finish.file_crc32 != hello.file_crc32 || finish.file_crc32 != actual_crc) {
            throw std::runtime_error("whole-file CRC32 mismatch");
        }

        committed_path = staged->commit();
        can_reply = false;
        send_result(transport, {0U, "file accepted"});
        return {committed_path, received_bytes};
    } catch (const std::exception& error) {
        std::string message = error.what();
        if (!committed_path.empty()) {
            message = "file published at " + committed_path.string() +
                      "; success acknowledgement failed: " + message;
        }
        if (staged) {
            try {
                staged->discard();
            } catch (const std::exception& cleanup_error) {
                message += "; staging cleanup failed: ";
                message += cleanup_error.what();
            }
        }
        if (can_reply) {
            try {
                if (message.size() > 512U) {
                    message.resize(512U);
                }
                send_result(transport, {1U, message});
            } catch (...) {
            }
        }
        throw std::runtime_error(message);
    }
}

} // namespace file_transfer
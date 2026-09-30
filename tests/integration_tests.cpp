#include <fcntl.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        std::array<char, 32> pattern{};
        constexpr std::string_view seed = "/tmp/file_transfer_itXXXXXX";
        std::copy(seed.begin(), seed.end(), pattern.begin());
        char* created = ::mkdtemp(pattern.data());
        if (created == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        path_ = created;
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void redirect_output(const std::filesystem::path& path) {
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (descriptor < 0 || ::dup2(descriptor, STDOUT_FILENO) < 0 ||
        ::dup2(descriptor, STDERR_FILENO) < 0) {
        _exit(120);
    }
    if (descriptor > STDERR_FILENO) {
        ::close(descriptor);
    }
}

pid_t spawn(const std::filesystem::path& executable,
            const std::vector<std::string>& arguments,
            const std::filesystem::path& log,
            const std::filesystem::path& working_directory = {}) {
    const pid_t child = ::fork();
    if (child < 0) {
        throw std::runtime_error("fork failed");
    }
    if (child == 0) {
        redirect_output(log);
        if (!working_directory.empty() && ::chdir(working_directory.c_str()) < 0) {
            _exit(121);
        }
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 2U);
        argv.push_back(const_cast<char*>(executable.c_str()));
        for (const std::string& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execv(executable.c_str(), argv.data());
        _exit(122);
    }
    return child;
}

bool wait_for_exit(pid_t process, std::chrono::milliseconds timeout, int& status) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t result = ::waitpid(process, &status, WNOHANG);
        if (result == process) {
            return true;
        }
        if (result < 0 && errno != EINTR) {
            throw std::runtime_error("waitpid failed");
        }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

int wait_for_success(pid_t process, std::chrono::milliseconds timeout,
                     const std::string& label) {
    int status = 0;
    if (!wait_for_exit(process, timeout, status)) {
        ::kill(process, SIGKILL);
        (void)::waitpid(process, &status, 0);
        throw std::runtime_error(label + " timed out");
    }
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          label + " exited unsuccessfully (status " + std::to_string(status) + ")");
    return status;
}

void wait_for_failure(pid_t process, std::chrono::milliseconds timeout,
                      const std::string& label) {
    int status = 0;
    if (!wait_for_exit(process, timeout, status)) {
        ::kill(process, SIGKILL);
        (void)::waitpid(process, &status, 0);
        throw std::runtime_error(label + " timed out");
    }
    check(WIFEXITED(status) && WEXITSTATUS(status) != 0,
          label + " unexpectedly succeeded");
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::size_t occurrences(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string_view::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

template <typename Predicate>
void wait_until(Predicate predicate, std::chrono::milliseconds timeout, const std::string& label) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return;
        }
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("timed out waiting for " + label);
}

class Server final {
public:
    Server(const std::filesystem::path& executable, const std::filesystem::path& root)
        : executable_(executable), log_(root / "server.log"), cwd_(root / "foreign-cwd") {
        std::filesystem::create_directories(cwd_);
        process_ = spawn(executable_, {"-s", "--port", "0"}, log_, cwd_);
        try {
            wait_until([this] { return read_text(log_).find("[SERVER] Listening on ") != std::string::npos; },
                       10s, "server startup");
            const std::string text = read_text(log_);
            const std::regex endpoint(R"(Listening on 0\.0\.0\.0:([0-9]+))");
            std::smatch match;
            check(std::regex_search(text, match, endpoint), "server did not report its listening port");
            port_ = match[1].str();
        } catch (...) {
            stop();
            throw;
        }
    }

    ~Server() { stop(); }
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    [[nodiscard]] const std::string& port() const noexcept { return port_; }
    [[nodiscard]] const std::filesystem::path& log() const noexcept { return log_; }

    int connect_idle_client() const {
        const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
        check(descriptor >= 0, "could not create stalled client socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(std::stoul(port_)));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
            ::close(descriptor);
            throw std::runtime_error("could not connect stalled client socket");
        }
        return descriptor;
    }

    void send_signal(int signal_number) const {
        check(::kill(process_, signal_number) == 0, "could not send test signal to server");
    }

    pid_t start_client(const std::filesystem::path& file, const std::filesystem::path& log) const {
        return spawn(executable_, {"-c", file.string(), "--host", "127.0.0.1", "--port", port_}, log);
    }

    void stop(int signal_number = SIGTERM) noexcept {
        if (process_ <= 0) {
            return;
        }
        ::kill(process_, signal_number);
        int status = 0;
        try {
            if (!wait_for_exit(process_, 5s, status)) {
                ::kill(process_, SIGKILL);
                (void)::waitpid(process_, &status, 0);
            }
        } catch (...) {
            ::kill(process_, SIGKILL);
            (void)::waitpid(process_, &status, 0);
        }
        process_ = -1;
    }

private:
    std::filesystem::path executable_;
    std::filesystem::path log_;
    std::filesystem::path cwd_;
    std::string port_;
    pid_t process_ = -1;
};

void write_pattern_file(const std::filesystem::path& path, std::uint64_t size, std::uint32_t seed) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    check(static_cast<bool>(output), "could not create test input " + path.string());
    std::array<char, 65536> buffer{};
    std::uint64_t offset = 0;
    while (offset < size) {
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), size - offset));
        for (std::size_t index = 0; index < count; ++index) {
            const std::uint64_t position = offset + static_cast<std::uint64_t>(index);
            const std::uint32_t mixed = static_cast<std::uint32_t>(position * 2654435761ULL) ^ seed;
            buffer[index] = static_cast<char>(mixed & 0xFFU);
        }
        output.write(buffer.data(), static_cast<std::streamsize>(count));
        check(static_cast<bool>(output), "failed writing test input");
        offset += static_cast<std::uint64_t>(count);
    }
}

void create_sparse_file(const std::filesystem::path& path, std::uint64_t size) {
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    check(descriptor >= 0, "could not create interruption test file");
    const int result = ::ftruncate(descriptor, static_cast<off_t>(size));
    const int saved_errno = errno;
    ::close(descriptor);
    if (result < 0) {
        throw std::runtime_error("ftruncate failed: " + std::to_string(saved_errno));
    }
}

bool files_equal(const std::filesystem::path& first, const std::filesystem::path& second) {
    if (std::filesystem::file_size(first) != std::filesystem::file_size(second)) {
        return false;
    }
    std::ifstream left(first, std::ios::binary);
    std::ifstream right(second, std::ios::binary);
    std::array<char, 65536> left_buffer{};
    std::array<char, 65536> right_buffer{};
    for (;;) {
        left.read(left_buffer.data(), static_cast<std::streamsize>(left_buffer.size()));
        right.read(right_buffer.data(), static_cast<std::streamsize>(right_buffer.size()));
        const std::streamsize left_count = left.gcount();
        const std::streamsize right_count = right.gcount();
        if (left_count != right_count ||
            !std::equal(left_buffer.begin(), left_buffer.begin() + left_count, right_buffer.begin())) {
            return false;
        }
        if (left_count == 0) {
            return true;
        }
    }
}

std::vector<std::filesystem::path> final_files(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> result;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() == ".hex") {
            result.push_back(entry.path());
        }
    }
    return result;
}

std::size_t staged_files(const std::filesystem::path& directory) {
    std::size_t count = 0;
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            if (entry.path().extension() == ".part") {
                ++count;
            }
        }
    } catch (const std::filesystem::filesystem_error& error) {
        if (error.code() != std::errc::no_such_file_or_directory) {
            throw;
        }
    }
    return count;
}

std::size_t staging_directories(const std::filesystem::path& directory) {
    std::size_t count = 0;
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            if (entry.is_directory() && entry.path().filename().string().starts_with(".file_transfer.")) {
                ++count;
            }
        }
    } catch (const std::filesystem::filesystem_error& error) {
        if (error.code() != std::errc::no_such_file_or_directory) {
            throw;
        }
    }
    return count;
}

std::uintmax_t staged_bytes(const std::filesystem::path& directory) {
    std::uintmax_t total = 0;
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            if (entry.path().extension() == ".part") {
                total += entry.file_size();
            }
        }
    } catch (const std::filesystem::filesystem_error& error) {
        if (error.code() != std::errc::no_such_file_or_directory) {
            throw;
        }
    }
    return total;
}

void verify_received_files(const std::filesystem::path& output_directory,
                          const std::vector<std::filesystem::path>& inputs) {
    std::vector<std::filesystem::path> outputs = final_files(output_directory);
    check(outputs.size() == inputs.size(), "unexpected number of published .hex files");
    std::vector<bool> matched(outputs.size(), false);
    for (const auto& input : inputs) {
        bool found = false;
        for (std::size_t index = 0; index < outputs.size(); ++index) {
            if (!matched[index] && files_equal(input, outputs[index])) {
                matched[index] = true;
                found = true;
                break;
            }
        }
        check(found, "received file did not match source bytes: " + input.string());
    }
}

void test_transfers_and_concurrency(const Server& server,
                                    const std::filesystem::path& root,
                                    const std::filesystem::path& output_directory) {
    const std::filesystem::path inputs_directory = root / "inputs";
    std::filesystem::create_directories(inputs_directory);
    std::vector<std::filesystem::path> inputs;

    const auto empty = inputs_directory / "empty.bin";
    write_pattern_file(empty, 0U, 1U);
    inputs.push_back(empty);
    const auto one_byte = inputs_directory / "one.bin";
    write_pattern_file(one_byte, 1U, 2U);
    inputs.push_back(one_byte);
    const auto binary = inputs_directory / "all-bytes.bin";
    write_pattern_file(binary, 4096U, 3U);
    {
        std::fstream file(binary, std::ios::binary | std::ios::in | std::ios::out);
        for (unsigned int value = 0; value <= 255U; ++value) {
            const char byte = static_cast<char>(value);
            file.write(&byte, 1);
        }
    }
    inputs.push_back(binary);
    const auto large = inputs_directory / "large.bin";
    write_pattern_file(large, 32U * 1024U * 1024U, 4U);
    inputs.push_back(large);

    std::size_t client_index = 0;
    for (const auto& input : inputs) {
        const pid_t client = server.start_client(input, root / ("client-" + std::to_string(client_index++) + ".log"));
        wait_for_success(client, 30s, "client transfer");
    }
    verify_received_files(output_directory, inputs);
    check(staged_files(output_directory) == 0U, "staged file remained after successful transfers");
        check(staging_directories(output_directory) == 0U,
            "staging directory remained after successful transfers");

    std::vector<pid_t> clients;
    for (std::uint32_t index = 0; index < 8U; ++index) {
        const auto input = inputs_directory / ("parallel-" + std::to_string(index) + ".bin");
        write_pattern_file(input, 512U * 1024U + index, 100U + index);
        inputs.push_back(input);
        clients.push_back(server.start_client(input, root / ("parallel-client-" + std::to_string(index) + ".log")));
    }
    for (const pid_t client : clients) {
        wait_for_success(client, 30s, "parallel client transfer");
    }
    verify_received_files(output_directory, inputs);
    check(staged_files(output_directory) == 0U, "staged file remained after parallel transfers");
        check(staging_directories(output_directory) == 0U,
            "staging directory remained after parallel transfers");
}

void test_interrupted_client_and_recovery(const Server& server,
                                          const std::filesystem::path& root,
                                          const std::filesystem::path& output_directory,
                                          const std::vector<std::filesystem::path>& previous_inputs) {
    const std::size_t previous_count = final_files(output_directory).size();
    const auto interrupted = root / "interrupted-large.bin";
    create_sparse_file(interrupted, 512U * 1024U * 1024U);
    const pid_t client = server.start_client(interrupted, root / "interrupted-client.log");

    int client_status = 0;
    bool killed = false;
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (staged_bytes(output_directory) != 0U) {
            if (::kill(client, SIGKILL) == 0) {
                killed = true;
            }
            break;
        }
        const pid_t waited = ::waitpid(client, &client_status, WNOHANG);
        if (waited == client) {
            break;
        }
        if (waited < 0 && errno != EINTR) {
            throw std::runtime_error("waitpid failed while interrupting client");
        }
        std::this_thread::sleep_for(1ms);
    }
    check(killed, "large client completed before it could be interrupted");
    check(::waitpid(client, &client_status, 0) == client, "could not reap interrupted client");
    check(WIFSIGNALED(client_status) && WTERMSIG(client_status) == SIGKILL,
          "client was not killed during transfer");

    wait_until([&] {
        return read_text(server.log()).find("[SERVER] Transfer failed:") != std::string::npos &&
             staged_files(output_directory) == 0U && staging_directories(output_directory) == 0U;
    }, 15s, "server cleanup after client disconnect");
    check(final_files(output_directory).size() == previous_count,
          "interrupted transfer produced a final .hex file");

    const auto recovery = root / "after-interruption.bin";
    write_pattern_file(recovery, 16387U, 0x5A17U);
    const pid_t next_client = server.start_client(recovery, root / "recovery-client.log");
    wait_for_success(next_client, 15s, "client after interruption");

    std::vector<std::filesystem::path> expected = previous_inputs;
    expected.push_back(recovery);
    verify_received_files(output_directory, expected);
    check(staged_files(output_directory) == 0U, "staged file remained after recovery transfer");
    check(staging_directories(output_directory) == 0U,
          "staging directory remained after recovery transfer");
}

void test_worker_limit_and_recovery(const Server& server,
                                    const std::filesystem::path& root,
                                    const std::filesystem::path& output_directory) {
    const std::string rejected_marker = "[SERVER] Client rejected: active connection limit reached";
    const std::string joined_marker = "[SERVER] Worker joined:";
    constexpr std::size_t successful_transfers_before = 12U;
    wait_until([&] {
        return occurrences(read_text(server.log()), joined_marker) == successful_transfers_before;
    }, 10s, "reap of the 12 previous completed transfers");
    const std::size_t rejected_before = occurrences(read_text(server.log()), rejected_marker);
    const std::size_t joined_before = occurrences(read_text(server.log()), joined_marker);
    std::vector<int> clients;
    for (int index = 0; index < 34; ++index) {
        clients.push_back(server.connect_idle_client());
    }
    wait_until([&] {
        return occurrences(read_text(server.log()), rejected_marker) == rejected_before + 2U;
    }, 10s, "active-client limit");
    for (const int descriptor : clients) {
        ::close(descriptor);
    }
    wait_until([&] {
        return occurrences(read_text(server.log()), joined_marker) == joined_before + 32U;
    }, 10s, "worker cleanup after idle client close");

    const auto recovery = root / "inputs" / "after-limit.bin";
    write_pattern_file(recovery, 2049U, 0x32U);
    const std::vector<std::filesystem::path> previous_files = final_files(output_directory);
    const pid_t client = server.start_client(recovery, root / "limit-recovery-client.log");
    wait_for_success(client, 10s, "client after worker limit");
    const auto published_files = final_files(output_directory);
    check(published_files.size() == previous_files.size() + 1U,
          "worker-limit recovery did not publish exactly one file");
    bool bytes_match = false;
    for (const auto& published : published_files) {
        if (files_equal(recovery, published)) {
            bytes_match = true;
            break;
        }
    }
    check(bytes_match, "worker-limit recovery file bytes differ");
}

void run_integration(const std::filesystem::path& original_executable) {
    TemporaryDirectory temporary;
    const auto root = temporary.path();
    const auto executable_directory = root / "bin";
    const auto foreign_cwd = root / "foreign-cwd";
    std::filesystem::create_directories(executable_directory);
    const auto executable = executable_directory / "file_transfer";
    std::filesystem::copy_file(original_executable, executable,
                               std::filesystem::copy_options::overwrite_existing);
    const auto output_directory = executable_directory;

    Server server(executable, root);
    const auto invalid_arguments_log = root / "invalid-arguments.log";
    wait_for_failure(spawn(executable, {}, invalid_arguments_log), 5s, "missing CLI mode");
    wait_for_failure(spawn(executable, {"-x"}, invalid_arguments_log), 5s, "unknown CLI mode");
    wait_for_failure(spawn(executable, {"-c", root.string()}, invalid_arguments_log),
                     5s, "directory input path");
    wait_for_failure(spawn(executable, {"-c", (root / "missing.bin").string()}, invalid_arguments_log),
                     5s, "missing input path");
        check(read_text(invalid_arguments_log).find("[CLIENT] Transfer failed:") != std::string::npos,
            "client did not clearly report input/transfer failure");
    wait_for_failure(spawn(executable, {"-\xD1\x81", "sample.bin"}, invalid_arguments_log),
                     5s, "Cyrillic client flag");
    std::vector<std::filesystem::path> inputs;
    test_transfers_and_concurrency(server, root, output_directory);
    test_worker_limit_and_recovery(server, root, output_directory);
    const auto inputs_directory = root / "inputs";
    for (const auto& entry : std::filesystem::directory_iterator(inputs_directory)) {
        inputs.push_back(entry.path());
    }
    check(std::filesystem::current_path() != foreign_cwd, "test parent unexpectedly changed cwd");
    check(final_files(root).empty(), "received files were saved relative to server cwd");
    verify_received_files(output_directory, inputs);
    test_interrupted_client_and_recovery(server, root, output_directory, inputs);

    const std::string log = read_text(server.log());
    check(log.find("[SERVER] Transfer failed:") != std::string::npos,
          "server did not report the interrupted transfer");
        const std::string connection_marker = "[SERVER] Client connected:";
        const std::size_t previous_connections = occurrences(log, connection_marker);
        const std::size_t previous_failures = occurrences(log, "[SERVER] Transfer failed:");
        const int stalled_client = server.connect_idle_client();
        wait_until([&] {
          return occurrences(read_text(server.log()), connection_marker) > previous_connections;
        }, 5s, "stalled client acceptance");
    server.stop();
        ::close(stalled_client);
        const std::string stopped_log = read_text(server.log());
        check(stopped_log.find("[SERVER] Stopped") != std::string::npos,
          "server did not stop cleanly");
        check(occurrences(stopped_log, "[SERVER] Transfer failed:") > previous_failures,
            "server shutdown did not cancel its active worker");

            Server interrupt_server(executable, root / "interrupt-server");
            interrupt_server.send_signal(SIGUSR1);
            interrupt_server.stop(SIGINT);
            check(read_text(interrupt_server.log()).find("[SERVER] Stopped") != std::string::npos,
                "server did not stop cleanly on SIGINT");
}

} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 2, "expected file_transfer executable path");
        run_integration(argv[1]);
        std::cout << "[PASS] process integration: payloads, concurrency, interruption, recovery, executable directory\n";
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] integration: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
#include "file_transfer/cli.hpp"
#include "file_transfer/storage.hpp"
#include "file_transfer/tcp_transport.hpp"
#include "file_transfer/transfer.hpp"

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <syncstream>
#include <system_error>
#include <thread>
#include <variant>
#include <vector>

namespace {

void install_signal_handlers() {
    struct sigaction ignore_pipe {};
    ignore_pipe.sa_handler = SIG_IGN;
    ::sigemptyset(&ignore_pipe.sa_mask);
    if (::sigaction(SIGPIPE, &ignore_pipe, nullptr) < 0) {
        throw std::system_error(errno, std::generic_category(), "ignore SIGPIPE");
    }
    if (::sigaction(SIGUSR1, &ignore_pipe, nullptr) < 0) {
        throw std::system_error(errno, std::generic_category(), "ignore SIGUSR1");
    }
}

class SignalWaiter final {
public:
    SignalWaiter(file_transfer::ITransportListener& listener,
                 std::atomic<bool>& stopping, const sigset_t& signals)
        : thread_([this, &listener, &stopping, signals]() {
              int received_signal = 0;
              (void)::sigwait(&signals, &received_signal);
              stopping.store(true, std::memory_order_release);
              listener.cancel();
              finished_.store(true, std::memory_order_release);
          }) {}

    ~SignalWaiter() {
        if (thread_.joinable()) {
            if (!finished_.load(std::memory_order_acquire)) {
                (void)::pthread_kill(thread_.native_handle(), SIGTERM);
            }
            thread_.join();
        }
    }
    SignalWaiter(const SignalWaiter&) = delete;
    SignalWaiter& operator=(const SignalWaiter&) = delete;

private:
    std::atomic<bool> finished_{false};
    std::thread thread_;
};

std::filesystem::path executable_directory() {
    std::vector<char> buffer(256U);
    for (;;) {
        const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            throw std::system_error(errno, std::generic_category(), "read /proc/self/exe");
        }
        if (static_cast<std::size_t>(length) < buffer.size()) {
            return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(length)))
                .parent_path();
        }
        if (buffer.size() >= 1024U * 1024U) {
            throw std::runtime_error("executable path is too long");
        }
        buffer.resize(buffer.size() * 2U);
    }
}

void log_line(const std::string& message) {
    std::osyncstream output(std::cout);
    output << message << std::endl;
}

struct Worker {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
    std::shared_ptr<file_transfer::ITransport> transport;
    std::string peer;
};

constexpr std::size_t maximum_active_clients = 32U;

void remove_active_transport(std::mutex& mutex,
                             std::vector<std::shared_ptr<file_transfer::ITransport>>& active,
                             const std::shared_ptr<file_transfer::ITransport>& transport) {
    std::lock_guard lock(mutex);
    const auto found = std::find(active.begin(), active.end(), transport);
    if (found != active.end()) {
        active.erase(found);
    }
}

class WorkerCleanup final {
public:
    WorkerCleanup(file_transfer::ITransportListener& listener, std::mutex& active_mutex,
                  std::vector<std::shared_ptr<file_transfer::ITransport>>& active,
                  std::list<Worker>& workers) noexcept
        : listener_(listener), active_mutex_(active_mutex), active_(active), workers_(workers) {}

    ~WorkerCleanup() { shutdown(); }
    WorkerCleanup(const WorkerCleanup&) = delete;
    WorkerCleanup& operator=(const WorkerCleanup&) = delete;

    void shutdown() noexcept {
        if (shutdown_) {
            return;
        }
        shutdown_ = true;
        listener_.cancel();
        {
            std::lock_guard lock(active_mutex_);
            for (const auto& transport : active_) {
                transport->cancel();
            }
        }
        for (Worker& worker : workers_) {
            if (worker.thread.joinable()) {
                worker.thread.join();
            }
        }
    }

private:
    file_transfer::ITransportListener& listener_;
    std::mutex& active_mutex_;
    std::vector<std::shared_ptr<file_transfer::ITransport>>& active_;
    std::list<Worker>& workers_;
    bool shutdown_ = false;
};

void run_server(std::uint16_t port, const std::filesystem::path& output_directory) {
    install_signal_handlers();
    sigset_t stop_signals{};
    ::sigemptyset(&stop_signals);
    ::sigaddset(&stop_signals, SIGINT);
    ::sigaddset(&stop_signals, SIGTERM);
    const int mask_error = ::pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);
    if (mask_error != 0) {
        throw std::system_error(mask_error, std::generic_category(), "block server stop signals");
    }

    file_transfer::TcpListener listener(port);
    auto store = std::make_shared<file_transfer::FileStore>(output_directory);
    log_line("[SERVER] Listening on " + listener.local_endpoint());

    std::atomic<bool> stopping{false};
    SignalWaiter signal_waiter(listener, stopping, stop_signals);

    std::mutex active_mutex;
    std::vector<std::shared_ptr<file_transfer::ITransport>> active;
    std::list<Worker> workers;
    WorkerCleanup worker_cleanup(listener, active_mutex, active, workers);
    std::chrono::milliseconds accept_retry_delay{100};

    while (!stopping.load(std::memory_order_acquire)) {
        for (auto iterator = workers.begin(); iterator != workers.end();) {
            if (iterator->done->load(std::memory_order_acquire)) {
                if (iterator->thread.joinable()) {
                    iterator->thread.join();
                }
                {
                    std::lock_guard lock(active_mutex);
                    const auto found = std::find(active.begin(), active.end(), iterator->transport);
                    if (found != active.end()) {
                        active.erase(found);
                    }
                }
                log_line("[SERVER] Worker joined: " + iterator->peer);
                iterator = workers.erase(iterator);
            } else {
                ++iterator;
            }
        }

        std::optional<file_transfer::AcceptedConnection> incoming;
        try {
            incoming = listener.accept();
        } catch (const std::system_error& error) {
            if (error.code().value() == ECANCELED && stopping.load(std::memory_order_acquire)) {
                break;
            }
            if (error.code().value() == EINTR) {
                continue;
            }
            if (stopping.load(std::memory_order_acquire)) {
                break;
            }
            log_line("[SERVER] Accept failed: " + std::string(error.what()));
            const int code = error.code().value();
            if (code == EMFILE || code == ENFILE || code == ENOBUFS || code == ENOMEM) {
                std::this_thread::sleep_for(accept_retry_delay);
                accept_retry_delay = std::min(accept_retry_delay * 2, std::chrono::milliseconds{2000});
            }
            continue;
        }
        if (!incoming) {
            continue;
        }
        file_transfer::AcceptedConnection accepted = std::move(*incoming);
        accept_retry_delay = std::chrono::milliseconds{100};

        log_line("[SERVER] Client connected: " + accepted.peer);
        auto transport = std::shared_ptr<file_transfer::ITransport>(std::move(accepted.transport));
        bool admitted = false;
        {
            std::lock_guard lock(active_mutex);
            if (active.size() < maximum_active_clients) {
                active.push_back(transport);
                admitted = true;
            }
        }
        if (!admitted) {
            transport->cancel();
            log_line("[SERVER] Client rejected: active connection limit reached");
            continue;
        }
        auto done = std::make_shared<std::atomic<bool>>(false);
        workers.emplace_back();
        Worker& worker = workers.back();
        worker.done = done;
        worker.transport = transport;
        worker.peer = accepted.peer;
        try {
            worker.thread = std::thread([transport, store, done, &listener]() {
                try {
                    const file_transfer::ReceivedFile received = file_transfer::receive_file(
                        *transport, *store, [](std::uint64_t size) {
                            log_line("[SERVER] Transfer started: " + std::to_string(size) + " bytes");
                        });
                    log_line("[SERVER] Transfer completed: " + received.path.string());
                } catch (const std::exception& error) {
                    log_line("[SERVER] Transfer failed: " + std::string(error.what()));
                } catch (...) {
                    log_line("[SERVER] Transfer failed: unknown error");
                }
                done->store(true, std::memory_order_release);
                listener.notify();
            });
        } catch (const std::exception& error) {
            remove_active_transport(active_mutex, active, transport);
            done->store(true, std::memory_order_release);
            log_line("[SERVER] Could not start worker: " + std::string(error.what()));
        }
    }

    worker_cleanup.shutdown();
    log_line("[SERVER] Stopped");
}

class FileDescriptor final {
public:
    explicit FileDescriptor(int value) noexcept : value_(value) {}
    ~FileDescriptor() {
        if (value_ >= 0) {
            ::close(value_);
        }
    }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    [[nodiscard]] int get() const noexcept { return value_; }

private:
    int value_;
};

void run_client(const file_transfer::ClientOptions& options) {
    const int descriptor = ::open(options.file.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(), "open input file");
    }
    FileDescriptor input(descriptor);
    const file_transfer::FileMetadata metadata = file_transfer::calculate_file_metadata(input.get());

    log_line("[CLIENT] Connecting to " + options.host + ":" + std::to_string(options.port));
    std::unique_ptr<file_transfer::ITransport> transport =
        file_transfer::connect_tcp(options.host, options.port);
    log_line("[CLIENT] Transfer started: " + options.file.string() + " (" +
             std::to_string(metadata.size) + " bytes)");
    file_transfer::send_file(*transport, input.get(), metadata);
    log_line("[CLIENT] Transfer completed successfully");
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    try {
        const file_transfer::Command command = file_transfer::parse_cli(arguments);
        if (std::holds_alternative<file_transfer::HelpOptions>(command)) {
            std::cout << file_transfer::usage_text();
            return 0;
        }
        if (const auto* server = std::get_if<file_transfer::ServerOptions>(&command)) {
            run_server(server->port, executable_directory());
        } else {
            try {
                run_client(std::get<file_transfer::ClientOptions>(command));
            } catch (const std::exception& error) {
                log_line("[CLIENT] Transfer failed: " + std::string(error.what()));
                return 1;
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] " << error.what() << '\n'
                  << file_transfer::usage_text();
        return 2;
    }
}
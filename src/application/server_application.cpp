#include "file_transfer/application/server_application.hpp"

#include "file_transfer/application/event_loop.hpp"
#include "file_transfer/application/logging.hpp"
#include "file_transfer/application/signal_waiter.hpp"
#include "file_transfer/checksum/crc32.hpp"
#include "file_transfer/protocol/frame_parser.hpp"
#include "file_transfer/storage/file_store.hpp"
#include "file_transfer/transport/nonblocking_transport.hpp"
#include "file_transfer/transport/tcp_listener.hpp"

#include <unistd.h>
#if defined(__linux__)
#include <sys/timerfd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace file_transfer {
namespace {

constexpr std::size_t dispatch_budget = 256U * 1024U;
constexpr std::uint32_t magic = 0x4654524EU;

void append_u16(std::vector<std::byte>& output, std::uint16_t value) {
    output.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
    output.push_back(static_cast<std::byte>(value & 0xFFU));
}

void append_u32(std::vector<std::byte>& output, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::byte>((value >> static_cast<unsigned>(shift)) & 0xFFU));
    }
}

std::vector<std::byte> encode_message(protocol::MessageType type, std::span<const std::byte> payload) {
    std::vector<std::byte> bytes;
    bytes.reserve(protocol::frame_header_size + payload.size());
    append_u32(bytes, magic);
    append_u16(bytes, protocol::version);
    append_u16(bytes, static_cast<std::uint16_t>(type));
    append_u32(bytes, static_cast<std::uint32_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

class ServerConnection final : public std::enable_shared_from_this<ServerConnection> {
public:
    ServerConnection(AcceptedConnection connection, std::shared_ptr<IFileStore> store)
        : transport_(std::move(connection.transport)),
          stream_(dynamic_cast<INonBlockingTransport*>(transport_.get())),
          store_(std::move(store)), peer_(std::move(connection.peer)) {
        if (stream_ == nullptr) throw std::runtime_error("accepted transport does not support non-blocking I/O");
    }

    [[nodiscard]] int fd() const noexcept { return stream_->fd(); }
    [[nodiscard]] bool dead() const noexcept { return dead_; }
    [[nodiscard]] bool timed_out(std::chrono::steady_clock::time_point now,
                                 std::chrono::milliseconds timeout) const noexcept {
        return now - last_activity_ >= timeout;
    }

    void on_event(application::EventLoop& loop, std::uint32_t events) {
        if ((events & application::event_error) != 0U || (events & application::event_hangup) != 0U) {
            fail(loop, "socket closed by peer");
            return;
        }
        if ((events & application::event_read) != 0U) read_available(loop);
        if (!dead_ && (events & application::event_write) != 0U) write_available(loop);
    }

    void close(application::EventLoop& loop) noexcept {
        if (!dead_) {
            dead_ = true;
            loop.remove(fd());
            transport_->cancel();
        }
        if (staged_) {
            try { staged_->discard(); } catch (...) {}
            staged_.reset();
        }
    }

private:
    void read_available(application::EventLoop& loop) {
        std::array<std::byte, 64U * 1024U> buffer{};
        std::size_t budget = dispatch_budget;
        while (budget > 0U && !dead_) {
            const std::size_t requested = std::min(buffer.size(), budget);
            const NonBlockingResult result = stream_->recv_nonblocking(std::span<std::byte>(buffer.data(), requested));
            if (result.status == NonBlockingStatus::progress) {
                last_activity_ = std::chrono::steady_clock::now();
                budget -= result.count;
                try {
                    parser_.feed(std::span<const std::byte>(buffer.data(), result.count));
                    while (parser_.has_message()) {
                        process(parser_.pop_message(), loop);
                        if (dead_) return;
                    }
                } catch (const std::exception& error) {
                    fail(loop, error.what());
                    return;
                }
                continue;
            }
            if (result.status == NonBlockingStatus::would_block) return;
            if (result.status == NonBlockingStatus::eof) {
                try { parser_.finish(); }
                catch (const std::exception& error) { fail(loop, error.what()); return; }
                close(loop);
                return;
            }
            fail(loop, std::system_category().message(result.error));
        }
    }

    void process(protocol::Message message, application::EventLoop& loop) {
        if (state_ == State::await_hello) {
            if (message.type != protocol::MessageType::hello) throw std::runtime_error("expected HELLO message");
            const protocol::Hello hello = protocol::decode_hello(message.payload);
            if (hello.chunk_size == 0U || hello.chunk_size > protocol::maximum_chunk_size) {
                throw std::runtime_error("invalid negotiated chunk size");
            }
            expected_size_ = hello.file_size;
            expected_crc_ = hello.file_crc32;
            chunk_size_ = hello.chunk_size;
            staged_ = store_->create_staged_file();
            state_ = expected_size_ == 0U ? State::await_finish : State::receiving;
            application::log_parts("[SERVER] Transfer started: ", expected_size_, " bytes");
            return;
        }
        if (state_ == State::receiving) {
            if (message.type != protocol::MessageType::data) throw std::runtime_error("expected DATA message");
            const protocol::DataView data = protocol::decode_data(message.payload);
            const std::uint64_t remaining = expected_size_ - received_bytes_;
            const std::size_t expected = static_cast<std::size_t>(std::min<std::uint64_t>(chunk_size_, remaining));
            if (data.sequence != received_chunks_ || data.bytes.size() != expected || crc32(data.bytes) != data.payload_crc32) {
                throw std::runtime_error("invalid DATA block");
            }
            staged_->write(data.bytes);
            checksum_.update(data.bytes);
            received_bytes_ += static_cast<std::uint64_t>(data.bytes.size());
            ++received_chunks_;
            if (received_bytes_ == expected_size_) state_ = State::await_finish;
            return;
        }
        if (state_ == State::await_finish) {
            if (message.type != protocol::MessageType::finish) throw std::runtime_error("expected FINISH message");
            const protocol::Finish finish = protocol::decode_finish(message.payload);
            if (finish.total_bytes != received_bytes_ || finish.total_chunks != received_chunks_ ||
                finish.file_crc32 != expected_crc_ || checksum_.value() != expected_crc_) {
                throw std::runtime_error("whole-file CRC32 or size mismatch");
            }
            const std::filesystem::path path = staged_->commit();
            staged_.reset();
            queue_result(loop, {0U, "file accepted"});
            application::log_parts("[SERVER] Transfer completed: ", path.string());
            state_ = State::sending_result;
            return;
        }
        throw std::runtime_error("unexpected message after FINISH");
    }

    void fail(application::EventLoop& loop, std::string message) {
        if (dead_) return;
        application::log_parts("[SERVER] Transfer failed: ", message);
        if (staged_) {
            try { staged_->discard(); } catch (...) {}
            staged_.reset();
        }
        if (state_ != State::sending_result) {
            if (message.size() > 512U) message.resize(512U);
            try {
                queue_result(loop, {1U, message});
                state_ = State::sending_result;
                return;
            } catch (...) {}
        }
        close(loop);
    }

    void queue_result(application::EventLoop& loop, const protocol::Result& result) {
        const auto payload = protocol::encode_result(result);
        output_ = encode_message(protocol::MessageType::result, payload);
        output_offset_ = 0U;
        loop.modify(fd(), application::event_read | application::event_write);
    }

    void write_available(application::EventLoop& loop) {
        while (output_offset_ < output_.size()) {
            const NonBlockingResult result = stream_->send_nonblocking(
                std::span<const std::byte>(output_.data() + output_offset_, output_.size() - output_offset_));
            if (result.status == NonBlockingStatus::progress) {
                last_activity_ = std::chrono::steady_clock::now();
                output_offset_ += result.count;
                continue;
            }
            if (result.status == NonBlockingStatus::would_block) return;
            close(loop);
            return;
        }
        close(loop);
    }

    enum class State { await_hello, receiving, await_finish, sending_result };
    std::unique_ptr<ITransport> transport_;
    INonBlockingTransport* stream_;
    std::shared_ptr<IFileStore> store_;
    std::string peer_;
    protocol::FrameParser parser_;
    std::unique_ptr<IStagedFile> staged_;
    std::vector<std::byte> output_;
    std::size_t output_offset_ = 0U;
    std::uint64_t expected_size_ = 0U;
    std::uint64_t received_bytes_ = 0U;
    std::uint64_t received_chunks_ = 0U;
    std::uint32_t expected_crc_ = 0U;
    std::uint32_t chunk_size_ = 0U;
    Crc32 checksum_;
    State state_ = State::await_hello;
    bool dead_ = false;
    std::chrono::steady_clock::time_point last_activity_ = std::chrono::steady_clock::now();
};

} // namespace

ServerApplication::ServerApplication(std::uint16_t port, std::filesystem::path output_directory,
                                     std::uint64_t idle_timeout_ms)
    : port_(port), output_directory_(std::move(output_directory)), idle_timeout_ms_(idle_timeout_ms) {}

void ServerApplication::run() {
    TcpListener listener(port_);
    auto store = std::make_shared<FileStore>(output_directory_);
    application::EventLoop loop;
    std::atomic<bool> stopping{false};
    application::SignalWaiter signal_waiter(listener, stopping);
    std::vector<std::shared_ptr<ServerConnection>> connections;
    const auto check_timeouts = [&]() {
        const auto now = std::chrono::steady_clock::now();
        const auto timeout = std::chrono::milliseconds(idle_timeout_ms_);
        for (const auto& connection : connections) {
            if (!connection->dead() && connection->timed_out(now, timeout)) {
                application::log_line("[SERVER] Transfer failed: idle timeout");
                connection->close(loop);
            }
        }
    };
#if !defined(__linux__)
    std::mutex timeout_mutex;
    std::condition_variable timeout_condition;
    bool timeout_stop = false;
    std::thread timeout_thread([&] {
        std::unique_lock lock(timeout_mutex);
        while (!timeout_stop) {
            timeout_condition.wait_for(lock, std::chrono::milliseconds(idle_timeout_ms_));
            if (!timeout_stop) listener.notify();
        }
    });
#endif

    application::log_parts("[SERVER] Listening on ", listener.local_endpoint());
    loop.add(listener.fd(), application::event_read, [&](std::uint32_t) {
        for (;;) {
            try {
                auto incoming = listener.accept_nonblocking();
                if (!incoming) break;
                const std::string peer = incoming->peer;
                auto connection = std::make_shared<ServerConnection>(std::move(*incoming), store);
                application::log_parts("[SERVER] Client connected: ", peer);
                const int descriptor = connection->fd();
                std::weak_ptr<ServerConnection> weak = connection;
                loop.add(descriptor, application::event_read | application::event_error | application::event_hangup,
                         [weak, &loop](std::uint32_t events) {
                             if (const auto locked = weak.lock()) locked->on_event(loop, events);
                         });
                connections.push_back(std::move(connection));
            } catch (const std::exception& error) {
                application::log_parts("[SERVER] Accept failed: ", error.what());
                break;
            }
        }
    });
    const int wake_fd = signal_waiter.fd() >= 0 ? signal_waiter.fd() : listener.wake_fd();
    loop.add(wake_fd, application::event_read, [&](std::uint32_t) {
#if defined(__linux__)
        if (signal_waiter.consume()) {
            stopping.store(true, std::memory_order_release);
            loop.stop();
            return;
        }
#endif
        std::array<unsigned char, 64> bytes{};
        while (::read(listener.wake_fd(), bytes.data(), bytes.size()) > 0) {}
    check_timeouts();
        if (stopping.load(std::memory_order_acquire)) loop.stop();
    });
#if defined(__linux__)
    const int timer = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (timer < 0) throw std::system_error(errno, std::generic_category(), "create timeout timer");
    itimerspec timer_spec{};
    const std::uint64_t interval_ms = std::max<std::uint64_t>(1U, std::min<std::uint64_t>(idle_timeout_ms_, 1000U));
    timer_spec.it_value.tv_sec = static_cast<time_t>(interval_ms / 1000U);
    timer_spec.it_value.tv_nsec = static_cast<long>((interval_ms % 1000U) * 1000000U);
    timer_spec.it_interval = timer_spec.it_value;
    if (::timerfd_settime(timer, 0, &timer_spec, nullptr) < 0) {
        const int error = errno;
        ::close(timer);
        throw std::system_error(error, std::generic_category(), "arm timeout timer");
    }
    loop.add(timer, application::event_read, [&](std::uint32_t) {
        std::uint64_t expirations = 0;
        (void)::read(timer, &expirations, sizeof(expirations));
        check_timeouts();
    });
#endif
    loop.run();

    for (const auto& connection : connections) connection->close(loop);
#if !defined(__linux__)
    {
        std::lock_guard lock(timeout_mutex);
        timeout_stop = true;
    }
    timeout_condition.notify_one();
    timeout_thread.join();
#endif
#if defined(__linux__)
    loop.remove(timer);
    ::close(timer);
#endif
    application::log_line("[SERVER] Stopped");
}

} // namespace file_transfer

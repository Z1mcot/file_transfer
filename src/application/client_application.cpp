#include "file_transfer/application/client_application.hpp"

#include "file_transfer/application/event_loop.hpp"
#include "file_transfer/application/logging.hpp"
#include "file_transfer/application/unique_fd.hpp"
#include "file_transfer/checksum/crc32.hpp"
#include "file_transfer/protocol/frame_parser.hpp"
#include "file_transfer/transport/nonblocking_transport.hpp"
#include "file_transfer/transport/tcp_connector.hpp"
#include "file_transfer/transfer/transfer.hpp"
#include "file_transfer/utils/byte_utils.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace file_transfer
{
    // PImpl of the client application, which manages multiple concurrent file transfers to a server.
    namespace
    {

        inline constexpr std::size_t DISPATCH_BUDGET = 256U * 1024U;

        namespace bu = byte_utils;

        std::vector<std::byte> frame(protocol::MessageType type, std::span<const std::byte> payload)
        {
            std::vector<std::byte> result;
            result.reserve(protocol::FRAME_HEADER_SIZE + payload.size());

            bu::append_u32(result, protocol::MAGIC_NUM);
            bu::append_u16(result, protocol::VERSION);
            bu::append_u16(result, static_cast<std::uint16_t>(type));
            bu::append_u32(result, static_cast<std::uint32_t>(payload.size()));

            result.insert(result.end(), payload.begin(), payload.end());
            return result;
        }

        void pread_exact(int descriptor, std::span<std::byte> buffer, std::uint64_t offset)
        {
            std::size_t completed = 0U;
            while (completed < buffer.size())
            {
                const ssize_t count = ::pread(descriptor, buffer.data() + completed, buffer.size() - completed,
                                              static_cast<off_t>(offset + completed));

                if (count > 0)
                {
                    completed += static_cast<std::size_t>(count);
                    continue;
                }
                if (count < 0 && errno == EINTR)
                    continue;
                if (count == 0)
                    throw std::runtime_error("input file ended before its declared size");

                throw std::system_error(errno, std::generic_category(), "read input file");
            }
        }

        class ClientTransfer final : public std::enable_shared_from_this<ClientTransfer>
        {
        public:
            ClientTransfer(std::filesystem::path path, const std::string &host, std::uint16_t port)
                : path_(std::move(path))
            {
                const int descriptor = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);

                if (descriptor < 0)
                    throw std::system_error(errno, std::generic_category(), "open input file");

                input_ = std::make_unique<application::UniqueFd>(descriptor);
                metadata_ = calculate_file_metadata(input_->get());

                PendingTcpConnection connection = connect_tcp_nonblocking(host, port);
                transport_ = std::move(connection.transport);
                stream_ = transport_.get();

                state_ = connection.connected ? State::sending_hello : State::connecting;
                if (connection.connected)
                    queue_hello();
            }

            [[nodiscard]] int fd() const noexcept { return stream_->fd(); }
            [[nodiscard]] bool done() const noexcept { return done_; }
            [[nodiscard]] bool failed() const noexcept { return failed_; }
            [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

            void on_event(application::EventLoop &loop, std::uint32_t events)
            {
                if (state_ == State::connecting && (events & application::event_write) != 0U)
                {
                    int error = 0;
                    socklen_t size = sizeof(error);

                    if (::getsockopt(fd(), SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error != 0)
                    {
                        fail(error == 0 ? "could not inspect connection" : std::system_category().message(error));
                        return;
                    }

                    state_ = State::sending_hello;
                    queue_hello();
                }

                if (done_)
                    return;

                if ((events & application::event_write) != 0U && state_ != State::waiting_result)
                    send_available(loop);

                if (!done_ && (events & application::event_read) != 0U)
                    read_result();

                if (!done_ && ((events & application::event_error) != 0U || (events & application::event_hangup) != 0U))
                {
                    fail("connection closed");
                }
            }

        private:
            enum class State
            {
                connecting,
                sending_hello,
                sending_data,
                sending_finish,
                waiting_result
            };

            void queue_hello()
            {
                output_ = frame(protocol::MessageType::hello, protocol::encode_hello(
                                                                  {metadata_.size, metadata_.crc32, protocol::MAXIMUM_CHUNK_SIZE}));

                output_offset_ = 0U;
            }

            void queue_next_data_or_finish()
            {
                if (offset_ < metadata_.size)
                {
                    const std::size_t count = static_cast<std::size_t>(
                        std::min<std::uint64_t>(protocol::MAXIMUM_CHUNK_SIZE, metadata_.size - offset_));

                    std::vector<std::byte> bytes(count);
                    pread_exact(input_->get(), bytes, offset_);

                    output_ = frame(protocol::MessageType::data,
                                    protocol::encode_data(sequence_, crc32(bytes), bytes));
                    output_offset_ = 0U;
                    state_ = State::sending_data;

                    return;
                }
                output_ = frame(protocol::MessageType::finish,
                                protocol::encode_finish({metadata_.size, sequence_, metadata_.crc32}));

                output_offset_ = 0U;
                state_ = State::sending_finish;
            }

            void send_available(application::EventLoop &loop)
            {
                std::size_t budget = DISPATCH_BUDGET;
                while (budget > 0U && output_offset_ < output_.size())
                {
                    const NonBlockingResult result = stream_->send_nonblocking(
                        std::span<const std::byte>(output_.data() + output_offset_, output_.size() - output_offset_));

                    if (result.status == NonBlockingStatus::progress)
                    {
                        output_offset_ += result.count;
                        budget -= std::min(budget, result.count);
                        continue;
                    }

                    if (result.status == NonBlockingStatus::would_block)
                        return;

                    fail(result.error == 0 ? "socket write failed" : std::system_category().message(result.error));
                    return;
                }

                if (output_offset_ != output_.size())
                    return;

                if (state_ == State::sending_hello)
                {
                    queue_next_data_or_finish();
                }
                else if (state_ == State::sending_data)
                {
                    offset_ += static_cast<std::uint64_t>(
                        std::min<std::uint64_t>(protocol::MAXIMUM_CHUNK_SIZE, metadata_.size - offset_));
                    ++sequence_;
                    queue_next_data_or_finish();
                }
                else if (state_ == State::sending_finish)
                {
                    state_ = State::waiting_result;
                    loop.modify(fd(), application::event_read | application::event_error | application::event_hangup);
                    return;
                }

                if (!output_.empty())
                    send_available(loop);
            }

            void read_result()
            {
                std::array<std::byte, protocol::MAXIMUM_CHUNK_SIZE> buffer{};
                for (;;)
                {
                    const NonBlockingResult result = stream_->recv_nonblocking(buffer);

                    if (result.status == NonBlockingStatus::progress)
                    {
                        try
                        {
                            parser_.feed(std::span<const std::byte>(buffer.data(), result.count));
                            if (parser_.has_message())
                            {
                                const protocol::Message message = parser_.pop_message();
                                if (message.type != protocol::MessageType::result)
                                    throw std::runtime_error("unexpected server response");
                                
                                const protocol::Result response = protocol::decode_result(message.payload);
                                
                                if (response.code != 0U)
                                    fail(response.message);
                                else {
                                    done_ = true;
                                    application::log_parts("[CLIENT] Transfer completed successfully: ", path_.string());
                                }
                                
                                return;
                            }
                        }
                        catch (const std::exception &error)
                        {
                            fail(error.what());
                            return;
                        }
                        continue;
                    }

                    if (result.status == NonBlockingStatus::would_block)
                        return;

                    fail(result.status == NonBlockingStatus::eof ? "unexpected EOF" : "socket read failed");
                    return;
                }
            }

            void fail(const std::string &message)
            {
                if (done_)
                    return;
                failed_ = true;
                done_ = true;
                error_ = message;
            }

            std::filesystem::path path_;

            std::unique_ptr<application::UniqueFd> input_;
            FileMetadata metadata_{};

            std::unique_ptr<TcpTransport> transport_;
            INonBlockingTransport *stream_ = nullptr;

            protocol::FrameParser parser_;
            std::vector<std::byte> output_;

            std::size_t output_offset_ = 0U;
            std::uint64_t offset_ = 0U;
            std::uint64_t sequence_ = 0U;

            std::string error_;

            State state_ = State::connecting;

            bool done_ = false;
            bool failed_ = false;
        };

        class ClientScheduler final : public std::enable_shared_from_this<ClientScheduler>
        {
        public:
            ClientScheduler(application::EventLoop &loop, ClientOptions options)
                : loop_(loop), options_(std::move(options))
            {
                max_active_ = options_.max_active;

                if (max_active_ == 0U)
                {
                    struct rlimit limits{};
                    const std::size_t available = ::getrlimit(RLIMIT_NOFILE, &limits) == 0
                                                      ? static_cast<std::size_t>(limits.rlim_cur > 64U ? limits.rlim_cur - 64U : 1U) / 2U
                                                      : 16U;

                    max_active_ = std::max<std::size_t>(1U, std::min(options_.files.size(), available));
                }

                max_active_ = std::max<std::size_t>(1U, std::min(max_active_, options_.files.size()));
            }

            void start() { start_available(); }

            void on_event(int descriptor, std::uint32_t events)
            {
                const auto iterator = active_.find(descriptor);
                if (iterator == active_.end())
                    return;

                const auto transfer = iterator->second;
                transfer->on_event(loop_, events);

                if (transfer->done())
                {
                    loop_.remove(descriptor);

                    if (transfer->failed())
                    {
                        ++failed_;
                        application::log_parts("[CLIENT] Transfer failed: ", transfer->path().string());
                    }

                    active_.erase(iterator);
                    start_available();
                }

                if (active_.empty() && next_ == options_.files.size())
                    loop_.stop();
            }

            [[nodiscard]] std::size_t failed() const noexcept { return failed_; }

        private:
            void start_available()
            {
                while (active_.size() < max_active_ && next_ < options_.files.size())
                {
                    const std::filesystem::path path = options_.files[next_++];

                    try
                    {
                        auto transfer = std::make_shared<ClientTransfer>(path, options_.host, options_.port);
                        const int descriptor = transfer->fd();

                        std::weak_ptr<ClientScheduler> weak_scheduler = shared_from_this();
                        std::weak_ptr<ClientTransfer> weak_transfer = transfer;

                        active_[descriptor] = transfer;

                        loop_.add(descriptor, application::event_read | application::event_write | application::event_error | application::event_hangup,
                                  [weak_scheduler, weak_transfer, descriptor](std::uint32_t events)
                                  {
                                      if (weak_transfer.lock() && weak_scheduler.lock())
                                      {
                                          weak_scheduler.lock()->on_event(descriptor, events);
                                      }
                                  });

                        application::log_parts("[CLIENT] Transfer started: ", path.string(), " (", transfer->path().string(), ")");
                    }
                    catch (const std::exception &error)
                    {
                        ++failed_;
                        application::log_parts("[CLIENT] Transfer failed: ", path.string(), ": ", error.what());
                    }
                }

                if (active_.empty() && next_ == options_.files.size())
                    loop_.stop();
            }

            application::EventLoop &loop_;
            ClientOptions options_;

            std::unordered_map<int, std::shared_ptr<ClientTransfer>> active_;

            std::size_t max_active_ = 1U;
            std::size_t next_ = 0U;
            std::size_t failed_ = 0U;
        };

    } // namespace

    ClientApplication::ClientApplication(ClientOptions options) : options_(std::move(options)) {}

    void ClientApplication::run()
    {
        application::EventLoop loop;
        auto scheduler = std::make_shared<ClientScheduler>(loop, options_);
        scheduler->start();
        loop.run();
        if (scheduler->failed() != 0U)
            throw std::runtime_error("one or more transfers failed");
    }

} // namespace file_transfer

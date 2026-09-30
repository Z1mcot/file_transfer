#include "file_transfer/tcp_transport.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace file_transfer {
namespace {

[[noreturn]] void throw_socket_error(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

void set_close_on_exec(int descriptor) {
    const int flags = ::fcntl(descriptor, F_GETFD);
    if (flags < 0 || ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) < 0) {
        throw_socket_error("fcntl(FD_CLOEXEC)");
    }
}

void set_receive_timeout(int descriptor) {
    const timeval timeout{30, 0};
    if (::setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        throw_socket_error("setsockopt(SO_RCVTIMEO)");
    }
}

std::string numeric_address(const sockaddr* address, socklen_t length) {
    std::array<char, NI_MAXHOST> host{};
    std::array<char, NI_MAXSERV> service{};
    const int result = ::getnameinfo(address, length, host.data(), static_cast<socklen_t>(host.size()),
                                     service.data(), static_cast<socklen_t>(service.size()),
                                     NI_NUMERICHOST | NI_NUMERICSERV);
    if (result != 0) {
        throw std::runtime_error(::gai_strerror(result));
    }
    return std::string(host.data()) + ":" + service.data();
}

} // namespace

TcpTransport::TcpTransport(int descriptor) : descriptor_(descriptor) {
    set_receive_timeout(descriptor_);
}

TcpTransport::~TcpTransport() {
    if (descriptor_ >= 0) {
        ::close(descriptor_);
    }
}

std::size_t TcpTransport::read_some(std::span<std::byte> buffer) {
    for (;;) {
        const ssize_t count = ::recv(descriptor_, buffer.data(), buffer.size(), 0);
        if (count >= 0) {
            return static_cast<std::size_t>(count);
        }
        if (errno != EINTR) {
            throw_socket_error("recv");
        }
    }
}

std::size_t TcpTransport::write_some(std::span<const std::byte> buffer) {
    for (;;) {
        const ssize_t count = ::send(descriptor_, buffer.data(), buffer.size(), MSG_NOSIGNAL);
        if (count >= 0) {
            return static_cast<std::size_t>(count);
        }
        if (errno != EINTR) {
            throw_socket_error("send");
        }
    }
}

void TcpTransport::cancel() noexcept {
    if (descriptor_ >= 0) {
        ::shutdown(descriptor_, SHUT_RDWR);
    }
}

TcpListener::TcpListener(std::uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    const std::string service = std::to_string(port);
    addrinfo* addresses = nullptr;
    const int lookup = ::getaddrinfo(nullptr, service.c_str(), &hints, &addresses);
    if (lookup != 0) {
        throw std::runtime_error(::gai_strerror(lookup));
    }

    int last_error = EADDRNOTAVAIL;
    for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        const int descriptor = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (descriptor < 0) {
            last_error = errno;
            continue;
        }
        try {
            set_close_on_exec(descriptor);
        } catch (...) {
            ::close(descriptor);
            ::freeaddrinfo(addresses);
            throw;
        }
        const int enabled = 1;
        if (::setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0 ||
            ::bind(descriptor, address->ai_addr, address->ai_addrlen) < 0 ||
            ::listen(descriptor, SOMAXCONN) < 0) {
            last_error = errno;
            ::close(descriptor);
            continue;
        }

        sockaddr_in bound{};
        socklen_t bound_size = sizeof(bound);
        if (::getsockname(descriptor, reinterpret_cast<sockaddr*>(&bound), &bound_size) < 0) {
            last_error = errno;
            ::close(descriptor);
            continue;
        }
        descriptor_ = descriptor;
        port_ = ntohs(bound.sin_port);
        break;
    }
    ::freeaddrinfo(addresses);
    if (descriptor_ < 0) {
        throw std::system_error(last_error, std::generic_category(), "create TCP listener");
    }
    int cancellation_pipe[2]{};
    if (::pipe2(cancellation_pipe, O_CLOEXEC | O_NONBLOCK) < 0) {
        const int error = errno;
        ::close(descriptor_);
        descriptor_ = -1;
        throw std::system_error(error, std::generic_category(), "create listener cancellation pipe");
    }
    cancel_read_ = cancellation_pipe[0];
    cancel_write_ = cancellation_pipe[1];
}

TcpListener::~TcpListener() {
    if (descriptor_ >= 0) {
        ::close(descriptor_);
    }
    if (cancel_read_ >= 0) {
        ::close(cancel_read_);
    }
    if (cancel_write_ >= 0) {
        ::close(cancel_write_);
    }
}

std::optional<AcceptedConnection> TcpListener::accept() {
    std::array<pollfd, 2> descriptors{{
        {descriptor_, POLLIN, 0},
        {cancel_read_, POLLIN, 0},
    }};
    for (;;) {
        const int ready = ::poll(descriptors.data(), descriptors.size(), -1);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready < 0) {
            throw_socket_error("poll listener");
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            bool cancelled = false;
            std::array<unsigned char, 64> events{};
            for (;;) {
                const ssize_t count = ::read(cancel_read_, events.data(), events.size());
                if (count > 0) {
                    for (ssize_t index = 0; index < count; ++index) {
                        cancelled = cancelled || events[static_cast<std::size_t>(index)] == 1U;
                    }
                    continue;
                }
                if (count < 0 && errno == EINTR) {
                    continue;
                }
                break;
            }
            if (cancelled) {
                return std::nullopt;
            }
            return std::nullopt;
        }
        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            throw std::runtime_error("listener socket failed");
        }
        if ((descriptors[0].revents & POLLIN) != 0) {
            break;
        }
    }

    sockaddr_storage peer{};
    socklen_t peer_size = sizeof(peer);
    const int client = ::accept(descriptor_, reinterpret_cast<sockaddr*>(&peer), &peer_size);
    if (client < 0) {
        throw_socket_error("accept");
    }
    try {
        set_close_on_exec(client);
        std::string peer_name = numeric_address(reinterpret_cast<const sockaddr*>(&peer), peer_size);
        return AcceptedConnection{std::make_unique<TcpTransport>(client), std::move(peer_name)};
    } catch (...) {
        ::close(client);
        throw;
    }
}

void TcpListener::cancel() noexcept {
    if (cancel_write_ < 0) {
        return;
    }
    const unsigned char notification = 1U;
    for (;;) {
        const ssize_t result = ::write(cancel_write_, &notification, sizeof(notification));
        if (result >= 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        if (errno != EINTR) {
            return;
        }
    }
}

void TcpListener::notify() noexcept {
    if (cancel_write_ < 0) {
        return;
    }
    const unsigned char notification = 2U;
    for (;;) {
        const ssize_t result = ::write(cancel_write_, &notification, sizeof(notification));
        if (result >= 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        if (errno != EINTR) {
            return;
        }
    }
}

std::string TcpListener::local_endpoint() const {
    return "0.0.0.0:" + std::to_string(port_);
}

std::unique_ptr<ITransport> connect_tcp(const std::string& host, std::uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    const std::string service = std::to_string(port);
    addrinfo* addresses = nullptr;
    const int lookup = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
    if (lookup != 0) {
        throw std::runtime_error(std::string("resolve host: ") + ::gai_strerror(lookup));
    }

    int last_error = ECONNREFUSED;
    for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        const int descriptor = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (descriptor < 0) {
            last_error = errno;
            continue;
        }
        if (::connect(descriptor, address->ai_addr, address->ai_addrlen) == 0) {
            ::freeaddrinfo(addresses);
            try {
                set_close_on_exec(descriptor);
                return std::make_unique<TcpTransport>(descriptor);
            } catch (...) {
                ::close(descriptor);
                throw;
            }
        }
        last_error = errno;
        ::close(descriptor);
    }
    ::freeaddrinfo(addresses);
    throw std::system_error(last_error, std::generic_category(), "connect");
}

} // namespace file_transfer
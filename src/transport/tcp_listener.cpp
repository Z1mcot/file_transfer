#include "file_transfer/transport/tcp_listener.hpp"

#include "file_transfer/transport/tcp_transport.hpp"
#include "file_transfer/transport/detail/tcp_socket_utils.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <system_error>
#include <utility>

namespace file_transfer {

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
            detail::set_close_on_exec(descriptor);
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
            detail::throw_socket_error("poll listener");
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            std::array<unsigned char, 64> events{};
            for (;;) {
                const ssize_t count = ::read(cancel_read_, events.data(), events.size());
                if (count > 0) {
                    continue;
                }
                if (count < 0 && errno == EINTR) {
                    continue;
                }
                break;
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
        detail::throw_socket_error("accept");
    }
    try {
        detail::set_close_on_exec(client);
        std::string peer_name = detail::numeric_address(reinterpret_cast<const sockaddr*>(&peer), peer_size);
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

} // namespace file_transfer
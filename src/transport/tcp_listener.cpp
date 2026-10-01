#include "file_transfer/transport/tcp_listener.hpp"

#include "file_transfer/transport/tcp_transport.hpp"
#include "file_transfer/transport/detail/tcp_socket_utils.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

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
        const int flags = ::fcntl(descriptor_, F_GETFL, 0);
        if (flags < 0 || ::fcntl(descriptor_, F_SETFL, flags | O_NONBLOCK) < 0) {
            last_error = errno;
            ::close(descriptor_);
            descriptor_ = -1;
            continue;
        }
        
        port_ = ntohs(bound.sin_port);
        break;
    }

    ::freeaddrinfo(addresses);
    if (descriptor_ < 0) {
        throw std::system_error(last_error, std::generic_category(), "create TCP listener");
    }

}

TcpListener::~TcpListener() {
    if (descriptor_ >= 0) {
        ::close(descriptor_);
    }
}

std::optional<AcceptedConnection> TcpListener::accept_nonblocking() {
    sockaddr_storage peer{};
    socklen_t peer_size = sizeof(peer);
    const int client = ::accept(descriptor_, reinterpret_cast<sockaddr*>(&peer), &peer_size);
    if (client < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return std::nullopt;
        }
        detail::throw_socket_error("accept");
    }

    try {
        detail::set_close_on_exec(client);
        const int flags = ::fcntl(client, F_GETFL, 0);
        if (flags < 0 || ::fcntl(client, F_SETFL, flags | O_NONBLOCK) < 0) {
            throw std::system_error(errno, std::generic_category(), "set accepted socket non-blocking");
        }
        
        std::string peer_name = detail::numeric_address(reinterpret_cast<const sockaddr*>(&peer), peer_size);
        return AcceptedConnection{std::make_unique<TcpTransport>(client, true), std::move(peer_name)};
    } catch (...) {
        ::close(client);
        throw;
    }
}

int TcpListener::fd() const noexcept { return descriptor_; }

std::string TcpListener::local_endpoint() const {
    return "0.0.0.0:" + std::to_string(port_);
}

} // namespace file_transfer
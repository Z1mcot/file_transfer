#include "file_transfer/transport/tcp_transport.hpp"

#include "file_transfer/transport/detail/tcp_socket_utils.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

namespace file_transfer {

TcpTransport::TcpTransport(int descriptor) : descriptor_(descriptor) {
    detail::set_receive_timeout(descriptor_);
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
            detail::throw_socket_error("recv");
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
            detail::throw_socket_error("send");
        }
    }
}

void TcpTransport::cancel() noexcept {
    if (descriptor_ >= 0) {
        ::shutdown(descriptor_, SHUT_RDWR);
    }
}

} // namespace file_transfer
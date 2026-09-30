#include "file_transfer/transport/tcp_transport.hpp"

#include "file_transfer/transport/detail/tcp_socket_utils.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <fcntl.h>

namespace file_transfer {

TcpTransport::TcpTransport(int descriptor, bool nonblocking) : descriptor_(descriptor) {
    if (nonblocking) {
        const int flags = ::fcntl(descriptor_, F_GETFL, 0);
        if (flags < 0 || ::fcntl(descriptor_, F_SETFL, flags | O_NONBLOCK) < 0) {
            const int error = errno;
            ::close(descriptor_);
            throw std::system_error(error, std::generic_category(), "set socket non-blocking");
        }
    } else {
        detail::set_receive_timeout(descriptor_);
    }
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

int TcpTransport::fd() const noexcept { return descriptor_; }

NonBlockingResult TcpTransport::recv_nonblocking(std::span<std::byte> buffer) noexcept {
    for (;;) {
        const ssize_t count = ::recv(descriptor_, buffer.data(), buffer.size(), 0);
        if (count > 0) return {NonBlockingStatus::progress, static_cast<std::size_t>(count), 0};
        if (count == 0) return {NonBlockingStatus::eof, 0, 0};
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return {NonBlockingStatus::would_block, 0, errno};
        return {NonBlockingStatus::error, 0, errno};
    }
}

NonBlockingResult TcpTransport::send_nonblocking(std::span<const std::byte> buffer) noexcept {
    for (;;) {
        const ssize_t count = ::send(descriptor_, buffer.data(), buffer.size(), MSG_NOSIGNAL);
        if (count > 0) return {NonBlockingStatus::progress, static_cast<std::size_t>(count), 0};
        if (count == 0) return {NonBlockingStatus::eof, 0, 0};
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return {NonBlockingStatus::would_block, 0, errno};
        return {NonBlockingStatus::error, 0, errno};
    }
}

} // namespace file_transfer
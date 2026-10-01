#pragma once

#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <array>
#include <stdexcept>
#include <string>
#include <system_error>

namespace file_transfer::detail {

[[noreturn]] inline void throw_socket_error(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

inline void set_close_on_exec(int descriptor) {
    const int flags = ::fcntl(descriptor, F_GETFD);

    if (flags < 0 || ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) < 0) {
        throw_socket_error("fcntl(FD_CLOEXEC)");
    }
}

inline void set_receive_timeout(int descriptor) {
    const timeval timeout{30, 0};
    
    if (::setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        throw_socket_error("setsockopt(SO_RCVTIMEO)");
    }
}

inline std::string numeric_address(const sockaddr* address, socklen_t length) {
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

} // namespace file_transfer::detail
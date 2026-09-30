#include "file_transfer/tcp_connector.hpp"

#include "file_transfer/tcp_transport.hpp"
#include "tcp_socket_utils.hpp"

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace file_transfer {

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
                detail::set_close_on_exec(descriptor);
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
#pragma once

#include "file_transfer/transport/transport.hpp"
#include "file_transfer/transport/tcp_transport.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace file_transfer {

[[nodiscard]] std::unique_ptr<ITransport> connect_tcp(const std::string& host, std::uint16_t port);

struct PendingTcpConnection {
	std::unique_ptr<TcpTransport> transport;
	bool connected = false;
};

[[nodiscard]] PendingTcpConnection connect_tcp_nonblocking(const std::string& host, std::uint16_t port);

} // namespace file_transfer
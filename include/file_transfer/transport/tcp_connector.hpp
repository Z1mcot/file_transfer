#pragma once

#include "file_transfer/transport/transport.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace file_transfer {

[[nodiscard]] std::unique_ptr<ITransport> connect_tcp(const std::string& host, std::uint16_t port);

} // namespace file_transfer
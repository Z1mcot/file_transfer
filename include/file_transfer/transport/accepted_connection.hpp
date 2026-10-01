#pragma once

#include "file_transfer/transport/nonblocking_transport.hpp"

#include <memory>
#include <string>

namespace file_transfer {

struct AcceptedConnection {
    std::unique_ptr<INonBlockingTransport> transport;
    std::string peer;
};

} // namespace file_transfer
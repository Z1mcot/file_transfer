#pragma once

#include "file_transfer/transport/transport.hpp"

#include <memory>
#include <string>

namespace file_transfer {

struct AcceptedConnection {
    std::unique_ptr<ITransport> transport;
    std::string peer;
};

} // namespace file_transfer
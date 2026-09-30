#pragma once

#include "file_transfer/accepted_connection.hpp"

#include <optional>
#include <string>

namespace file_transfer {

class ITransportListener {
public:
    virtual ~ITransportListener() = default;
    virtual std::optional<AcceptedConnection> accept() = 0;
    virtual void cancel() noexcept = 0;
    virtual void notify() noexcept = 0;
    [[nodiscard]] virtual std::string local_endpoint() const = 0;
};

} // namespace file_transfer
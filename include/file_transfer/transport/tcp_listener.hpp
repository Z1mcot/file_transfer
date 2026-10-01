#pragma once

#include "file_transfer/transport/accepted_connection.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace file_transfer {

class TcpListener final {
public:
    explicit TcpListener(std::uint16_t port);
    ~TcpListener();
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    std::optional<AcceptedConnection> accept_nonblocking();

    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] std::string local_endpoint() const;

private:
    int descriptor_ = -1;
    std::uint16_t port_ = 0;
};

} // namespace file_transfer
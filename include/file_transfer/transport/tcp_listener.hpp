#pragma once

#include "file_transfer/transport/i_transport_listener.hpp"

#include <cstdint>

namespace file_transfer {

class TcpListener final : public ITransportListener {
public:
    explicit TcpListener(std::uint16_t port);
    ~TcpListener() override;
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    std::optional<AcceptedConnection> accept() override;
    void cancel() noexcept override;
    void notify() noexcept override;
    [[nodiscard]] std::string local_endpoint() const override;

private:
    int descriptor_ = -1;
    int cancel_read_ = -1;
    int cancel_write_ = -1;
    std::uint16_t port_ = 0;
};

} // namespace file_transfer
#pragma once

#include "file_transfer/transport/nonblocking_transport.hpp"

namespace file_transfer {

class TcpTransport final : public INonBlockingTransport {
public:
    explicit TcpTransport(int descriptor, bool nonblocking = false);
    ~TcpTransport() override;
    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    void cancel() noexcept override;
    [[nodiscard]] int fd() const noexcept override;
    
    NonBlockingResult recv_nonblocking(std::span<std::byte> buffer) noexcept override;
    NonBlockingResult send_nonblocking(std::span<const std::byte> buffer) noexcept override;

    void shutdown_write() noexcept override;

private:
    int descriptor_;
};

} // namespace file_transfer
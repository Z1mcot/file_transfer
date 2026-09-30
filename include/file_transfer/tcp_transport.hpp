#pragma once

#include "file_transfer/transport.hpp"

namespace file_transfer {

class TcpTransport final : public ITransport {
public:
    explicit TcpTransport(int descriptor);
    ~TcpTransport() override;
    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    std::size_t read_some(std::span<std::byte> buffer) override;
    std::size_t write_some(std::span<const std::byte> buffer) override;
    void cancel() noexcept override;

private:
    int descriptor_;
};

} // namespace file_transfer
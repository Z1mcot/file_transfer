#pragma once

#include "file_transfer/transport.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace file_transfer {

struct AcceptedConnection {
    std::unique_ptr<ITransport> transport;
    std::string peer;
};

class ITransportListener {
public:
    virtual ~ITransportListener() = default;
    virtual std::optional<AcceptedConnection> accept() = 0;
    virtual void cancel() noexcept = 0;
    virtual void notify() noexcept = 0;
    [[nodiscard]] virtual std::string local_endpoint() const = 0;
};

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

[[nodiscard]] std::unique_ptr<ITransport> connect_tcp(const std::string& host, std::uint16_t port);

} // namespace file_transfer
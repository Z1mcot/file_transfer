#pragma once

#include <cstddef>
#include <span>

namespace file_transfer {

enum class NonBlockingStatus { progress, would_block, eof, error };

struct NonBlockingResult {
    NonBlockingStatus status;
    std::size_t count = 0;
    int error = 0;
};

class INonBlockingTransport {
public:
    virtual ~INonBlockingTransport() = default;
    [[nodiscard]] virtual int fd() const noexcept = 0;

    virtual void shutdown_write() noexcept {}

    virtual void cancel() noexcept = 0;
    
    virtual NonBlockingResult recv_nonblocking(std::span<std::byte> buffer) noexcept = 0;
    virtual NonBlockingResult send_nonblocking(std::span<const std::byte> buffer) noexcept = 0;
};

} // namespace file_transfer
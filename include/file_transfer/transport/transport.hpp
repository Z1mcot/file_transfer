#pragma once

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>

namespace file_transfer {

class ITransport {
public:
    virtual ~ITransport() = default;

    virtual std::size_t read_some(std::span<std::byte> buffer) = 0;
    virtual std::size_t write_some(std::span<const std::byte> buffer) = 0;
    
    virtual void cancel() noexcept = 0;
};

inline void read_exact(ITransport& transport, std::span<std::byte> buffer) {
    std::size_t offset = 0;

    while (offset < buffer.size()) {
        const std::size_t count = transport.read_some(buffer.subspan(offset));
    
        if (count == 0) {
            throw std::runtime_error("unexpected EOF");
        }
    
        if (count > buffer.size() - offset) {
            throw std::runtime_error("transport returned an invalid read size");
        }
    
        offset += count;
    }
}

inline void write_all(ITransport& transport, std::span<const std::byte> buffer) {
    std::size_t offset = 0;
    while (offset < buffer.size()) {
        const std::size_t count = transport.write_some(buffer.subspan(offset));
        
        if (count == 0 || count > buffer.size() - offset) {
            throw std::runtime_error("transport failed to write all bytes");
        }
        
        offset += count;
    }
}

} // namespace file_transfer
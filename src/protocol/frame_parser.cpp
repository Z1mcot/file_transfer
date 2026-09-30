#include "file_transfer/protocol/frame_parser.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace file_transfer::protocol {
namespace {

constexpr std::uint32_t magic = 0x4654524EU;
constexpr std::size_t maximum_frame_payload = maximum_chunk_size + 16U;

std::uint16_t read_u16(const std::byte* bytes) {
    return static_cast<std::uint16_t>((std::to_integer<std::uint16_t>(bytes[0]) << 8U) |
                                      std::to_integer<std::uint16_t>(bytes[1]));
}

std::uint32_t read_u32(const std::byte* bytes) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4U; ++index) {
        value = (value << 8U) | std::to_integer<std::uint8_t>(bytes[index]);
    }
    return value;
}

} // namespace

void FrameParser::feed(std::span<const std::byte> bytes) {
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    while (buffer_.size() >= frame_header_size) {
        const auto* header = buffer_.data();
        if (read_u32(header) != magic) {
            throw std::runtime_error("invalid protocol magic");
        }
        if (read_u16(header + 4U) != version) {
            throw std::runtime_error("unsupported protocol version");
        }
        const std::uint16_t raw_type = read_u16(header + 6U);
        if (raw_type < static_cast<std::uint16_t>(MessageType::hello) ||
            raw_type > static_cast<std::uint16_t>(MessageType::result)) {
            throw std::runtime_error("unknown message type");
        }
        const std::uint32_t payload_size = read_u32(header + 8U);
        if (payload_size > maximum_frame_payload) {
            throw std::runtime_error("message payload exceeds protocol limit");
        }
        const std::size_t frame_size = frame_header_size + static_cast<std::size_t>(payload_size);
        if (buffer_.size() < frame_size) {
            return;
        }
        Message message{static_cast<MessageType>(raw_type),
                        std::vector<std::byte>(buffer_.begin() + static_cast<std::ptrdiff_t>(frame_header_size),
                                               buffer_.begin() + static_cast<std::ptrdiff_t>(frame_size))};
        messages_.push_back(std::move(message));
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(frame_size));
    }
}

bool FrameParser::has_message() const noexcept { return !messages_.empty(); }

Message FrameParser::pop_message() {
    if (messages_.empty()) {
        throw std::runtime_error("no parsed protocol message");
    }
    Message message = std::move(messages_.front());
    messages_.erase(messages_.begin());
    return message;
}

void FrameParser::finish() {
    if (!buffer_.empty()) {
        throw std::runtime_error("unexpected EOF in protocol frame");
    }
}

} // namespace file_transfer::protocol
#include "file_transfer/protocol/frame_parser.hpp"
#include "file_transfer/utils/byte_utils.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace bu = file_transfer::byte_utils;

namespace file_transfer::protocol
{

    void FrameParser::feed(std::span<const std::byte> bytes)
    {
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
        while (buffer_.size() >= FRAME_HEADER_SIZE)
        {
            const auto *header = buffer_.data();
            if (bu::read_u32(header) != MAGIC_NUM)
                throw std::runtime_error("invalid protocol magic");
            

            if (bu::read_u16(header + 4U) != VERSION)
                throw std::runtime_error("unsupported protocol version");
            

            const std::uint16_t raw_type = bu::read_u16(header + 6U);
            if (raw_type < static_cast<std::uint16_t>(MessageType::hello) ||
                raw_type > static_cast<std::uint16_t>(MessageType::result)) {
                throw std::runtime_error("unknown message type");
            }
            

            const std::uint32_t payload_size = bu::read_u32(header + 8U);
            if (payload_size > MAXIMUM_FRAME_PAYLOAD)
                throw std::runtime_error("message payload exceeds protocol limit");

            const std::size_t frame_size = FRAME_HEADER_SIZE + static_cast<std::size_t>(payload_size);
            if (buffer_.size() < frame_size)
                return;

            Message message{static_cast<MessageType>(raw_type),
                            std::vector<std::byte>(buffer_.begin() + static_cast<std::ptrdiff_t>(FRAME_HEADER_SIZE),
                                                   buffer_.begin() + static_cast<std::ptrdiff_t>(frame_size))};

            messages_.push_back(std::move(message));
            buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(frame_size));
        }
    }

    bool FrameParser::has_message() const noexcept { return !messages_.empty(); }

    Message FrameParser::pop_message()
    {
        if (messages_.empty())
        {
            throw std::runtime_error("no parsed protocol message");
        }
        Message message = std::move(messages_.front());
        messages_.erase(messages_.begin());
        return message;
    }

    void FrameParser::finish()
    {
        if (!buffer_.empty())
        {
            throw std::runtime_error("unexpected EOF in protocol frame");
        }
    }

} // namespace file_transfer::protocol
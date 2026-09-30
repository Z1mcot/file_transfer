#include "file_transfer/protocol.hpp"

#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace file_transfer::protocol {
namespace {

constexpr std::uint32_t magic = 0x4654524EU;
constexpr std::size_t maximum_frame_payload = maximum_chunk_size + 16U;
constexpr std::size_t maximum_result_message = 512U;

void append_u16(std::vector<std::byte>& output, std::uint16_t value) {
    output.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
    output.push_back(static_cast<std::byte>(value & 0xFFU));
}

void append_u32(std::vector<std::byte>& output, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::byte>((value >> static_cast<unsigned>(shift)) & 0xFFU));
    }
}

void append_u64(std::vector<std::byte>& output, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::byte>((value >> static_cast<unsigned>(shift)) & 0xFFU));
    }
}

std::uint16_t take_u16(std::span<const std::byte> bytes, std::size_t& offset) {
    if (bytes.size() - offset < 2U) {
        throw std::runtime_error("truncated uint16 field");
    }
    const auto high = std::to_integer<std::uint8_t>(bytes[offset]);
    const auto low = std::to_integer<std::uint8_t>(bytes[offset + 1U]);
    offset += 2U;
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(high) << 8U) | low);
}

std::uint32_t take_u32(std::span<const std::byte> bytes, std::size_t& offset) {
    if (bytes.size() - offset < 4U) {
        throw std::runtime_error("truncated uint32 field");
    }
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4U; ++index) {
        value = (value << 8U) | std::to_integer<std::uint8_t>(bytes[offset + index]);
    }
    offset += 4U;
    return value;
}

std::uint64_t take_u64(std::span<const std::byte> bytes, std::size_t& offset) {
    if (bytes.size() - offset < 8U) {
        throw std::runtime_error("truncated uint64 field");
    }
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8U; ++index) {
        value = (value << 8U) | std::to_integer<std::uint8_t>(bytes[offset + index]);
    }
    offset += 8U;
    return value;
}

void require_size(std::span<const std::byte> bytes, std::size_t expected, const char* name) {
    if (bytes.size() != expected) {
        throw std::runtime_error(std::string("invalid ") + name + " payload size");
    }
}

} // namespace

void write_message(ITransport& transport, MessageType type, std::span<const std::byte> payload) {
    if (payload.size() > maximum_frame_payload ||
        payload.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::runtime_error("message payload is too large");
    }

    std::vector<std::byte> header;
    header.reserve(frame_header_size);
    append_u32(header, magic);
    append_u16(header, version);
    append_u16(header, static_cast<std::uint16_t>(type));
    append_u32(header, static_cast<std::uint32_t>(payload.size()));
    write_all(transport, header);
    write_all(transport, payload);
}

Message read_message(ITransport& transport) {
    std::array<std::byte, frame_header_size> header{};
    read_exact(transport, header);
    std::size_t offset = 0;
    const std::uint32_t received_magic = take_u32(header, offset);
    const std::uint16_t received_version = take_u16(header, offset);
    const std::uint16_t raw_type = take_u16(header, offset);
    const std::uint32_t payload_size = take_u32(header, offset);
    if (received_magic != magic) {
        throw std::runtime_error("invalid protocol magic");
    }
    if (received_version != version) {
        throw std::runtime_error("unsupported protocol version");
    }
    if (raw_type < static_cast<std::uint16_t>(MessageType::hello) ||
        raw_type > static_cast<std::uint16_t>(MessageType::result)) {
        throw std::runtime_error("unknown message type");
    }
    if (payload_size > maximum_frame_payload) {
        throw std::runtime_error("message payload exceeds protocol limit");
    }

    Message message{static_cast<MessageType>(raw_type), std::vector<std::byte>(payload_size)};
    read_exact(transport, message.payload);
    return message;
}

std::vector<std::byte> encode_hello(const Hello& value) {
    std::vector<std::byte> bytes;
    bytes.reserve(16U);
    append_u64(bytes, value.file_size);
    append_u32(bytes, value.file_crc32);
    append_u32(bytes, value.chunk_size);
    return bytes;
}

Hello decode_hello(std::span<const std::byte> bytes) {
    require_size(bytes, 16U, "HELLO");
    std::size_t offset = 0;
    return {take_u64(bytes, offset), take_u32(bytes, offset), take_u32(bytes, offset)};
}

std::vector<std::byte> encode_data(
    std::uint64_t sequence,
    std::uint32_t checksum,
    std::span<const std::byte> data) {
    if (data.empty() || data.size() > maximum_chunk_size) {
        throw std::runtime_error("invalid DATA payload size");
    }
    std::vector<std::byte> bytes;
    bytes.reserve(16U + data.size());
    append_u64(bytes, sequence);
    append_u32(bytes, static_cast<std::uint32_t>(data.size()));
    append_u32(bytes, checksum);
    bytes.insert(bytes.end(), data.begin(), data.end());
    return bytes;
}

DataView decode_data(std::span<const std::byte> bytes) {
    if (bytes.size() < 16U) {
        throw std::runtime_error("truncated DATA payload");
    }
    std::size_t offset = 0;
    const std::uint64_t sequence = take_u64(bytes, offset);
    const std::uint32_t size = take_u32(bytes, offset);
    const std::uint32_t checksum = take_u32(bytes, offset);
    if (size == 0U || size > maximum_chunk_size || bytes.size() - offset != size) {
        throw std::runtime_error("invalid DATA block length");
    }
    return {sequence, checksum, bytes.subspan(offset)};
}

std::vector<std::byte> encode_finish(const Finish& value) {
    std::vector<std::byte> bytes;
    bytes.reserve(20U);
    append_u64(bytes, value.total_bytes);
    append_u64(bytes, value.total_chunks);
    append_u32(bytes, value.file_crc32);
    return bytes;
}

Finish decode_finish(std::span<const std::byte> bytes) {
    require_size(bytes, 20U, "FINISH");
    std::size_t offset = 0;
    return {take_u64(bytes, offset), take_u64(bytes, offset), take_u32(bytes, offset)};
}

std::vector<std::byte> encode_result(const Result& value) {
    if (value.message.size() > maximum_result_message) {
        throw std::runtime_error("RESULT message is too long");
    }
    std::vector<std::byte> bytes;
    bytes.reserve(4U + value.message.size());
    append_u16(bytes, value.code);
    append_u16(bytes, static_cast<std::uint16_t>(value.message.size()));
    for (const char character : value.message) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    return bytes;
}

Result decode_result(std::span<const std::byte> bytes) {
    if (bytes.size() < 4U) {
        throw std::runtime_error("truncated RESULT payload");
    }
    std::size_t offset = 0;
    const std::uint16_t code = take_u16(bytes, offset);
    const std::uint16_t text_size = take_u16(bytes, offset);
    if (text_size > maximum_result_message || bytes.size() - offset != text_size) {
        throw std::runtime_error("invalid RESULT message length");
    }
    std::string message;
    message.reserve(text_size);
    for (const std::byte character : bytes.subspan(offset)) {
        message.push_back(static_cast<char>(std::to_integer<unsigned char>(character)));
    }
    return {code, std::move(message)};
}

} // namespace file_transfer::protocol
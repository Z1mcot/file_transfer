#include "file_transfer/protocol/protocol.hpp"

#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

#include "file_transfer/utils/byte_utils.hpp"

namespace bu = file_transfer::byte_utils;

namespace file_transfer::protocol {

std::vector<std::byte> encode_hello(const Hello& value) {
    std::vector<std::byte> bytes;
    bytes.reserve(16U);
    bu::append_u64(bytes, value.file_size);
    bu::append_u32(bytes, value.file_crc32);
    bu::append_u32(bytes, value.chunk_size);
    return bytes;
}

Hello decode_hello(std::span<const std::byte> bytes) {
    bu::require_size(bytes, 16U, "HELLO");
    std::size_t offset = 0;
    return {bu::take_u64(bytes, offset), bu::take_u32(bytes, offset), bu::take_u32(bytes, offset)};
}

std::vector<std::byte> encode_data(
    std::uint64_t sequence,
    std::uint32_t checksum,
    std::span<const std::byte> data) {
    if (data.empty() || data.size() > MAXIMUM_CHUNK_SIZE) {
        throw std::runtime_error("invalid DATA payload size");
    }
    std::vector<std::byte> bytes;
    bytes.reserve(16U + data.size());

    bu::append_u64(bytes, sequence);
    bu::append_u32(bytes, static_cast<std::uint32_t>(data.size()));
    bu::append_u32(bytes, checksum);
    bytes.insert(bytes.end(), data.begin(), data.end());
    
    return bytes;
}

DataView decode_data(std::span<const std::byte> bytes) {
    if (bytes.size() < 16U) {
        throw std::runtime_error("truncated DATA payload");
    }
    std::size_t offset = 0;
    const std::uint64_t sequence = bu::take_u64(bytes, offset);
    const std::uint32_t size = bu::take_u32(bytes, offset);
    const std::uint32_t checksum = bu::take_u32(bytes, offset);
    if (size == 0U || size > MAXIMUM_CHUNK_SIZE || bytes.size() - offset != size) {
        throw std::runtime_error("invalid DATA block length");
    }
    return {sequence, checksum, bytes.subspan(offset)};
}

std::vector<std::byte> encode_finish(const Finish& value) {
    std::vector<std::byte> bytes;
    bytes.reserve(20U);
    bu::append_u64(bytes, value.total_bytes);
    bu::append_u64(bytes, value.total_chunks);
    bu::append_u32(bytes, value.file_crc32);
    return bytes;
}

Finish decode_finish(std::span<const std::byte> bytes) {
    bu::require_size(bytes, 20U, "FINISH");
    std::size_t offset = 0;
    return {bu::take_u64(bytes, offset), bu::take_u64(bytes, offset), bu::take_u32(bytes, offset)};
}

std::vector<std::byte> encode_result(const Result& value) {
    if (value.message.size() > MAXIMUM_RESULT_MESSAGE_SIZE) {
        throw std::runtime_error("RESULT message is too long");
    }
    
    std::vector<std::byte> bytes;
    bytes.reserve(4U + value.message.size());
    bu::append_u16(bytes, value.code);
    bu::append_u16(bytes, static_cast<std::uint16_t>(value.message.size()));
    
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
    const std::uint16_t code = bu::take_u16(bytes, offset);
    const std::uint16_t text_size = bu::take_u16(bytes, offset);
    if (text_size > MAXIMUM_RESULT_MESSAGE_SIZE || bytes.size() - offset != text_size) {
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
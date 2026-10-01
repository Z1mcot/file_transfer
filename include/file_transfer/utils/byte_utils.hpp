#pragma once

#include <vector>
#include <span>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace file_transfer::byte_utils {

inline void append_u16(std::vector<std::byte>& output, std::uint16_t value) {
    output.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
    output.push_back(static_cast<std::byte>(value & 0xFFU));
}

inline void append_u32(std::vector<std::byte>& output, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::byte>((value >> static_cast<unsigned>(shift)) & 0xFFU));
    }
}

inline void append_u64(std::vector<std::byte>& output, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::byte>((value >> static_cast<unsigned>(shift)) & 0xFFU));
    }
}

inline std::uint16_t read_u16(const std::byte *bytes, std::size_t offset = 0)
{
    const auto high = std::to_integer<std::uint8_t>(bytes[offset]);
    const auto low = std::to_integer<std::uint8_t>(bytes[offset + 1U]);

    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(high) << 8U) | low);
}

inline std::uint32_t read_u32(const std::byte *bytes, std::size_t offset = 0)
{
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4U; ++index)
        value = (value << 8U) | std::to_integer<std::uint8_t>(bytes[offset + index]);
    
    return value;
}

inline std::uint64_t read_u64(const std::byte *bytes, std::size_t offset = 0)
{
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8U; ++index) {
        value = (value << 8U) | std::to_integer<std::uint8_t>(bytes[offset + index]);
    }
    
    return value;
}

inline std::uint16_t take_u16(std::span<const std::byte> bytes, std::size_t& offset) {
    if (offset > bytes.size() || bytes.size() - offset < 2U) {
        throw std::runtime_error("truncated uint16 field");
    }

    const auto res = read_u16(bytes.data(), offset);
    
    offset += 2U;
    return res;
}

inline std::uint32_t take_u32(std::span<const std::byte> bytes, std::size_t& offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4U) {
        throw std::runtime_error("truncated uint32 field");
    }

    std::uint32_t value = read_u32(bytes.data(), offset);
    
    offset += 4U;
    return value;
}

inline std::uint64_t take_u64(std::span<const std::byte> bytes, std::size_t& offset) {
    if (offset > bytes.size() || bytes.size() - offset < 8U) {
        throw std::runtime_error("truncated uint64 field");
    }

    std::uint64_t value = read_u64(bytes.data(), offset);
    offset += 8U;
    return value;
}

inline void require_size(std::span<const std::byte> bytes, std::size_t expected, const char* name) {
    if (bytes.size() != expected) {
        throw std::runtime_error(std::string("invalid ") + name + " payload size");
    }
}

} // namespace
#include "file_transfer/crc32.hpp"

#include <array>
#include <cstddef>

namespace file_transfer {
namespace {

constexpr std::array<std::uint32_t, 256> make_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::size_t index = 0; index < table.size(); ++index) {
        std::uint32_t value = static_cast<std::uint32_t>(index);
        for (int bit = 0; bit < 8; ++bit) {
            value = (value >> 1U) ^ ((value & 1U) != 0U ? 0xEDB88320U : 0U);
        }
        table[index] = value;
    }
    return table;
}

constexpr auto crc_table = make_table();

} // namespace

void Crc32::update(std::span<const std::byte> bytes) noexcept {
    for (const std::byte byte : bytes) {
        const std::uint8_t index = static_cast<std::uint8_t>(
            state_ ^ static_cast<std::uint32_t>(std::to_integer<unsigned char>(byte)));
        state_ = (state_ >> 8U) ^ crc_table[index];
    }
}

std::uint32_t Crc32::value() const noexcept {
    return state_ ^ 0xFFFFFFFFU;
}

std::uint32_t crc32(std::span<const std::byte> bytes) noexcept {
    Crc32 checksum;
    checksum.update(bytes);
    return checksum.value();
}

std::uint32_t crc32(std::string_view text) noexcept {
    const auto* data = reinterpret_cast<const std::byte*>(text.data());
    return crc32(std::span<const std::byte>(data, text.size()));
}

} // namespace file_transfer
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace file_transfer {

class Crc32 {
public:
    void update(std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] std::uint32_t value() const noexcept;

private:
    std::uint32_t state_ = 0xFFFFFFFFU;
};

[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] std::uint32_t crc32(std::string_view text) noexcept;

} // namespace file_transfer
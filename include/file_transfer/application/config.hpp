#pragma once
#include <cstdint>
#include <string_view>

namespace file_transfer::application::config {
    inline constexpr std::uint16_t default_port = 5000U;
    inline constexpr std::uint64_t default_idle_timeout_ms = 30000U;
    inline constexpr std::size_t default_max_active = 0U;
    inline constexpr std::string_view default_host = "127.0.0.1";

    inline constexpr std::size_t DISPATCH_BUDGET = 256U * 1024U;
}
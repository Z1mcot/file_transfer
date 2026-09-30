#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace file_transfer {

struct HelpOptions {};
struct ServerOptions {
    std::uint16_t port = 5000;
    std::uint64_t idle_timeout_ms = 30000U;
};
struct ClientOptions {
    std::filesystem::path file;
    std::vector<std::filesystem::path> files;
    std::string host = "127.0.0.1";
    std::uint16_t port = 5000;
    std::size_t max_active = 0U;
};

using Command = std::variant<HelpOptions, ServerOptions, ClientOptions>;

[[nodiscard]] Command parse_cli(std::span<const std::string_view> arguments);
[[nodiscard]] std::string usage_text();

} // namespace file_transfer
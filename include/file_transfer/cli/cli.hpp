#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace file_transfer {

struct HelpOptions {};
struct ServerOptions {
    std::uint16_t port = 5000;
};
struct ClientOptions {
    std::filesystem::path file;
    std::string host = "127.0.0.1";
    std::uint16_t port = 5000;
};

using Command = std::variant<HelpOptions, ServerOptions, ClientOptions>;

[[nodiscard]] Command parse_cli(std::span<const std::string_view> arguments);
[[nodiscard]] std::string usage_text();

} // namespace file_transfer
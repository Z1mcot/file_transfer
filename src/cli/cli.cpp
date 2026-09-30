#include "file_transfer/cli.hpp"

#include <charconv>
#include <limits.h>

#include <stdexcept>

namespace file_transfer {
namespace {

std::uint16_t parse_port(std::string_view text, bool allow_zero) {
    unsigned int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size() ||
        value > 65535U || (!allow_zero && value == 0U)) {
        throw std::invalid_argument("invalid port: " + std::string(text));
    }
    return static_cast<std::uint16_t>(value);
}

std::string option_value(std::span<const std::string_view> arguments, std::size_t& index,
                         std::string_view option) {
    if (index + 1U >= arguments.size() || arguments[index + 1U].empty()) {
        throw std::invalid_argument("missing value for " + std::string(option));
    }
    ++index;
    return std::string(arguments[index]);
}

[[noreturn]] void unknown_option(std::string_view option) {
    if (option == "-\xD1\x81") {
        throw std::invalid_argument("unknown flag '-с' uses a Cyrillic character; use ASCII '-c'");
    }
    throw std::invalid_argument("unknown option: " + std::string(option));
}

} // namespace

Command parse_cli(std::span<const std::string_view> arguments) {
    if (arguments.size() == 1U && (arguments[0] == "--help" || arguments[0] == "-h")) {
        return HelpOptions{};
    }
    if (arguments.empty()) {
        throw std::invalid_argument("missing mode");
    }

    if (arguments[0] == "-s") {
        ServerOptions options;
        bool port_seen = false;
        for (std::size_t index = 1U; index < arguments.size(); ++index) {
            if (arguments[index] == "--port") {
                if (port_seen) {
                    throw std::invalid_argument("--port specified more than once");
                }
                options.port = parse_port(option_value(arguments, index, "--port"), true);
                port_seen = true;
            } else {
                unknown_option(arguments[index]);
            }
        }
        return options;
    }

    if (arguments[0] == "-c") {
        if (arguments.size() < 2U || arguments[1].empty()) {
            throw std::invalid_argument("client mode requires a file path");
        }
        ClientOptions options;
        options.file = std::filesystem::path(arguments[1]);
        if (options.file.native().size() >= static_cast<std::size_t>(PATH_MAX)) {
            throw std::invalid_argument("file path is too long");
        }
        bool host_seen = false;
        bool port_seen = false;
        for (std::size_t index = 2U; index < arguments.size(); ++index) {
            if (arguments[index] == "--host") {
                if (host_seen) {
                    throw std::invalid_argument("--host specified more than once");
                }
                options.host = option_value(arguments, index, "--host");
                if (options.host.size() > 253U) {
                    throw std::invalid_argument("host name is too long");
                }
                host_seen = true;
            } else if (arguments[index] == "--port") {
                if (port_seen) {
                    throw std::invalid_argument("--port specified more than once");
                }
                options.port = parse_port(option_value(arguments, index, "--port"), false);
                port_seen = true;
            } else {
                unknown_option(arguments[index]);
            }
        }
        return options;
    }

    unknown_option(arguments[0]);
}

std::string usage_text() {
    return "Usage:\n"
           "  file_transfer -s [--port PORT]\n"
           "  file_transfer -c <file> [--host HOST] [--port PORT]\n"
           "  file_transfer --help\n";
}

} // namespace file_transfer
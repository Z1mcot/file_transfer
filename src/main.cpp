#include "file_transfer/cli/cli.hpp"
#include "file_transfer/application/client_application.hpp"
#include "file_transfer/application/server_application.hpp"

#include "file_transfer/application/executable_path.hpp"
#include "file_transfer/application/logging.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>
#include <variant>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    try {
        const file_transfer::Command command = file_transfer::parse_cli(arguments);
        if (std::holds_alternative<file_transfer::HelpOptions>(command)) {
            std::cout << file_transfer::usage_text();
            return 0;
        }
        if (const auto* server = std::get_if<file_transfer::ServerOptions>(&command)) {
            file_transfer::ServerApplication application(
                server->port, file_transfer::application::executable_directory(), server->idle_timeout_ms);
            application.run();
            return 0;
        }

        try {
            file_transfer::ClientApplication application(
                std::get<file_transfer::ClientOptions>(command));
            application.run();
            return 0;
        } catch (const std::exception& error) {
            file_transfer::application::log_parts("[CLIENT] Transfer failed: ", error.what());
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] " << error.what() << '\n'
                  << file_transfer::usage_text();
        return 2;
    }
}
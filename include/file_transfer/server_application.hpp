#pragma once

#include <cstdint>
#include <filesystem>

namespace file_transfer {

class ServerApplication final {
public:
    ServerApplication(std::uint16_t port, std::filesystem::path output_directory);
    void run();

private:
    std::uint16_t port_;
    std::filesystem::path output_directory_;
};

} // namespace file_transfer
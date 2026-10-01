#pragma once

#include <cstdint>
#include <filesystem>

namespace file_transfer {

class ServerApplication final {
public:
    ServerApplication(std::uint16_t port, std::filesystem::path output_directory,
                      std::uint64_t idle_timeout_ms = 30000U);
    void run();

private:
    std::uint16_t port_;
    std::filesystem::path output_directory_;
    std::uint64_t idle_timeout_ms_;
};

} // namespace file_transfer
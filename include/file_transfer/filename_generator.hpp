#pragma once

#include <chrono>
#include <filesystem>
#include <mutex>

namespace file_transfer {

class FilenameGenerator {
public:
    [[nodiscard]] std::filesystem::path next();

private:
    std::mutex mutex_;
    std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds> last_{};
    bool has_last_ = false;
};

} // namespace file_transfer
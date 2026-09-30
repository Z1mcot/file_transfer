#pragma once

#include <chrono>
#include <filesystem>
#include <mutex>

namespace file_transfer {

class FilenameGenerator {
    using Clock = std::chrono::system_clock;
    using Duration = std::chrono::microseconds;
    using TimePoint = std::chrono::time_point<Clock, Duration>;
public:
    [[nodiscard]] std::filesystem::path next();

private:
    std::mutex mutex_;
    TimePoint last_{};
    bool has_last_ = false;
};

} // namespace file_transfer
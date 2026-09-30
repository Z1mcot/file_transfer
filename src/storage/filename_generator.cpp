#include "file_transfer/filename_generator.hpp"

#include <cstdio>
#include <ctime>
#include <stdexcept>

namespace file_transfer {

std::filesystem::path FilenameGenerator::next() {
    std::lock_guard lock(mutex_);
    auto timestamp = std::chrono::time_point_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now());
    if (has_last_ && timestamp <= last_) {
        timestamp = last_ + std::chrono::microseconds{1};
    }
    last_ = timestamp;
    has_last_ = true;

    const auto seconds = std::chrono::floor<std::chrono::seconds>(timestamp);
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(timestamp - seconds).count();
    const std::time_t time = std::chrono::system_clock::to_time_t(seconds);
    std::tm utc{};
    if (::gmtime_r(&time, &utc) == nullptr) {
        throw std::runtime_error("could not format output timestamp");
    }
    char name[40]{};
    const int length = std::snprintf(name, sizeof(name), "%04d%02d%02d_%02d%02d%02d_%06lld.hex",
                                     utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                                     utc.tm_hour, utc.tm_min, utc.tm_sec,
                                     static_cast<long long>(micros));
    if (length < 0 || static_cast<std::size_t>(length) >= sizeof(name)) {
        throw std::runtime_error("could not format output filename");
    }
    return std::filesystem::path(name);
}

} // namespace file_transfer
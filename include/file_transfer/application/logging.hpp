#pragma once

#include <iostream>
#include <sstream>
#include <string_view>
#include <mutex>
#include <utility>

namespace file_transfer::application {

inline void log_line(std::string_view message) {
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    std::cout << message << std::endl;
}

template <typename... Parts>
void log_parts(Parts&&... parts) {
    std::ostringstream message;
    (message << ... << std::forward<Parts>(parts));
    log_line(message.str());
}

} // namespace file_transfer::application
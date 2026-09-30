#include "file_transfer/application/executable_path.hpp"

#include <unistd.h>

#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace file_transfer::application {

std::filesystem::path executable_directory() {
    std::vector<char> buffer(256U);
    for (;;) {
        const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            throw std::system_error(errno, std::generic_category(), "read /proc/self/exe");
        }
        if (static_cast<std::size_t>(length) < buffer.size()) {
            return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(length)))
                .parent_path();
        }
        if (buffer.size() >= 1024U * 1024U) {
            throw std::runtime_error("executable path is too long");
        }
        buffer.resize(buffer.size() * 2U);
    }
}

} // namespace file_transfer::application
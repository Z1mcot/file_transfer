#include "file_transfer/transfer/transfer.hpp"

#include "file_transfer/checksum/crc32.hpp"

#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

namespace file_transfer {

void pread_exact(int descriptor, std::span<std::byte> buffer, std::uint64_t offset) {
    std::size_t completed = 0;
    while (completed < buffer.size()) {
        const std::uint64_t current = offset + static_cast<std::uint64_t>(completed);
        const auto file_offset = static_cast<off_t>(current);

        const ssize_t count = ::pread(descriptor, buffer.data() + completed, buffer.size() - completed,
                                      file_offset);
        if (count > 0) {
            completed += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count == 0) {
            throw std::runtime_error("input file ended before its declared size");
        }
        throw std::system_error(errno, std::generic_category(), "read input file");
    }
}

FileMetadata calculate_file_metadata(int descriptor) {
    struct stat information {};
    if (::fstat(descriptor, &information) < 0) {
        throw std::system_error(errno, std::generic_category(), "stat input file");
    }

    if (!S_ISREG(information.st_mode) || information.st_size < 0) {
        throw std::runtime_error("input path is not a regular file");
    }

    const auto size = static_cast<std::uint64_t>(information.st_size);
    Crc32 checksum;

    std::array<std::byte, protocol::MAXIMUM_CHUNK_SIZE> buffer{};
    std::uint64_t offset = 0;
    while (offset < size) {
        const std::size_t requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), size - offset));

        pread_exact(descriptor, std::span<std::byte>(buffer.data(), requested), offset);

        checksum.update(std::span<const std::byte>(buffer.data(), requested));
        offset += static_cast<std::uint64_t>(requested);
    }
    return {size, checksum.value()};
}

} // namespace file_transfer
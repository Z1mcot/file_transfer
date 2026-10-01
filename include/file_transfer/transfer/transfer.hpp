#pragma once

#include "file_transfer/storage/i_file_store.hpp"
#include "file_transfer/protocol/protocol.hpp"

#include <cstdint>
#include <functional>
#include <filesystem>
#include <span>

namespace file_transfer {

struct FileMetadata {
    std::uint64_t size;
    std::uint32_t crc32;
};

void pread_exact(int descriptor, std::span<std::byte> buffer, std::uint64_t offset);

[[nodiscard]] FileMetadata calculate_file_metadata(int descriptor);

}
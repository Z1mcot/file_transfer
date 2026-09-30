#pragma once

#include "file_transfer/i_file_store.hpp"
#include "file_transfer/protocol.hpp"
#include "file_transfer/transport.hpp"

#include <cstdint>
#include <functional>
#include <filesystem>

namespace file_transfer {

struct FileMetadata {
    std::uint64_t size;
    std::uint32_t crc32;
};

struct ReceivedFile {
    std::filesystem::path path;
    std::uint64_t size;
};

using TransferStarted = std::function<void(std::uint64_t)>;

[[nodiscard]] FileMetadata calculate_file_metadata(int descriptor);
void send_file(ITransport& transport, int descriptor, const FileMetadata& metadata);
[[nodiscard]] ReceivedFile receive_file(
    ITransport& transport,
    IFileStore& store,
    const TransferStarted& on_started);

} // namespace file_transfer
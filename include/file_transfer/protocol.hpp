#pragma once

#include "file_transfer/transport.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace file_transfer::protocol {

inline constexpr std::uint16_t version = 1;
inline constexpr std::uint32_t maximum_chunk_size = 64U * 1024U;
inline constexpr std::size_t frame_header_size = 12;

enum class MessageType : std::uint16_t {
    hello = 1,
    data = 2,
    finish = 3,
    result = 4,
};

struct Message {
    MessageType type;
    std::vector<std::byte> payload;
};

struct Hello {
    std::uint64_t file_size;
    std::uint32_t file_crc32;
    std::uint32_t chunk_size;
};

struct DataView {
    std::uint64_t sequence;
    std::uint32_t payload_crc32;
    std::span<const std::byte> bytes;
};

struct Finish {
    std::uint64_t total_bytes;
    std::uint64_t total_chunks;
    std::uint32_t file_crc32;
};

struct Result {
    std::uint16_t code;
    std::string message;
};

void write_message(ITransport& transport, MessageType type, std::span<const std::byte> payload);
[[nodiscard]] Message read_message(ITransport& transport);

[[nodiscard]] std::vector<std::byte> encode_hello(const Hello& value);
[[nodiscard]] Hello decode_hello(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encode_data(
    std::uint64_t sequence,
    std::uint32_t checksum,
    std::span<const std::byte> bytes);
[[nodiscard]] DataView decode_data(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encode_finish(const Finish& value);
[[nodiscard]] Finish decode_finish(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encode_result(const Result& value);
[[nodiscard]] Result decode_result(std::span<const std::byte> bytes);

} // namespace file_transfer::protocol
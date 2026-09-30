#pragma once

#include <cstddef>
#include <filesystem>
#include <span>

namespace file_transfer {

class IStagedFile {
public:
    virtual ~IStagedFile() = default;
    virtual void write(std::span<const std::byte> bytes) = 0;
    [[nodiscard]] virtual std::filesystem::path commit() = 0;
    virtual void discard() = 0;
};

} // namespace file_transfer
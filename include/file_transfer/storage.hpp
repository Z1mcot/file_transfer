#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>

namespace file_transfer {

class IStagedFile {
public:
    virtual ~IStagedFile() = default;
    virtual void write(std::span<const std::byte> bytes) = 0;
    [[nodiscard]] virtual std::filesystem::path commit() = 0;
    virtual void discard() = 0;
};

class IFileStore {
public:
    virtual ~IFileStore() = default;
    [[nodiscard]] virtual std::unique_ptr<IStagedFile> create_staged_file() = 0;
};

class FilenameGenerator {
public:
    [[nodiscard]] std::filesystem::path next();

private:
    std::mutex mutex_;
    std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds> last_{};
    bool has_last_ = false;
};

class FileStore final : public IFileStore {
public:
    explicit FileStore(std::filesystem::path directory);
    [[nodiscard]] std::unique_ptr<IStagedFile> create_staged_file() override;

private:
    std::filesystem::path directory_;
    std::shared_ptr<FilenameGenerator> filenames_;
};

} // namespace file_transfer
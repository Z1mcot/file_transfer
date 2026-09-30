#pragma once

#include "file_transfer/storage/filename_generator.hpp"
#include "file_transfer/storage/i_file_store.hpp"

#include <filesystem>
#include <memory>

namespace file_transfer {

class FileStore final : public IFileStore {
public:
    explicit FileStore(std::filesystem::path directory);
    [[nodiscard]] std::unique_ptr<IStagedFile> create_staged_file() override;

private:
    std::filesystem::path directory_;
    std::shared_ptr<FilenameGenerator> filenames_;
};

} // namespace file_transfer
#pragma once

#include "file_transfer/storage/filename_generator.hpp"
#include "file_transfer/storage/i_staged_file.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace file_transfer {

class PosixStagedFile final : public IStagedFile {
public:
    PosixStagedFile(int descriptor, int staging_directory, int output_directory,
                    std::string staging_name, std::filesystem::path directory,
                    std::shared_ptr<FilenameGenerator> filenames);
    ~PosixStagedFile() override;

    void write(std::span<const std::byte> bytes) override;
    [[nodiscard]] std::filesystem::path commit() override;
    void discard() override;

private:
    void close_descriptors() noexcept;
    [[nodiscard]] static bool rename_no_replace(
        int source_directory, int destination_directory, const std::string& destination,
        const std::filesystem::path& destination_path);

    int descriptor_;
    int staging_directory_;
    int output_directory_;
    std::string staging_name_;
    std::filesystem::path directory_;
    std::shared_ptr<FilenameGenerator> filenames_;
    bool published_ = false;
};

} // namespace file_transfer
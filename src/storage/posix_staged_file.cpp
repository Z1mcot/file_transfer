#include "posix_staged_file.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <syncstream>
#include <system_error>
#include <utility>

namespace file_transfer {
namespace {

constexpr const char* staged_filename = "payload.part";

[[noreturn]] void throw_file_error(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

} // namespace

PosixStagedFile::PosixStagedFile(int descriptor, int staging_directory, int output_directory,
                                 std::string staging_name, std::filesystem::path directory,
                                 std::shared_ptr<FilenameGenerator> filenames)
    : descriptor_(descriptor), staging_directory_(staging_directory), output_directory_(output_directory),
      staging_name_(std::move(staging_name)), directory_(std::move(directory)),
      filenames_(std::move(filenames)) {}

PosixStagedFile::~PosixStagedFile() {
    try {
        discard();
    } catch (const std::exception& error) {
        std::osyncstream(std::cerr) << "[STORAGE] Staging cleanup failed: " << error.what() << '\n';
        close_descriptors();
    } catch (...) {
        std::osyncstream(std::cerr) << "[STORAGE] Staging cleanup failed: unknown error\n";
        close_descriptors();
    }
}

void PosixStagedFile::write(std::span<const std::byte> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::write(descriptor_, bytes.data() + offset, bytes.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count == 0) {
            throw std::runtime_error("write returned zero");
        }
        throw_file_error("write staged file");
    }
}

std::filesystem::path PosixStagedFile::commit() {
    if (::fsync(descriptor_) < 0) {
        throw_file_error("fsync staged file");
    }
    const int descriptor = std::exchange(descriptor_, -1);
    if (::close(descriptor) < 0) {
        throw_file_error("close staged file");
    }

    for (int attempt = 0; attempt < 1000; ++attempt) {
        const std::filesystem::path filename = filenames_->next();
        const std::filesystem::path destination = directory_ / filename;
        if (rename_no_replace(staging_directory_, output_directory_, filename.filename().string(),
                              destination)) {
            if (::fsync(staging_directory_) < 0 || ::fsync(output_directory_) < 0) {
                const int error = errno;
                if (::unlinkat(output_directory_, filename.filename().c_str(), 0) == 0) {
                    if (::fsync(output_directory_) < 0) {
                        const int rollback_sync_error = errno;
                        throw std::system_error(
                            rollback_sync_error, std::generic_category(),
                            "directory fsync failed after rollback; file may reappear after a crash: " +
                                destination.string());
                    }
                } else {
                    const int rollback_error = errno;
                    throw std::system_error(
                        error, std::generic_category(),
                        "directory fsync failed; validated file remains at " + destination.string() +
                            " (rollback failed: " + std::strerror(rollback_error) + ")");
                }
                errno = error;
                throw_file_error("fsync published file directories");
            }
            published_ = true;
            try {
                discard();
            } catch (const std::exception& error) {
                std::osyncstream(std::cerr) << "[STORAGE] File published at " << destination
                                            << "; staging cleanup failed: " << error.what() << '\n';
            } catch (...) {
                std::osyncstream(std::cerr) << "[STORAGE] File published at " << destination
                                            << "; staging cleanup failed: unknown error\n";
            }
            return destination;
        }
        if (errno != EEXIST) {
            throw_file_error("publish received file");
        }
    }
    throw std::runtime_error("could not allocate a unique output filename");
}

void PosixStagedFile::discard() {
    std::string failure;
    if (!published_ && staging_directory_ >= 0 &&
        ::unlinkat(staging_directory_, staged_filename, 0) < 0 && errno != ENOENT) {
        failure = std::string("could not remove staged payload: ") + std::strerror(errno);
    }
    if (descriptor_ >= 0) {
        const int descriptor = std::exchange(descriptor_, -1);
        if (::close(descriptor) < 0 && failure.empty()) {
            failure = std::string("could not close staged payload: ") + std::strerror(errno);
        }
    }
    if (output_directory_ >= 0 && !staging_name_.empty()) {
        if (::unlinkat(output_directory_, staging_name_.c_str(), AT_REMOVEDIR) < 0 && errno != ENOENT) {
            if (!failure.empty()) {
                failure += "; ";
            }
            failure += std::string("could not remove staging directory ") + staging_name_ + ": " +
                       std::strerror(errno);
        } else if (::fsync(output_directory_) < 0) {
            if (!failure.empty()) {
                failure += "; ";
            }
            failure += std::string("could not persist staging cleanup: ") + std::strerror(errno);
        }
    }
    if (!failure.empty()) {
        throw std::runtime_error(failure);
    }
    close_descriptors();
    staging_name_.clear();
}

void PosixStagedFile::close_descriptors() noexcept {
    if (descriptor_ >= 0) {
        ::close(std::exchange(descriptor_, -1));
    }
    if (staging_directory_ >= 0) {
        ::close(std::exchange(staging_directory_, -1));
    }
    if (output_directory_ >= 0) {
        ::close(std::exchange(output_directory_, -1));
    }
}

bool PosixStagedFile::rename_no_replace(int source_directory, int destination_directory,
                                        const std::string& destination,
                                        const std::filesystem::path& destination_path) {
#if defined(SYS_renameat2)
    if (::syscall(SYS_renameat2, source_directory, staged_filename,
                  destination_directory, destination.c_str(), RENAME_NOREPLACE) == 0) {
        return true;
    }
    if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP) {
        return false;
    }
#endif
    if (::linkat(source_directory, staged_filename, destination_directory, destination.c_str(), 0) < 0) {
        return false;
    }
    if (::unlinkat(source_directory, staged_filename, 0) == 0) {
        return true;
    }
    const int unlink_error = errno;
    if (::unlinkat(destination_directory, destination.c_str(), 0) < 0) {
        const int rollback_error = errno;
        throw std::system_error(
            unlink_error, std::generic_category(),
            "could not remove staged name or published output; validated file remains at " +
                destination_path.string() + " (rollback failed: " + std::strerror(rollback_error) + ")");
    }
    if (::fsync(destination_directory) < 0) {
        const int sync_error = errno;
        throw std::system_error(
            sync_error, std::generic_category(),
            "linkat rollback completed but output may reappear after a crash: " + destination_path.string());
    }
    errno = unlink_error;
    return false;
}

} // namespace file_transfer
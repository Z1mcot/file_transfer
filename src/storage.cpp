#include "file_transfer/storage.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <string>
#include <syncstream>
#include <system_error>
#include <utility>

namespace file_transfer {
namespace {

std::atomic<std::uint64_t> temporary_sequence{0};
constexpr const char* staged_filename = "payload.part";

[[noreturn]] void throw_file_error(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

std::string remove_staging_directory(int output_directory, const std::string& name) {
    if (::unlinkat(output_directory, name.c_str(), AT_REMOVEDIR) < 0 && errno != ENOENT) {
        return std::string("could not remove staging directory ") + name + ": " + std::strerror(errno);
    }
    if (::fsync(output_directory) < 0) {
        return std::string("could not persist staging directory cleanup ") + name + ": " +
               std::strerror(errno);
    }
    return {};
}

class StagedFile final : public IStagedFile {
public:
    StagedFile(int descriptor, int staging_directory, int output_directory,
               std::string staging_name, std::filesystem::path directory,
               std::shared_ptr<FilenameGenerator> filenames)
        : descriptor_(descriptor), staging_directory_(staging_directory),
          output_directory_(output_directory), staging_name_(std::move(staging_name)),
          directory_(std::move(directory)), filenames_(std::move(filenames)) {}

    ~StagedFile() override {
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

    void write(std::span<const std::byte> bytes) override {
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

    std::filesystem::path commit() override {
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

    void discard() override {
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

private:
    void close_descriptors() noexcept {
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

    static bool rename_no_replace(int source_directory, int destination_directory,
                                  const std::string& destination,
                                  const std::filesystem::path& destination_path) {
#if defined(SYS_renameat2)
        if (::syscall(SYS_renameat2, source_directory, staged_filename,
                      destination_directory, destination.c_str(),
                      RENAME_NOREPLACE) == 0) {
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
                    destination_path.string() + " (rollback failed: " +
                    std::strerror(rollback_error) + ")");
        }
        if (::fsync(destination_directory) < 0) {
            const int sync_error = errno;
            throw std::system_error(
                sync_error, std::generic_category(),
                "linkat rollback completed but output may reappear after a crash: " +
                    destination_path.string());
        }
        errno = unlink_error;
        return false;
    }

    int descriptor_;
    int staging_directory_;
    int output_directory_;
    std::string staging_name_;
    std::filesystem::path directory_;
    std::shared_ptr<FilenameGenerator> filenames_;
    bool published_ = false;
};

} // namespace

std::filesystem::path FilenameGenerator::next() {
    std::lock_guard lock(mutex_);
    auto timestamp = std::chrono::time_point_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now());
    if (has_last_ && timestamp <= last_) {
        timestamp = last_ + std::chrono::microseconds{1};
    }
    last_ = timestamp;
    has_last_ = true;

    const auto seconds = std::chrono::floor<std::chrono::seconds>(timestamp);
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(timestamp - seconds).count();
    const std::time_t time = std::chrono::system_clock::to_time_t(seconds);
    std::tm utc{};
    if (::gmtime_r(&time, &utc) == nullptr) {
        throw std::runtime_error("could not format output timestamp");
    }
    char name[40]{};
    const int length = std::snprintf(name, sizeof(name), "%04d%02d%02d_%02d%02d%02d_%06lld.hex",
                                     utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                                     utc.tm_hour, utc.tm_min, utc.tm_sec,
                                     static_cast<long long>(micros));
    if (length < 0 || static_cast<std::size_t>(length) >= sizeof(name)) {
        throw std::runtime_error("could not format output filename");
    }
    return std::filesystem::path(name);
}

FileStore::FileStore(std::filesystem::path directory)
    : directory_(std::move(directory)), filenames_(std::make_shared<FilenameGenerator>()) {
    if (!std::filesystem::is_directory(directory_)) {
        throw std::runtime_error("output directory does not exist: " + directory_.string());
    }
}

std::unique_ptr<IStagedFile> FileStore::create_staged_file() {
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const std::uint64_t sequence = temporary_sequence.fetch_add(1U, std::memory_order_relaxed);
        const std::string staging_name = ".file_transfer." + std::to_string(::getpid()) + "." +
                                         std::to_string(sequence);
        const int output_directory = ::open(directory_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (output_directory < 0) {
            throw_file_error("open output directory");
        }
        if (::mkdirat(output_directory, staging_name.c_str(), 0700) < 0) {
            const int error = errno;
            ::close(output_directory);
            if (error == EEXIST) {
                continue;
            }
            errno = error;
            throw_file_error("create private staging directory");
        }

        const int staging_directory = ::openat(
            output_directory, staging_name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (staging_directory < 0) {
            const int error = errno;
            const std::string cleanup_error = remove_staging_directory(output_directory, staging_name);
            ::close(output_directory);
            errno = error;
            throw std::system_error(error, std::generic_category(),
                                    "open private staging directory" +
                                        (cleanup_error.empty() ? std::string{} : "; " + cleanup_error));
        }
        struct stat staging_information {};
        if (::fstat(staging_directory, &staging_information) < 0) {
            const int error = errno;
            ::close(staging_directory);
            const std::string cleanup_error = remove_staging_directory(output_directory, staging_name);
            ::close(output_directory);
            errno = error;
            throw std::system_error(error, std::generic_category(),
                                    "stat private staging directory" +
                                        (cleanup_error.empty() ? std::string{} : "; " + cleanup_error));
        }
        if (!S_ISDIR(staging_information.st_mode) || staging_information.st_uid != ::geteuid() ||
            (staging_information.st_mode & 0077) != 0) {
            ::close(staging_directory);
            const std::string cleanup_error = remove_staging_directory(output_directory, staging_name);
            ::close(output_directory);
            throw std::runtime_error("private staging directory has unsafe ownership or permissions" +
                                     (cleanup_error.empty() ? std::string{} : "; " + cleanup_error));
        }
        const int descriptor = ::openat(staging_directory, staged_filename,
                                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (descriptor >= 0) {
            try {
                return std::make_unique<StagedFile>(descriptor, staging_directory, output_directory,
                                                    staging_name, directory_, filenames_);
            } catch (...) {
                ::unlinkat(staging_directory, staged_filename, 0);
                ::close(descriptor);
                ::close(staging_directory);
                const std::string cleanup_error = remove_staging_directory(output_directory, staging_name);
                ::close(output_directory);
                if (!cleanup_error.empty()) {
                    std::osyncstream(std::cerr) << "[STORAGE] Setup cleanup failed: "
                                                << cleanup_error << '\n';
                }
                throw;
            }
        }
        const int error = errno;
        std::string payload_cleanup_error;
        if (error == EEXIST && ::unlinkat(staging_directory, staged_filename, 0) < 0 && errno != ENOENT) {
            payload_cleanup_error = std::string("could not remove colliding staged payload: ") +
                                    std::strerror(errno);
        }
        ::close(staging_directory);
        const std::string cleanup_error = remove_staging_directory(output_directory, staging_name);
        ::close(output_directory);
        if (!payload_cleanup_error.empty()) {
            std::osyncstream(std::cerr) << "[STORAGE] Staging collision cleanup failed: "
                                        << payload_cleanup_error << '\n';
        }
        if (error != EEXIST) {
            errno = error;
            throw std::system_error(error, std::generic_category(),
                                    "create staged file" +
                                        (payload_cleanup_error.empty() ? std::string{} : "; " + payload_cleanup_error) +
                                        (cleanup_error.empty() ? std::string{} : "; " + cleanup_error));
        }
        if (!cleanup_error.empty()) {
            std::osyncstream(std::cerr) << "[STORAGE] Staging collision cleanup failed: "
                                        << cleanup_error << '\n';
        }
    }
    throw std::runtime_error("could not allocate a unique temporary filename");
}

} // namespace file_transfer
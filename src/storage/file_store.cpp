#include "file_transfer/file_store.hpp"

#include "posix_staged_file.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>
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

} // namespace

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
                return std::make_unique<PosixStagedFile>(descriptor, staging_directory, output_directory,
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
#include "file_transfer/storage/file_store.hpp"

#include "file_transfer/storage/posix_staged_file.hpp"

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

class ScopedDescriptor final {
public:
    explicit ScopedDescriptor(int descriptor = -1) noexcept : descriptor_(descriptor) {}
    ~ScopedDescriptor() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }

    ScopedDescriptor(const ScopedDescriptor&) = delete;
    ScopedDescriptor& operator=(const ScopedDescriptor&) = delete;

    [[nodiscard]] int get() const noexcept { return descriptor_; }
    int release() noexcept { return std::exchange(descriptor_, -1); }
    void reset() noexcept {
        if (descriptor_ >= 0) {
            ::close(std::exchange(descriptor_, -1));
        }
    }

private:
    int descriptor_;
};

std::string with_cleanup_error(std::string message, const std::string& cleanup_error) {
    if (!cleanup_error.empty()) {
        message += "; " + cleanup_error;
    }
    return message;
}

int open_private_staging_directory(int output_directory, const std::string& staging_name) {
    const int descriptor = ::openat(
        output_directory, staging_name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        const int error = errno;
        const std::string cleanup_error = remove_staging_directory(output_directory, staging_name);
        throw std::system_error(error, std::generic_category(),
                                with_cleanup_error("open private staging directory", cleanup_error));
    }

    struct stat information {};
    if (::fstat(descriptor, &information) < 0) {
        const int error = errno;
        ::close(descriptor);
        const std::string cleanup_error = remove_staging_directory(output_directory, staging_name);
        throw std::system_error(error, std::generic_category(),
                                with_cleanup_error("stat private staging directory", cleanup_error));
    }

    if (!S_ISDIR(information.st_mode) || information.st_uid != ::geteuid() ||
        (information.st_mode & 0077) != 0) {
        ::close(descriptor);
        const std::string cleanup_error = remove_staging_directory(output_directory, staging_name);
        throw std::runtime_error(with_cleanup_error(
            "private staging directory has unsafe ownership or permissions", cleanup_error));
    }

    return descriptor;
}

std::unique_ptr<IStagedFile> create_staged_file_attempt(
    const std::filesystem::path& directory,
    const std::shared_ptr<FilenameGenerator>& filenames,
    const std::string& staging_name) {
    ScopedDescriptor output_directory(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (output_directory.get() < 0) {
        throw_file_error("open output directory");
    }

    if (::mkdirat(output_directory.get(), staging_name.c_str(), 0700) < 0) {
        const int error = errno;
        if (error == EEXIST) {
            return nullptr;
        }
        errno = error;
        throw_file_error("create private staging directory");
    }

    ScopedDescriptor staging_directory(
        open_private_staging_directory(output_directory.get(), staging_name));
    const int descriptor = ::openat(staging_directory.get(), staged_filename,
                                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor >= 0) {
        try {
            auto staged_file = std::make_unique<PosixStagedFile>(
                descriptor, staging_directory.get(), output_directory.get(), staging_name, directory, filenames);
            staging_directory.release();
            output_directory.release();
            return staged_file;
        } catch (...) {
            ::unlinkat(staging_directory.get(), staged_filename, 0);
            ::close(descriptor);
            staging_directory.reset();
            const std::string cleanup_error = remove_staging_directory(output_directory.get(), staging_name);
            if (!cleanup_error.empty()) {
                std::cerr << "[STORAGE] Setup cleanup failed: " << cleanup_error << '\n';
            }
            throw;
        }
    }

    const int error = errno;
    std::string payload_cleanup_error;
    if (error == EEXIST && ::unlinkat(staging_directory.get(), staged_filename, 0) < 0 && errno != ENOENT) {
        payload_cleanup_error = std::string("could not remove colliding staged payload: ") +
                                std::strerror(errno);
    }

    staging_directory.reset();
    const std::string cleanup_error = remove_staging_directory(output_directory.get(), staging_name);
    if (!payload_cleanup_error.empty()) {
        std::cerr << "[STORAGE] Staging collision cleanup failed: " << payload_cleanup_error << '\n';
    }
    if (error != EEXIST) {
        throw std::system_error(error, std::generic_category(),
                                with_cleanup_error(with_cleanup_error("create staged file", payload_cleanup_error),
                                                   cleanup_error));
    }
    if (!cleanup_error.empty()) {
        std::cerr << "[STORAGE] Staging collision cleanup failed: " << cleanup_error << '\n';
    }
    return nullptr;
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
        if (auto staged_file = create_staged_file_attempt(directory_, filenames_, staging_name)) {
            return staged_file;
        }
    }
    throw std::runtime_error("could not allocate a unique temporary filename");
}

} // namespace file_transfer
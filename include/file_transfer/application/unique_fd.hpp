#pragma once

#include <unistd.h>

namespace file_transfer::application {

class UniqueFd final {
public:
    explicit UniqueFd(int descriptor) noexcept : descriptor_(descriptor) {}
    ~UniqueFd() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    [[nodiscard]] int get() const noexcept { return descriptor_; }

private:
    int descriptor_;
};

} // namespace file_transfer::application
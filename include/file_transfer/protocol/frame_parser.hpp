#pragma once

#include "file_transfer/protocol/protocol.hpp"

#include <span>
#include <vector>

namespace file_transfer::protocol {

class FrameParser final {
public:
    void feed(std::span<const std::byte> bytes);
    [[nodiscard]] bool has_message() const noexcept;
    [[nodiscard]] Message pop_message();
    void finish();

private:
    std::vector<std::byte> buffer_;
    std::vector<Message> messages_;
};

} // namespace file_transfer::protocol
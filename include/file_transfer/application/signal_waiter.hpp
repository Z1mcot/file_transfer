#pragma once

namespace file_transfer::application {

class SignalWaiter final {
public:
    SignalWaiter();
    ~SignalWaiter();

    SignalWaiter(const SignalWaiter&) = delete;
    SignalWaiter& operator=(const SignalWaiter&) = delete;
    
    [[nodiscard]] int fd() const noexcept;
    bool consume() noexcept;

private:
    int signal_fd_ = -1;
};

} // namespace file_transfer::application
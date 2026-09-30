#pragma once

#include "file_transfer/transport/i_transport_listener.hpp"

#include <atomic>
#include <thread>

namespace file_transfer::application {

class SignalWaiter final {
public:
    SignalWaiter(ITransportListener& listener, std::atomic<bool>& stopping);
    ~SignalWaiter();
    SignalWaiter(const SignalWaiter&) = delete;
    SignalWaiter& operator=(const SignalWaiter&) = delete;
    [[nodiscard]] int fd() const noexcept;
    bool consume() noexcept;

private:
    std::atomic<bool> finished_{false};
    std::thread thread_;
    int signal_fd_ = -1;
};

} // namespace file_transfer::application
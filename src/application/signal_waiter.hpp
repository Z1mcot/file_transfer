#pragma once

#include "file_transfer/i_transport_listener.hpp"

#include <atomic>
#include <thread>

namespace file_transfer::application {

class SignalWaiter final {
public:
    SignalWaiter(ITransportListener& listener, std::atomic<bool>& stopping);
    ~SignalWaiter();
    SignalWaiter(const SignalWaiter&) = delete;
    SignalWaiter& operator=(const SignalWaiter&) = delete;

private:
    std::atomic<bool> finished_{false};
    std::thread thread_;
};

} // namespace file_transfer::application
#include "file_transfer/application/signal_waiter.hpp"

#include <pthread.h>
#include <signal.h>

#include <cerrno>
#include <system_error>

namespace file_transfer::application {
namespace {

void install_ignored_signals() {
    struct sigaction ignore_signal {};
    ignore_signal.sa_handler = SIG_IGN;
    ::sigemptyset(&ignore_signal.sa_mask);
    if (::sigaction(SIGPIPE, &ignore_signal, nullptr) < 0) {
        throw std::system_error(errno, std::generic_category(), "ignore SIGPIPE");
    }
    if (::sigaction(SIGUSR1, &ignore_signal, nullptr) < 0) {
        throw std::system_error(errno, std::generic_category(), "ignore SIGUSR1");
    }
}

sigset_t block_stop_signals() {
    sigset_t signals{};
    ::sigemptyset(&signals);
    ::sigaddset(&signals, SIGINT);
    ::sigaddset(&signals, SIGTERM);
    const int error = ::pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    if (error != 0) {
        throw std::system_error(error, std::generic_category(), "block server stop signals");
    }
    return signals;
}

} // namespace

SignalWaiter::SignalWaiter(ITransportListener& listener, std::atomic<bool>& stopping) {
    install_ignored_signals();
    const sigset_t signals = block_stop_signals();
    thread_ = std::thread([this, &listener, &stopping, signals]() {
        int received_signal = 0;
        (void)::sigwait(&signals, &received_signal);
        stopping.store(true, std::memory_order_release);
        listener.cancel();
        finished_.store(true, std::memory_order_release);
    });
}

SignalWaiter::~SignalWaiter() {
    if (thread_.joinable()) {
        if (!finished_.load(std::memory_order_acquire)) {
            (void)::pthread_kill(thread_.native_handle(), SIGTERM);
        }
        thread_.join();
    }
}

} // namespace file_transfer::application
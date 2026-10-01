#include "file_transfer/application/signal_waiter.hpp"

#include <pthread.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>

namespace file_transfer::application {
namespace {

void install_ignored_signals() {
    struct sigaction ignore_signal {};
    ignore_signal.sa_handler = SIG_IGN;
    sigemptyset(&ignore_signal.sa_mask);
    if (::sigaction(SIGPIPE, &ignore_signal, nullptr) < 0) {
        throw std::system_error(errno, std::generic_category(), "ignore SIGPIPE");
    }
    if (::sigaction(SIGUSR1, &ignore_signal, nullptr) < 0) {
        throw std::system_error(errno, std::generic_category(), "ignore SIGUSR1");
    }
}

sigset_t block_stop_signals() {
    sigset_t signals{};
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    const int error = ::pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    if (error != 0) {
        throw std::system_error(error, std::generic_category(), "block server stop signals");
    }
    return signals;
}

} // namespace

SignalWaiter::SignalWaiter() {
    install_ignored_signals();
    const sigset_t signals = block_stop_signals();
    signal_fd_ = ::signalfd(-1, &signals, SFD_CLOEXEC | SFD_NONBLOCK);
    if (signal_fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "create signal fd");
    }
}

SignalWaiter::~SignalWaiter() {
    if (signal_fd_ >= 0) ::close(signal_fd_);
}

int SignalWaiter::fd() const noexcept { return signal_fd_; }

bool SignalWaiter::consume() noexcept {
    signalfd_siginfo information{};
    const ssize_t count = ::read(signal_fd_, &information, sizeof(information));
    return count == static_cast<ssize_t>(sizeof(information)) &&
           (information.ssi_signo == SIGINT || information.ssi_signo == SIGTERM);
}

} // namespace file_transfer::application
#include "file_transfer/application/event_loop.hpp"

#include <array>
#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sys/epoll.h>
#else
#include <sys/event.h>
#endif
#include <unistd.h>

namespace file_transfer::application {
namespace {

struct Registration {
    int descriptor;
    std::uint32_t events;
    EventLoop::Callback callback;
    bool active = true;
};

} // namespace

class EventLoop::Implementation final {
public:
    Implementation() {
#if defined(__linux__)
        descriptor_ = ::epoll_create1(EPOLL_CLOEXEC);
#else
        descriptor_ = ::kqueue();
#endif
        if (descriptor_ < 0) {
            throw std::system_error(errno, std::generic_category(), "create event loop");
        }
    }
    ~Implementation() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }

    void add(int descriptor, std::uint32_t events, EventLoop::Callback callback) {
        auto registration = std::make_shared<Registration>(Registration{descriptor, events, std::move(callback)});
#if defined(__linux__)
        epoll_event event{};
        event.events = events;
        event.data.ptr = registration.get();
        if (::epoll_ctl(descriptor_, EPOLL_CTL_ADD, descriptor, &event) < 0) {
            throw std::system_error(errno, std::generic_category(), "register descriptor");
        }
#else
        update_kqueue(*registration, 0U, events);
#endif
        registrations_[descriptor] = std::move(registration);
    }

    void modify(int descriptor, std::uint32_t events) {
        const auto iterator = registrations_.find(descriptor);
        if (iterator == registrations_.end()) {
            throw std::runtime_error("event loop descriptor is not registered");
        }
        Registration& registration = *iterator->second;
#if defined(__linux__)
        epoll_event event{};
        event.events = events;
        event.data.ptr = &registration;
        if (::epoll_ctl(descriptor_, EPOLL_CTL_MOD, descriptor, &event) < 0) {
            throw std::system_error(errno, std::generic_category(), "modify descriptor");
        }
#else
        update_kqueue(registration, registration.events, events);
#endif
        registration.events = events;
    }

    void remove(int descriptor) noexcept {
        const auto iterator = registrations_.find(descriptor);
        if (iterator == registrations_.end()) {
            return;
        }
        iterator->second->active = false;
#if defined(__linux__)
        (void)::epoll_ctl(descriptor_, EPOLL_CTL_DEL, descriptor, nullptr);
#else
        update_kqueue(*iterator->second, iterator->second->events, 0U);
#endif
        retired_.push_back(std::move(iterator->second));
        registrations_.erase(iterator);
    }

    void run() {
#if defined(__linux__)
        std::array<epoll_event, 128> events{};
#else
        std::array<struct kevent, 128> events{};
#endif
        while (!stopping_) {
#if defined(__linux__)
            const int count = ::epoll_wait(descriptor_, events.data(), static_cast<int>(events.size()), -1);
#else
            const int count = ::kevent(descriptor_, nullptr, 0, events.data(),
                                       static_cast<int>(events.size()), nullptr);
#endif
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0) {
                throw std::system_error(errno, std::generic_category(), "wait for events");
            }
            retired_.clear();
            for (int index = 0; index < count; ++index) {
#if defined(__linux__)
                auto* registration = static_cast<Registration*>(events[static_cast<std::size_t>(index)].data.ptr);
                const std::uint32_t flags = events[static_cast<std::size_t>(index)].events;
#else
                auto* registration = static_cast<Registration*>(events[static_cast<std::size_t>(index)].udata);
                std::uint32_t flags = 0U;
                if (events[static_cast<std::size_t>(index)].filter == EVFILT_READ) flags |= 0x001U;
                if (events[static_cast<std::size_t>(index)].filter == EVFILT_WRITE) flags |= 0x004U;
                if ((events[static_cast<std::size_t>(index)].flags & EV_EOF) != 0U) flags |= 0x010U;
                if ((events[static_cast<std::size_t>(index)].flags & EV_ERROR) != 0U) flags |= 0x008U;
#endif
                if (registration != nullptr && registration->active) {
                    registration->callback(flags);
                }
            }
        }
    }

    void stop() noexcept { stopping_ = true; }

private:
#if !defined(__linux__)
    void update_kqueue(Registration& registration, std::uint32_t old_events,
                       std::uint32_t new_events) {
        std::array<struct kevent, 2> changes{};
        int count = 0;
        if ((old_events & 0x001U) != (new_events & 0x001U)) {
            EV_SET(&changes[static_cast<std::size_t>(count++)], registration.descriptor, EVFILT_READ,
                   (new_events & 0x001U) != 0U ? EV_ADD : EV_DELETE, 0, 0, &registration);
        }
        if ((old_events & 0x004U) != (new_events & 0x004U)) {
            EV_SET(&changes[static_cast<std::size_t>(count++)], registration.descriptor, EVFILT_WRITE,
                   (new_events & 0x004U) != 0U ? EV_ADD : EV_DELETE, 0, 0, &registration);
        }
        if (count > 0 && ::kevent(descriptor_, changes.data(), count, nullptr, 0, nullptr) < 0 &&
            new_events != 0U) {
            throw std::system_error(errno, std::generic_category(), "register descriptor");
        }
    }
#endif

    int descriptor_ = -1;
    bool stopping_ = false;
    std::unordered_map<int, std::shared_ptr<Registration>> registrations_;
    std::vector<std::shared_ptr<Registration>> retired_;
};

EventLoop::EventLoop() : implementation_(std::make_unique<Implementation>()) {}
EventLoop::~EventLoop() = default;
void EventLoop::add(int descriptor, std::uint32_t events, Callback callback) {
    implementation_->add(descriptor, events, std::move(callback));
}
void EventLoop::modify(int descriptor, std::uint32_t events) { implementation_->modify(descriptor, events); }
void EventLoop::remove(int descriptor) noexcept { implementation_->remove(descriptor); }
void EventLoop::run() { implementation_->run(); }
void EventLoop::stop() noexcept { implementation_->stop(); }

} // namespace file_transfer::application
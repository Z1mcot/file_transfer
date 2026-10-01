#include "file_transfer/application/event_loop.hpp"

#include <array>
#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/epoll.h>
using EventsArray = std::array<epoll_event, 128>;

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
        descriptor_ = ::epoll_create1(EPOLL_CLOEXEC);
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

        epoll_event event{};
        event.events = events;
        event.data.ptr = registration.get();
        if (::epoll_ctl(descriptor_, EPOLL_CTL_ADD, descriptor, &event) < 0) {
            throw std::system_error(errno, std::generic_category(), "register descriptor");
        }
        registrations_[descriptor] = std::move(registration);
    }

    void modify(int descriptor, std::uint32_t events) {
        const auto iterator = registrations_.find(descriptor);
        if (iterator == registrations_.end()) {
            throw std::runtime_error("event loop descriptor is not registered");
        }
        Registration& registration = *iterator->second;

        epoll_event event{};
        event.events = events;
        event.data.ptr = &registration;
        if (::epoll_ctl(descriptor_, EPOLL_CTL_MOD, descriptor, &event) < 0) {
            throw std::system_error(errno, std::generic_category(), "modify descriptor");
        }
        registration.events = events;
    }

    void remove(int descriptor) noexcept {
        const auto iterator = registrations_.find(descriptor);
        if (iterator == registrations_.end()) {
            return;
        }

        iterator->second->active = false;
        (void)::epoll_ctl(descriptor_, EPOLL_CTL_DEL, descriptor, nullptr);

        retired_.push_back(std::move(iterator->second));
        registrations_.erase(iterator);
    }

    void run() {
        EventsArray events{};

        while (!stopping_) {
            const int count = wait_for_events(events);

            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0) {
                throw std::system_error(errno, std::generic_category(), "wait for events");
            }
            retired_.clear();
            for (int index = 0; index < count; ++index) {
                auto* registration = static_cast<Registration*>(events[static_cast<std::size_t>(index)].data.ptr);
                const std::uint32_t flags = events[static_cast<std::size_t>(index)].events;
                if (registration != nullptr && registration->active) {
                    registration->callback(flags);
                }
            }
        }
    }

    void stop() noexcept { stopping_ = true; }

private:
    int wait_for_events(EventsArray& events) const {
        return ::epoll_wait(descriptor_, events.data(), static_cast<int>(events.size()), -1);
    }

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
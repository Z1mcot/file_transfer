#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace file_transfer::application {

inline constexpr std::uint32_t event_read = 0x001U;
inline constexpr std::uint32_t event_write = 0x004U;
inline constexpr std::uint32_t event_error = 0x008U;
inline constexpr std::uint32_t event_hangup = 0x010U;

class EventLoop final {
public:
    using Callback = std::function<void(std::uint32_t)>;

    EventLoop();
    ~EventLoop();
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void add(int descriptor, std::uint32_t events, Callback callback);
    void modify(int descriptor, std::uint32_t events);
    void remove(int descriptor) noexcept;
    void run();
    void stop() noexcept;

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace file_transfer::application
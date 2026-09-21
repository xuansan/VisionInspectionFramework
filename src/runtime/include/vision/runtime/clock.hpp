#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace vision::runtime {
class Clock {
public:
    virtual ~Clock() = default;
    virtual std::uint64_t now_ns() const noexcept = 0;
};
class SteadyClock final : public Clock {
public:
    std::uint64_t now_ns() const noexcept override {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
};
class FakeClock final : public Clock {
public:
    explicit FakeClock(std::uint64_t start = 0) : value_(start) {}
    std::uint64_t now_ns() const noexcept override { return value_.load(); }
    void advance(std::uint64_t ns) {
        auto old = value_.load();
        do {
            if(ns > std::numeric_limits<std::uint64_t>::max() - old)
                throw std::overflow_error("Clock overflow");
        } while(!value_.compare_exchange_weak(old, old + ns));
    }
private:
    std::atomic<std::uint64_t> value_;
};
inline std::uint64_t deadline_after(const Clock& clock, std::uint64_t duration_ns) {
    const auto now = clock.now_ns();
    if(duration_ns > std::numeric_limits<std::uint64_t>::max() - now)
        throw std::overflow_error("Deadline overflow");
    return now + duration_ns;
}
} // namespace vision::runtime

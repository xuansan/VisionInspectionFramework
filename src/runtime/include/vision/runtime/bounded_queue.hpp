#pragma once
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace vision::runtime {
enum class PushStatus { Accepted, Full, TooLarge, Closed, InvalidCost };
struct QueueSnapshot { std::size_t count{}, bytes{}; bool closed{}; };
// In-process queue. Cost is the caller's explicit payload budget, not allocator RSS.
// No waits for space/items. The mutex protects only container/bookkeeping work.
template<class T> class BoundedQueue {
    static_assert(std::is_nothrow_move_constructible_v<T>);
public:
    BoundedQueue(std::size_t max_count, std::size_t max_bytes)
        : max_count_(max_count), max_bytes_(max_bytes) {
        if(!max_count || !max_bytes) throw std::invalid_argument("Positive queue limits required");
    }
    PushStatus try_push(T&& value, std::size_t cost) {
        std::lock_guard lock(mutex_);
        if(closed_) return PushStatus::Closed;
        if(!cost) return PushStatus::InvalidCost;
        if(cost > max_bytes_) return PushStatus::TooLarge;
        if(queue_.size() >= max_count_ || cost > max_bytes_ - bytes_) return PushStatus::Full;
        queue_.emplace_back(std::move(value), cost);
        bytes_ += cost;
        return PushStatus::Accepted;
    }
    std::optional<T> try_pop() {
        std::lock_guard lock(mutex_);
        if(queue_.empty()) return std::nullopt;
        std::optional<T> value(std::move(queue_.front().first));
        bytes_ -= queue_.front().second;
        queue_.pop_front();
        return value;
    }
    void close() { std::lock_guard lock(mutex_); closed_ = true; }
    QueueSnapshot snapshot() const {
        std::lock_guard lock(mutex_);
        return {queue_.size(), bytes_, closed_};
    }
private:
    const std::size_t max_count_, max_bytes_;
    mutable std::mutex mutex_;
    std::deque<std::pair<T, std::size_t>> queue_;
    std::size_t bytes_{};
    bool closed_{false};
};
} // namespace vision::runtime

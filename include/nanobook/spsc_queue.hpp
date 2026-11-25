#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace nanobook {

// Assumed destructive-interference size (x86-64 and most ARM64 cores).
inline constexpr std::size_t kCacheLineSize = 64;

// Bounded, lock-free, wait-free single-producer / single-consumer ring buffer.
//
//  * Exactly one thread may call the producer API (try_push / try_emplace) and
//    exactly one other thread the consumer API (try_pop / front / pop).
//  * Every operation completes in a bounded number of steps: no locks, no CAS
//    loops, no retries inside the queue.
//  * head_ (consumer) and tail_ (producer) are monotonically increasing counters
//    on separate cache lines; slots are addressed with `counter & mask_`.
//  * Each side keeps a private cached copy of the other side's counter and only
//    reloads the shared atomic when the cache says the queue looks full/empty,
//    which keeps cross-core cache-line traffic to a minimum.
//
// Synchronisation: the producer writes the slot, then publishes it with a
// release store to tail_; the consumer's acquire load of tail_ makes the slot
// contents visible. Symmetrically, the consumer releases the slot with a
// release store to head_, which the producer acquires before reusing it.
template <class T>
class SPSCQueue {
    static_assert(std::is_default_constructible_v<T>, "SPSCQueue slots are pre-constructed");
    static_assert(std::is_nothrow_move_assignable_v<T>, "T must be nothrow move-assignable");

public:
    // Capacity is rounded up to a power of two (minimum 2).
    explicit SPSCQueue(std::size_t min_capacity)
        : capacity_(std::bit_ceil(std::max<std::size_t>(min_capacity, 2))),
          mask_(capacity_ - 1),
          slots_(std::make_unique<T[]>(capacity_)) {}

    SPSCQueue(const SPSCQueue&)            = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    // ---- Producer ----------------------------------------------------------

    template <class... Args>
    [[nodiscard]] bool try_emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);  // only we write it
        if (tail - head_cache_ == capacity_) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (tail - head_cache_ == capacity_) return false;  // full
        }
        slots_[tail & mask_] = T(std::forward<Args>(args)...);
        tail_.store(tail + 1, std::memory_order_release);  // publish the slot
        return true;
    }

    [[nodiscard]] bool try_push(const T& value) noexcept(std::is_nothrow_copy_constructible_v<T>) {
        return try_emplace(value);
    }

    [[nodiscard]] bool try_push(T&& value) noexcept { return try_emplace(std::move(value)); }

    // ---- Consumer ----------------------------------------------------------

    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);  // only we write it
        if (head == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (head == tail_cache_) return false;  // empty
        }
        out = std::move(slots_[head & mask_]);
        head_.store(head + 1, std::memory_order_release);  // hand the slot back
        return true;
    }

    // Zero-copy access: inspect the oldest element in place, then pop() it.
    [[nodiscard]] T* front() noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (head == tail_cache_) return nullptr;
        }
        return &slots_[head & mask_];
    }

    // Precondition: front() returned non-null since the last pop().
    void pop() noexcept {
        head_.store(head_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }

    // ---- Either thread (approximate while the other side is active) --------

    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return tail >= head ? tail - head : 0;
    }

    [[nodiscard]] bool        empty_approx() const noexcept { return size_approx() == 0; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    // Consumer-owned cache line: read index + consumer's view of tail.
    alignas(kCacheLineSize) std::atomic<std::size_t> head_{0};
    std::size_t tail_cache_ = 0;

    // Producer-owned cache line: write index + producer's view of head.
    alignas(kCacheLineSize) std::atomic<std::size_t> tail_{0};
    std::size_t head_cache_ = 0;

    // Read-only after construction; own line so it is never invalidated by the
    // counters above.
    alignas(kCacheLineSize) const std::size_t capacity_;
    const std::size_t        mask_;
    std::unique_ptr<T[]>     slots_;
};

static_assert(std::atomic<std::size_t>::is_always_lock_free, "SPSCQueue requires lock-free atomics");

}  // namespace nanobook

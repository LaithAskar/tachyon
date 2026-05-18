#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace tachyon {

// Single-producer / single-consumer lock-free ring buffer.
//
// Contract: exactly one thread calls the producer-side methods (try_push) and
// exactly one — distinct — thread calls the consumer-side methods (try_pop).
// Any other usage pattern (two producers, two consumers, same thread on both
// sides without external synchronisation) is undefined behaviour. There is no
// runtime check; the cost of one is what this class exists to avoid.
//
// Layout: tail_ + cached_head_ live on a cache line owned by the producer,
// head_ + cached_tail_ on a line owned by the consumer, slots_ on its own
// line(s). The two indices never share a cache line, so the producer's tail
// store never invalidates the consumer's head line. Each side keeps a private
// cached copy of the counterpart index and only does an acquire-load when its
// local cache says "no progress" — that is the whole point of the cache, since
// the cross-core acquire-load is the expensive operation. In the common case
// where there is space (producer) or items (consumer), every push/pop is a
// single relaxed load plus a release store on the local line.
//
// One slot is reserved to disambiguate full from empty without a separate
// count, so usable capacity is Capacity - 1.
//
// Memory ordering follows the Lamport / Vyukov SPSC pattern:
//   - producer: store slot, then release tail (publishes the slot to consumer)
//   - consumer: acquire tail, then read slot (sees the slot the producer wrote)
//   - consumer: release head (publishes "slot is free" to producer)
//   - producer: acquire head (sees the producer's progress)
template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity >= 2,
                  "capacity must be >= 2 (one slot is reserved to disambiguate full/empty)");
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "capacity must be a power of two (so wrap-around is a bitmask, not a modulo)");
    static_assert(std::is_default_constructible_v<T>,
                  "T must be default-constructible (the slot array is default-initialised)");

public:
    static constexpr std::size_t capacity() noexcept { return Capacity; }
    static constexpr std::size_t max_size() noexcept { return Capacity - 1; }

    SpscQueue() = default;
    SpscQueue(const SpscQueue&)            = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;
    SpscQueue(SpscQueue&&)                 = delete;
    SpscQueue& operator=(SpscQueue&&)      = delete;

    // ---- producer side ------------------------------------------------------

    bool try_push(const T& v) {
        const auto tail = tail_.load(std::memory_order_relaxed);
        const auto next = (tail + 1) & kMask;
        if (next == cached_head_) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (next == cached_head_) return false;     // really full
        }
        slots_[tail] = v;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    bool try_push(T&& v) {
        const auto tail = tail_.load(std::memory_order_relaxed);
        const auto next = (tail + 1) & kMask;
        if (next == cached_head_) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (next == cached_head_) return false;
        }
        slots_[tail] = std::move(v);
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // ---- consumer side ------------------------------------------------------

    bool try_pop(T& out) {
        const auto head = head_.load(std::memory_order_relaxed);
        if (head == cached_tail_) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (head == cached_tail_) return false;     // really empty
        }
        out = std::move(slots_[head]);
        head_.store((head + 1) & kMask, std::memory_order_release);
        return true;
    }

    // ---- observers (approximate by design) ----------------------------------
    // Both sides may call these. The returned value is a snapshot in time;
    // by the time the caller looks at it, the real state may already differ.
    // Useful for diagnostics, never for correctness.

    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    std::size_t size_approx() const noexcept {
        const auto h = head_.load(std::memory_order_acquire);
        const auto t = tail_.load(std::memory_order_acquire);
        return (t - h) & kMask;
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    // x86-64 cache line is 64B in practice; std::hardware_destructive_interference_size
    // varies by toolchain (MSVC reports 64). We hardcode 64 here so the layout
    // is identical across compilers and we can reason about false sharing.
    static constexpr std::size_t kCacheLine = 64;

    // Producer-owned line: producer reads cached_head_, writes tail_.
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
    std::size_t cached_head_{0};

    // Consumer-owned line: consumer reads cached_tail_, writes head_.
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    std::size_t cached_tail_{0};

    // Data, on its own line so it doesn't share with either index line.
    alignas(kCacheLine) T slots_[Capacity]{};
};

}  // namespace tachyon

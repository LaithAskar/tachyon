#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>
#include <thread>

#include "tachyon/spsc_queue.hpp"

namespace tachyon {

// ---------------------------------------------------------------------------
// Single-threaded behaviour
// ---------------------------------------------------------------------------

TEST(SpscQueue, EmptyOnConstruction) {
    SpscQueue<int, 8> q;
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size_approx(), 0u);
}

TEST(SpscQueue, PushPopRoundTrip) {
    SpscQueue<int, 8> q;
    EXPECT_TRUE(q.try_push(42));
    EXPECT_FALSE(q.empty());
    int out = 0;
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 42);
    EXPECT_TRUE(q.empty());
}

TEST(SpscQueue, PreservesFifoOrder) {
    SpscQueue<int, 16> q;
    for (int i = 0; i < 10; ++i) ASSERT_TRUE(q.try_push(i));
    for (int i = 0; i < 10; ++i) {
        int v = -1;
        ASSERT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, i);
    }
}

TEST(SpscQueue, FullRejectsAtMaxSize) {
    SpscQueue<int, 8> q;                       // usable capacity = 7
    for (int i = 0; i < 7; ++i) ASSERT_TRUE(q.try_push(i));
    EXPECT_EQ(q.size_approx(), 7u);
    EXPECT_FALSE(q.try_push(99));              // would be 8 — reserved slot rule
}

TEST(SpscQueue, EmptyRejectsPop) {
    SpscQueue<int, 8> q;
    int out = 0;
    EXPECT_FALSE(q.try_pop(out));
}

// Drives the indices around the ring many times. If the bitmask/wrap math were
// off, this would either dropout or scramble values.
TEST(SpscQueue, WrapsAroundCleanly) {
    SpscQueue<int, 4> q;                       // usable capacity = 3
    int next_expected = 0;
    for (int batch = 0; batch < 1000; ++batch) {
        ASSERT_TRUE(q.try_push(batch * 10 + 0));
        ASSERT_TRUE(q.try_push(batch * 10 + 1));
        ASSERT_TRUE(q.try_push(batch * 10 + 2));
        EXPECT_FALSE(q.try_push(batch * 10 + 3));   // full
        int v = 0;
        ASSERT_TRUE(q.try_pop(v)); EXPECT_EQ(v, batch * 10 + 0);
        ASSERT_TRUE(q.try_pop(v)); EXPECT_EQ(v, batch * 10 + 1);
        ASSERT_TRUE(q.try_pop(v)); EXPECT_EQ(v, batch * 10 + 2);
        EXPECT_FALSE(q.try_pop(v));                 // empty
        (void)next_expected;
    }
}

// Smallest legal capacity (1 usable slot). If the static_assert bound were
// off, this would either fail to compile or behave wrong.
TEST(SpscQueue, MinimumCapacityWorks) {
    SpscQueue<int, 2> q;
    constexpr auto kMax = SpscQueue<int, 2>::max_size();   // hoist out: comma in template args confuses the EXPECT_EQ macro
    EXPECT_EQ(kMax, 1u);
    ASSERT_TRUE(q.try_push(7));
    EXPECT_FALSE(q.try_push(8));               // 1 usable slot is full
    int v = 0;
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 7);
    EXPECT_FALSE(q.try_pop(v));
}

// Non-trivial T: moves a payload that has real semantics. If the queue tried
// to copy where it shouldn't, this would still compile and pass — but if we
// later tighten to move-only types, this is the regression net.
TEST(SpscQueue, HandlesStructPayload) {
    struct Msg { std::uint64_t a = 0; std::uint64_t b = 0; std::uint64_t c = 0; };
    SpscQueue<Msg, 8> q;
    for (std::uint64_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(q.try_push(Msg{i, i * 2, i * 3}));
    }
    for (std::uint64_t i = 0; i < 5; ++i) {
        Msg m{};
        ASSERT_TRUE(q.try_pop(m));
        EXPECT_EQ(m.a, i);
        EXPECT_EQ(m.b, i * 2);
        EXPECT_EQ(m.c, i * 3);
    }
}

// ---------------------------------------------------------------------------
// Concurrent behaviour
//
// The two threads can race in any order the scheduler picks. The invariants
// we check are: (1) every value the producer pushes is eventually popped by
// the consumer, (2) the consumer pops them in strictly ascending order, and
// (3) the total count matches. If any of these fails, the memory ordering or
// wrap math is wrong, even on x86-TSO.
// ---------------------------------------------------------------------------

TEST(SpscQueue, TwoThreadMonotonicityStress) {
    constexpr std::size_t kN = 1'000'000;
    SpscQueue<std::uint64_t, 1024> q;
    std::atomic<bool> producer_done{false};

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kN; ++i) {
            // Busy-loop on full. In real code we would yield or pause; in a
            // test we want to hammer the path so the consumer sees as much
            // contention on the head/tail lines as possible.
            while (!q.try_push(i)) { /* spin */ }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::uint64_t expected = 0;
    std::uint64_t popped   = 0;
    std::thread consumer([&] {
        std::uint64_t v = 0;
        while (popped < kN) {
            if (q.try_pop(v)) {
                ASSERT_EQ(v, expected);            // strict FIFO across threads
                ++expected;
                ++popped;
            }
        }
    });

    producer.join();
    consumer.join();
    EXPECT_EQ(popped, kN);
    EXPECT_TRUE(producer_done.load());
    EXPECT_TRUE(q.empty());
}

// Same as above but the producer pauses sporadically. Exercises the
// cache-miss path (cached_head_ / cached_tail_ stale) more aggressively.
TEST(SpscQueue, TwoThreadPausedProducer) {
    constexpr std::size_t kN = 100'000;
    SpscQueue<std::uint64_t, 64> q;

    std::thread producer([&] {
        std::mt19937 rng(1234);
        for (std::uint64_t i = 0; i < kN; ++i) {
            while (!q.try_push(i)) { /* spin */ }
            if ((rng() & 0xFF) == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            }
        }
    });

    std::uint64_t expected = 0;
    std::thread consumer([&] {
        std::uint64_t v = 0;
        while (expected < kN) {
            if (q.try_pop(v)) {
                ASSERT_EQ(v, expected);
                ++expected;
            }
        }
    });

    producer.join();
    consumer.join();
    EXPECT_EQ(expected, kN);
    EXPECT_TRUE(q.empty());
}

}  // namespace tachyon

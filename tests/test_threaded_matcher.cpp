#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "tachyon/order.hpp"
#include "tachyon/threaded_matcher.hpp"
#include "tachyon/trade.hpp"
#include "tachyon/types.hpp"

namespace tachyon {

namespace {

Order make_limit(OrderId id, Side side, Price price, Quantity qty, TimestampNs ts = 0) {
    return Order{id, side, OrderType::Limit, price, qty, ts};
}

// Spin-pop with a generous wall-clock timeout so the tests terminate even if
// the matcher thread stalls. We pick 5s — any healthy matcher resolves a single
// message in microseconds; a 5s wait means something is wedged.
bool pop_with_timeout(ThreadedMatcher<>& m, Trade& out,
                      std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (m.try_pop_trade(out)) return true;
        std::this_thread::yield();
    }
    return false;
}

void submit_blocking(ThreadedMatcher<>& m, const Order& o) {
    while (!m.try_submit(o)) std::this_thread::yield();
}

void cancel_blocking(ThreadedMatcher<>& m, OrderId id) {
    while (!m.try_cancel(id)) std::this_thread::yield();
}

}  // namespace

// ---------------------------------------------------------------------------
// Single-threaded happy paths
// ---------------------------------------------------------------------------

TEST(ThreadedMatcher, RestingOrderProducesNoTrade) {
    ThreadedMatcher<> m;
    ASSERT_TRUE(m.try_submit(make_limit(1, Side::Buy, 100'00, 10)));

    // Give the worker time to process. After finalize, the book is ours.
    Trade t{};
    EXPECT_FALSE(pop_with_timeout(m, t, std::chrono::milliseconds(100)));

    const OrderBook& book = m.finalize();
    EXPECT_EQ(book.size(), 1u);
    ASSERT_TRUE(book.best_bid().has_value());
    EXPECT_EQ(*book.best_bid(), 100'00);
}

TEST(ThreadedMatcher, CrossingOrdersProduceOneTrade) {
    ThreadedMatcher<> m;
    submit_blocking(m, make_limit(1, Side::Sell, 100'00, 10));
    submit_blocking(m, make_limit(2, Side::Buy,  100'00, 10));

    Trade t{};
    ASSERT_TRUE(pop_with_timeout(m, t));
    EXPECT_EQ(t.taker_id, 2u);
    EXPECT_EQ(t.maker_id, 1u);
    EXPECT_EQ(t.price,    100'00);
    EXPECT_EQ(t.quantity, 10u);

    // No second trade.
    EXPECT_FALSE(pop_with_timeout(m, t, std::chrono::milliseconds(100)));

    const OrderBook& book = m.finalize();
    EXPECT_EQ(book.size(), 0u);
}

TEST(ThreadedMatcher, CancelRemovesRestingOrder) {
    ThreadedMatcher<> m;
    submit_blocking(m, make_limit(1, Side::Buy, 100'00, 10));
    cancel_blocking(m, 1);

    // After cancel, a crossing sell should rest (no liquidity to hit).
    submit_blocking(m, make_limit(2, Side::Sell, 100'00, 10));

    Trade t{};
    EXPECT_FALSE(pop_with_timeout(m, t, std::chrono::milliseconds(100)));

    const OrderBook& book = m.finalize();
    EXPECT_EQ(book.size(), 1u);                       // only the sell rests
    ASSERT_TRUE(book.best_ask().has_value());
    EXPECT_EQ(*book.best_ask(), 100'00);
}

// One taker sweeps multiple makers — all trade events must arrive in match
// order (price-time priority).
TEST(ThreadedMatcher, MultiTradeFanOutPreservesOrder) {
    ThreadedMatcher<> m;
    // Three asks: two at the same price (FIFO within level), one above.
    submit_blocking(m, make_limit(1, Side::Sell, 100'00, 5, /*ts=*/1));
    submit_blocking(m, make_limit(2, Side::Sell, 100'00, 5, /*ts=*/2));
    submit_blocking(m, make_limit(3, Side::Sell, 101'00, 5, /*ts=*/3));
    // Taker eats all three.
    submit_blocking(m, make_limit(99, Side::Buy, 101'00, 15));

    std::vector<Trade> trades;
    for (int i = 0; i < 3; ++i) {
        Trade t{};
        ASSERT_TRUE(pop_with_timeout(m, t)) << "missing trade " << i;
        trades.push_back(t);
    }
    Trade extra{};
    EXPECT_FALSE(pop_with_timeout(m, extra, std::chrono::milliseconds(100)));

    EXPECT_EQ(trades[0].maker_id, 1u);
    EXPECT_EQ(trades[0].price,    100'00);
    EXPECT_EQ(trades[1].maker_id, 2u);
    EXPECT_EQ(trades[1].price,    100'00);
    EXPECT_EQ(trades[2].maker_id, 3u);
    EXPECT_EQ(trades[2].price,    101'00);
    for (const Trade& tr : trades) EXPECT_EQ(tr.taker_id, 99u);
}

// ---------------------------------------------------------------------------
// Shutdown contract
// ---------------------------------------------------------------------------

TEST(ThreadedMatcher, FinalizeDrainsPendingInbound) {
    // Stuff the inbound with crossing pairs, then finalize immediately.
    // finalize() must drain — every submitted pair must produce its trade
    // before finalize() returns and the book reflects all the work.
    constexpr int kPairs = 500;

    ThreadedMatcher<> m;
    for (int i = 0; i < kPairs; ++i) {
        submit_blocking(m, make_limit(2 * i + 1, Side::Sell, 100'00 + i, 1));
        submit_blocking(m, make_limit(2 * i + 2, Side::Buy,  100'00 + i, 1));
    }

    const OrderBook& book = m.finalize();
    EXPECT_EQ(book.size(), 0u);                       // all crossed

    // And every trade event reached outbound. Worker isn't running anymore,
    // so try_pop sees a static state.
    int trades_seen = 0;
    Trade t{};
    while (m.try_pop_trade(t)) ++trades_seen;
    EXPECT_EQ(trades_seen, kPairs);
}

// ---------------------------------------------------------------------------
// Concurrent stress
//
// One producer thread submits orders, one consumer thread drains trades. Two
// independent threads — and the matcher itself is a third. If the SPSC fences
// or the drain-after-stop logic were wrong, this would deadlock or lose trades.
// ---------------------------------------------------------------------------

TEST(ThreadedMatcher, ConcurrentProducerConsumerStress) {
    constexpr int kPairs = 50'000;                    // 100k orders total

    ThreadedMatcher<> m;
    std::atomic<int>  trades_popped{0};
    std::atomic<bool> producer_done{false};

    std::thread consumer([&] {
        Trade t{};
        // Run until we've seen kPairs trades AND producer is done. Producer-done
        // alone isn't enough — outbound may still have backlog when producer
        // finishes; we have to drain it.
        while (trades_popped.load(std::memory_order_relaxed) < kPairs) {
            if (m.try_pop_trade(t)) {
                trades_popped.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    });

    std::thread producer([&] {
        for (int i = 0; i < kPairs; ++i) {
            // Sell first, then buy crossing it. Each pair yields one trade.
            submit_blocking(m, make_limit(2 * i + 1, Side::Sell, 100'00, 1));
            submit_blocking(m, make_limit(2 * i + 2, Side::Buy,  100'00, 1));
        }
        producer_done.store(true, std::memory_order_release);
    });

    producer.join();
    consumer.join();

    EXPECT_TRUE(producer_done.load());
    EXPECT_EQ(trades_popped.load(), kPairs);

    const OrderBook& book = m.finalize();
    EXPECT_EQ(book.size(), 0u);
}

}  // namespace tachyon

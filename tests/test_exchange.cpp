#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "tachyon/exchange.hpp"
#include "tachyon/order.hpp"
#include "tachyon/trade.hpp"
#include "tachyon/types.hpp"

namespace tachyon {

namespace {

Order make_limit(OrderId id, Side side, Price price, Quantity qty, TimestampNs ts = 0) {
    return Order{id, side, OrderType::Limit, price, qty, ts};
}

bool pop_with_timeout(Exchange<>& x, Trade& out,
                     std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (x.try_pop_trade(out)) return true;
        std::this_thread::yield();
    }
    return false;
}

void submit_blocking(Exchange<>& x, SymbolId sym, const Order& o) {
    while (!x.try_submit(sym, o)) std::this_thread::yield();
}

void cancel_blocking(Exchange<>& x, SymbolId sym, OrderId id) {
    while (!x.try_cancel(sym, id)) std::this_thread::yield();
}

}  // namespace

// ---------------------------------------------------------------------------
// Single-symbol degeneracy: Exchange with 1 book matches ThreadedMatcher.
// ---------------------------------------------------------------------------

TEST(Exchange, SingleSymbolCrossingTrade) {
    Exchange<> x(/*num_symbols=*/1);
    submit_blocking(x, 0, make_limit(1, Side::Sell, 100'00, 10));
    submit_blocking(x, 0, make_limit(2, Side::Buy,  100'00, 10));

    Trade t{};
    ASSERT_TRUE(pop_with_timeout(x, t));
    EXPECT_EQ(t.taker_id,  2u);
    EXPECT_EQ(t.maker_id,  1u);
    EXPECT_EQ(t.price,     100'00);
    EXPECT_EQ(t.quantity,  10u);
    EXPECT_EQ(t.symbol_id, 0u);

    x.finalize();
    EXPECT_EQ(x.book(0).size(), 0u);
}

// ---------------------------------------------------------------------------
// Multi-symbol isolation: an order on symbol 0 must never match against
// liquidity on symbol 1, even at the same price level.
// ---------------------------------------------------------------------------

TEST(Exchange, CrossSymbolDoesNotMatch) {
    Exchange<> x(/*num_symbols=*/2);
    submit_blocking(x, 0, make_limit(1, Side::Sell, 100'00, 10));   // sym 0 ask
    submit_blocking(x, 1, make_limit(2, Side::Buy,  100'00, 10));   // sym 1 bid

    Trade t{};
    EXPECT_FALSE(pop_with_timeout(x, t, std::chrono::milliseconds(100)))
        << "saw a cross-symbol trade — symbol isolation is broken";

    x.finalize();
    // Both orders rest in their respective books.
    EXPECT_EQ(x.book(0).size(), 1u);
    EXPECT_EQ(x.book(1).size(), 1u);
    ASSERT_TRUE(x.book(0).best_ask().has_value());
    EXPECT_EQ(*x.book(0).best_ask(), 100'00);
    ASSERT_TRUE(x.book(1).best_bid().has_value());
    EXPECT_EQ(*x.book(1).best_bid(), 100'00);
}

// ---------------------------------------------------------------------------
// Trades from each symbol are stamped with the right symbol_id.
// ---------------------------------------------------------------------------

TEST(Exchange, TradesCarryCorrectSymbolId) {
    Exchange<> x(/*num_symbols=*/3);
    // Symbol 2: crossing pair.
    submit_blocking(x, 2, make_limit(10, Side::Sell, 50'00, 5));
    submit_blocking(x, 2, make_limit(11, Side::Buy,  50'00, 5));
    // Symbol 0: different crossing pair, different price.
    submit_blocking(x, 0, make_limit(20, Side::Sell, 100'00, 7));
    submit_blocking(x, 0, make_limit(21, Side::Buy,  100'00, 7));

    std::vector<Trade> got;
    for (int i = 0; i < 2; ++i) {
        Trade t{};
        ASSERT_TRUE(pop_with_timeout(x, t)) << "trade " << i << " missing";
        got.push_back(t);
    }

    // FIFO across symbols on the inbound — symbol 2 pair pushed first.
    EXPECT_EQ(got[0].symbol_id, 2u);
    EXPECT_EQ(got[0].price,     50'00);
    EXPECT_EQ(got[1].symbol_id, 0u);
    EXPECT_EQ(got[1].price,     100'00);

    x.finalize();
}

// ---------------------------------------------------------------------------
// Cancel must target the right symbol — a wrong-symbol cancel is a no-op.
// ---------------------------------------------------------------------------

TEST(Exchange, CancelIsSymbolScoped) {
    Exchange<> x(/*num_symbols=*/2);
    submit_blocking(x, 0, make_limit(42, Side::Buy, 100'00, 5));
    cancel_blocking(x, 1, /*id=*/42);                       // wrong symbol — no-op

    x.finalize();
    EXPECT_EQ(x.book(0).size(), 1u) << "wrong-symbol cancel removed the order";
    EXPECT_EQ(x.book(1).size(), 0u);
}

// ---------------------------------------------------------------------------
// Out-of-range symbol on submit: silent drop, no crash, no trade.
// ---------------------------------------------------------------------------

TEST(Exchange, OutOfRangeSymbolIdIsDropped) {
    Exchange<> x(/*num_symbols=*/2);
    submit_blocking(x, /*sym=*/9, make_limit(1, Side::Buy, 100'00, 1));   // bad
    submit_blocking(x, /*sym=*/0, make_limit(2, Side::Buy, 100'00, 1));   // good

    // Sleep briefly so the worker has time to process both (the bad one is a
    // no-op, the good one rests with no trade).
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Trade t{};
    EXPECT_FALSE(x.try_pop_trade(t));

    x.finalize();
    EXPECT_EQ(x.book(0).size(), 1u);
    EXPECT_EQ(x.book(1).size(), 0u);
}

// ---------------------------------------------------------------------------
// Concurrent stress across multiple symbols. One producer thread interleaves
// crossing pairs across N symbols; one consumer drains. Every pair must
// produce exactly one trade, with the right symbol_id.
// ---------------------------------------------------------------------------

TEST(Exchange, ConcurrentMultiSymbolStress) {
    constexpr int kSymbols   = 4;
    constexpr int kPerSymbol = 10'000;
    constexpr int kTotal     = kSymbols * kPerSymbol;

    Exchange<> x(/*num_symbols=*/kSymbols);
    std::atomic<int>           total_popped{0};
    std::vector<std::atomic<int>> per_symbol_popped(kSymbols);
    for (auto& c : per_symbol_popped) c.store(0);

    std::thread consumer([&] {
        Trade t{};
        while (total_popped.load(std::memory_order_relaxed) < kTotal) {
            if (x.try_pop_trade(t)) {
                ASSERT_LT(t.symbol_id, static_cast<SymbolId>(kSymbols));
                per_symbol_popped[t.symbol_id].fetch_add(1, std::memory_order_relaxed);
                total_popped.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    });

    std::thread producer([&] {
        // Interleave symbols round-robin so the inbound queue carries a mix.
        OrderId id = 1;
        for (int i = 0; i < kPerSymbol; ++i) {
            for (int s = 0; s < kSymbols; ++s) {
                submit_blocking(x, s, make_limit(id++, Side::Sell, 100'00, 1));
                submit_blocking(x, s, make_limit(id++, Side::Buy,  100'00, 1));
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(total_popped.load(), kTotal);
    for (int s = 0; s < kSymbols; ++s) {
        EXPECT_EQ(per_symbol_popped[s].load(), kPerSymbol)
            << "symbol " << s << " lost or gained trades";
    }

    x.finalize();
    for (int s = 0; s < kSymbols; ++s) {
        EXPECT_EQ(x.book(s).size(), 0u) << "symbol " << s << " has resting orders";
    }
}

}  // namespace tachyon

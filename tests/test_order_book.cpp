#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "tachyon/order_book.hpp"

namespace tachyon {

namespace {

Order make_limit(OrderId id, Side side, Price price, Quantity qty, TimestampNs ts = 0) {
    return Order{id, side, OrderType::Limit, price, qty, ts};
}

Order make_market(OrderId id, Side side, Quantity qty, TimestampNs ts = 0) {
    return Order{id, side, OrderType::Market, /*price=*/0, qty, ts};
}

}  // namespace

// --- Week 1 -----------------------------------------------------------------

TEST(OrderBook, EmptyOnConstruction) {
    OrderBook book;
    EXPECT_EQ(book.size(), 0u);
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
}

TEST(OrderBook, SingleBuyLimitBecomesBestBid) {
    OrderBook book;
    auto trades = book.submit(make_limit(1, Side::Buy, 100'00, 10));
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.size(), 1u);
    ASSERT_TRUE(book.best_bid().has_value());
    EXPECT_EQ(*book.best_bid(), 100'00);
    EXPECT_FALSE(book.best_ask().has_value());
}

TEST(OrderBook, MultipleBidsBestBidIsHighest) {
    OrderBook book;
    book.submit(make_limit(1, Side::Buy, 100'00, 10));
    book.submit(make_limit(2, Side::Buy, 101'00, 10));
    book.submit(make_limit(3, Side::Buy, 99'00, 10));
    EXPECT_EQ(book.size(), 3u);
    ASSERT_TRUE(book.best_bid().has_value());
    EXPECT_EQ(*book.best_bid(), 101'00);
}

TEST(OrderBook, MultipleAsksBestAskIsLowest) {
    OrderBook book;
    book.submit(make_limit(1, Side::Sell, 102'00, 10));
    book.submit(make_limit(2, Side::Sell, 101'00, 10));
    book.submit(make_limit(3, Side::Sell, 103'00, 10));
    EXPECT_EQ(book.size(), 3u);
    ASSERT_TRUE(book.best_ask().has_value());
    EXPECT_EQ(*book.best_ask(), 101'00);
}

TEST(OrderBook, CancelRemovesRestingOrder) {
    OrderBook book;
    book.submit(make_limit(1, Side::Buy, 100'00, 10));
    book.submit(make_limit(2, Side::Buy, 101'00, 5));
    ASSERT_TRUE(book.cancel(2));
    EXPECT_EQ(book.size(), 1u);
    EXPECT_EQ(*book.best_bid(), 100'00);
    EXPECT_FALSE(book.cancel(2));   // second cancel is a no-op
    EXPECT_FALSE(book.cancel(999)); // unknown id
}

TEST(OrderBook, CancelEmptiesLevel) {
    OrderBook book;
    book.submit(make_limit(1, Side::Buy, 100'00, 10));
    ASSERT_TRUE(book.cancel(1));
    EXPECT_EQ(book.size(), 0u);
    EXPECT_FALSE(book.best_bid().has_value());
}

// --- Week 2: matching / crossing / FIFO ------------------------------------

TEST(OrderBook, CrossingLimitGeneratesTrade) {
    OrderBook book;
    book.submit(make_limit(1, Side::Sell, 100'00, 10));
    auto trades = book.submit(make_limit(2, Side::Buy, 100'00, 10));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].taker_id, 2u);
    EXPECT_EQ(trades[0].maker_id, 1u);
    EXPECT_EQ(trades[0].price, 100'00);
    EXPECT_EQ(trades[0].quantity, 10u);
    EXPECT_EQ(book.size(), 0u);
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
}

TEST(OrderBook, AggressiveBuyTakesPriceImprovement) {
    // Taker offers 105, best ask is 100. Trade should print at 100 (maker price).
    OrderBook book;
    book.submit(make_limit(1, Side::Sell, 100'00, 5));
    auto trades = book.submit(make_limit(2, Side::Buy, 105'00, 5));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 100'00);
}

TEST(OrderBook, PartialFillRestingMakerRemains) {
    OrderBook book;
    book.submit(make_limit(1, Side::Sell, 100'00, 10));
    auto trades = book.submit(make_limit(2, Side::Buy, 100'00, 4));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 4u);
    EXPECT_EQ(book.size(), 1u);
    EXPECT_EQ(*book.best_ask(), 100'00);
}

TEST(OrderBook, PartialFillResidualLimitInserted) {
    // Taker buy 15 at 100, only 10 available -> 5 residual sits as best bid.
    OrderBook book;
    book.submit(make_limit(1, Side::Sell, 100'00, 10));
    auto trades = book.submit(make_limit(2, Side::Buy, 100'00, 15));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 10u);
    EXPECT_EQ(book.size(), 1u);
    ASSERT_TRUE(book.best_bid().has_value());
    EXPECT_EQ(*book.best_bid(), 100'00);
    EXPECT_FALSE(book.best_ask().has_value());
}

TEST(OrderBook, FifoAtSamePriceLevel) {
    OrderBook book;
    book.submit(make_limit(1, Side::Sell, 100'00, 5, /*ts=*/1));
    book.submit(make_limit(2, Side::Sell, 100'00, 5, /*ts=*/2));
    book.submit(make_limit(3, Side::Sell, 100'00, 5, /*ts=*/3));

    auto trades = book.submit(make_limit(99, Side::Buy, 100'00, 7));
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].maker_id, 1u);  // earliest first
    EXPECT_EQ(trades[0].quantity, 5u);
    EXPECT_EQ(trades[1].maker_id, 2u);  // partial fill of next
    EXPECT_EQ(trades[1].quantity, 2u);
    EXPECT_EQ(book.size(), 2u);
}

TEST(OrderBook, SweepMultipleLevels) {
    OrderBook book;
    book.submit(make_limit(1, Side::Sell, 100'00, 5));
    book.submit(make_limit(2, Side::Sell, 101'00, 5));
    book.submit(make_limit(3, Side::Sell, 102'00, 5));

    auto trades = book.submit(make_limit(99, Side::Buy, 101'00, 8));
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].price, 100'00);
    EXPECT_EQ(trades[0].quantity, 5u);
    EXPECT_EQ(trades[1].price, 101'00);
    EXPECT_EQ(trades[1].quantity, 3u);
    EXPECT_EQ(book.size(), 2u);             // 2 sell levels remain (partial 101, full 102)
    EXPECT_EQ(*book.best_ask(), 101'00);
}

TEST(OrderBook, NonCrossingLimitJustRests) {
    OrderBook book;
    book.submit(make_limit(1, Side::Sell, 101'00, 5));
    auto trades = book.submit(make_limit(2, Side::Buy, 100'00, 5));
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.size(), 2u);
    EXPECT_EQ(*book.best_bid(), 100'00);
    EXPECT_EQ(*book.best_ask(), 101'00);
}

// --- Week 3: market orders, partial fills, edge cases ----------------------

TEST(OrderBook, MarketBuyConsumesBook) {
    OrderBook book;
    book.submit(make_limit(1, Side::Sell, 100'00, 3));
    book.submit(make_limit(2, Side::Sell, 101'00, 3));

    auto trades = book.submit(make_market(99, Side::Buy, 5));
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].price, 100'00);
    EXPECT_EQ(trades[1].price, 101'00);
    EXPECT_EQ(book.size(), 1u);
    EXPECT_EQ(*book.best_ask(), 101'00);
}

TEST(OrderBook, MarketOrderResidualIsDropped) {
    OrderBook book;
    book.submit(make_limit(1, Side::Sell, 100'00, 3));
    auto trades = book.submit(make_market(99, Side::Buy, 10));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 3u);
    EXPECT_EQ(book.size(), 0u);
    EXPECT_FALSE(book.best_bid().has_value());  // residual NOT inserted
    EXPECT_FALSE(book.best_ask().has_value());
}

TEST(OrderBook, MarketOrderOnEmptyBookDoesNothing) {
    OrderBook book;
    auto trades = book.submit(make_market(99, Side::Buy, 10));
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.size(), 0u);
}

TEST(OrderBook, ZeroQuantityRejected) {
    OrderBook book;
    auto trades = book.submit(make_limit(1, Side::Buy, 100'00, 0));
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.size(), 0u);
    EXPECT_FALSE(book.best_bid().has_value());
}

// Synthetic deterministic replay: random stream, assert invariants throughout.
TEST(OrderBook, ReplayInvariants) {
    OrderBook book;
    std::mt19937_64 rng(0xC0FFEE);
    std::uniform_int_distribution<int>      side_dist(0, 1);
    std::uniform_int_distribution<int>      type_dist(0, 9);  // 1-in-10 market
    std::uniform_int_distribution<Price>    price_dist(95'00, 105'00);
    std::uniform_int_distribution<Quantity> qty_dist(1, 10);

    constexpr int N = 5000;
    std::unordered_set<OrderId> live_limits;
    std::uint64_t buy_volume_filled  = 0;
    std::uint64_t sell_volume_filled = 0;

    for (int i = 1; i <= N; ++i) {
        Side side       = side_dist(rng) ? Side::Buy : Side::Sell;
        bool is_market  = type_dist(rng) == 0;
        Price px        = price_dist(rng);
        Quantity qty    = qty_dist(rng);
        Order o = is_market ? make_market(static_cast<OrderId>(i), side, qty, i)
                            : make_limit(static_cast<OrderId>(i), side, px, qty, i);

        const std::size_t size_before = book.size();
        auto trades = book.submit(o);

        // Per-trade: buy & sell volume must balance (each trade increments both).
        for (const auto& t : trades) {
            buy_volume_filled  += t.quantity;
            sell_volume_filled += t.quantity;
            EXPECT_GT(t.quantity, 0u);
        }

        // Crossed book invariant: never best_bid >= best_ask after a submit.
        auto bb = book.best_bid();
        auto ba = book.best_ask();
        if (bb && ba) {
            EXPECT_LT(*bb, *ba) << "crossed book after order " << i;
        }

        // Size accounting: book grew by at most 1, can shrink arbitrarily.
        EXPECT_LE(book.size(), size_before + 1) << "book grew by >1 on order " << i;

        if (!is_market) live_limits.insert(static_cast<OrderId>(i));
    }

    // Buy/sell filled volumes must always match (each Trade contributes equally).
    EXPECT_EQ(buy_volume_filled, sell_volume_filled);

    // Cancel every live limit. Most are gone (filled or cancelled); the rest
    // must drain to an empty book. Successful cancels must equal book.size().
    const std::size_t resting = book.size();
    std::size_t cancelled = 0;
    for (OrderId id : live_limits) {
        if (book.cancel(id)) ++cancelled;
    }
    EXPECT_EQ(cancelled, resting);
    EXPECT_EQ(book.size(), 0u);
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
}

}  // namespace tachyon

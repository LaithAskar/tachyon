#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "tachyon/matching_engine.hpp"
#include "tachyon/order_book.hpp"

namespace tachyon {

namespace {

Order make_limit(OrderId id, Side side, Price price, Quantity qty, TimestampNs ts = 0) {
    return Order{id, side, OrderType::Limit, price, qty, ts};
}

Order make_market(OrderId id, Side side, Quantity qty, TimestampNs ts = 0) {
    return Order{id, side, OrderType::Market, /*price=*/0, qty, ts};
}

Order make_limit_acct(OrderId id, Side side, Price price, Quantity qty,
                     AccountId account, TimestampNs ts = 0) {
    return Order{id, side, OrderType::Limit, price, qty, ts, account};
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

TEST(OrderBook, TopBidsHighestFirstAndAggregated) {
    OrderBook book;
    book.submit(make_limit(1, Side::Buy, 100'00, 10));
    book.submit(make_limit(2, Side::Buy, 101'00, 5));
    book.submit(make_limit(3, Side::Buy, 101'00, 3));  // same level as #2
    book.submit(make_limit(4, Side::Buy,  99'00, 20));

    auto top = book.top_bids(5);
    ASSERT_EQ(top.size(), 3u);
    EXPECT_EQ(top[0].price, 101'00);
    EXPECT_EQ(top[0].total_qty, 8u);
    EXPECT_EQ(top[0].order_count, 2u);
    EXPECT_EQ(top[1].price, 100'00);
    EXPECT_EQ(top[1].total_qty, 10u);
    EXPECT_EQ(top[1].order_count, 1u);
    EXPECT_EQ(top[2].price,  99'00);
}

TEST(OrderBook, TopAsksLowestFirstAndLimited) {
    OrderBook book;
    book.submit(make_limit(1, Side::Sell, 101'00, 10));
    book.submit(make_limit(2, Side::Sell, 100'00, 5));
    book.submit(make_limit(3, Side::Sell, 102'00, 7));

    auto top = book.top_asks(2);  // request fewer than available
    ASSERT_EQ(top.size(), 2u);
    EXPECT_EQ(top[0].price, 100'00);
    EXPECT_EQ(top[1].price, 101'00);
}

TEST(OrderBook, TopOnEmptyBookIsEmpty) {
    OrderBook book;
    EXPECT_TRUE(book.top_bids(10).empty());
    EXPECT_TRUE(book.top_asks(10).empty());
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

// --- Phase 2: MatchingEngine trade listener --------------------------------

TEST(MatchingEngine, TradeListenerFiresInMatchOrder) {
    MatchingEngine engine;
    std::vector<Trade> seen;
    engine.set_trade_listener([&](const Trade& t) { seen.push_back(t); });

    // Build the maker side, then submit a taker that produces 2 trades.
    engine.on_order(make_limit(1, Side::Sell, 100'00, 5, /*ts=*/1));
    engine.on_order(make_limit(2, Side::Sell, 101'00, 5, /*ts=*/2));
    ASSERT_TRUE(seen.empty());     // resting orders don't trade

    auto trades = engine.on_order(make_limit(99, Side::Buy, 101'00, 8));
    ASSERT_EQ(trades.size(), 2u);
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0].maker_id, 1u);
    EXPECT_EQ(seen[0].price,    100'00);
    EXPECT_EQ(seen[1].maker_id, 2u);
    EXPECT_EQ(seen[1].price,    101'00);
}

TEST(MatchingEngine, NoListenerInstalledIsSafe) {
    MatchingEngine engine;
    engine.on_order(make_limit(1, Side::Sell, 100'00, 5));
    auto trades = engine.on_order(make_limit(2, Side::Buy, 100'00, 5));
    EXPECT_EQ(trades.size(), 1u);   // just doesn't crash; no callback set
}

TEST(MatchingEngine, ListenerCanBeClearedMidStream) {
    MatchingEngine engine;
    int count = 0;
    engine.set_trade_listener([&](const Trade&) { ++count; });

    engine.on_order(make_limit(1, Side::Sell, 100'00, 5));
    engine.on_order(make_limit(2, Side::Buy,  100'00, 5));
    EXPECT_EQ(count, 1);

    engine.set_trade_listener({});                              // unsubscribe
    engine.on_order(make_limit(3, Side::Sell, 100'00, 5));
    engine.on_order(make_limit(4, Side::Buy,  100'00, 5));
    EXPECT_EQ(count, 1);   // unchanged
}

// --- Phase 2 refinement: JSON snapshot -------------------------------------

TEST(OrderBook, SnapshotJsonEmptyBook) {
    OrderBook book;
    EXPECT_EQ(book.snapshot_json(5),
              R"({"bid":null,"ask":null,"size":0,"bids":[],"asks":[]})");
}

TEST(OrderBook, SnapshotJsonOneEachSide) {
    OrderBook book;
    book.submit(make_limit(1, Side::Buy,  100'00, 5));
    book.submit(make_limit(2, Side::Sell, 101'00, 7));
    EXPECT_EQ(book.snapshot_json(5),
              R"({"bid":10000,"ask":10100,"size":2,)"
              R"("bids":[[10000,5,1]],"asks":[[10100,7,1]]})");
}

TEST(OrderBook, SnapshotJsonRespectsDepthAndOrdering) {
    OrderBook book;
    // Three bid levels, three ask levels. Snapshot depth=2 keeps top two.
    book.submit(make_limit(1, Side::Buy,  100'00, 5));
    book.submit(make_limit(2, Side::Buy,   99'00, 6));
    book.submit(make_limit(3, Side::Buy,   98'00, 7));
    book.submit(make_limit(4, Side::Sell, 101'00, 8));
    book.submit(make_limit(5, Side::Sell, 102'00, 9));
    book.submit(make_limit(6, Side::Sell, 103'00, 10));

    EXPECT_EQ(book.snapshot_json(2),
              R"({"bid":10000,"ask":10100,"size":6,)"
              R"("bids":[[10000,5,1],[9900,6,1]],)"
              R"("asks":[[10100,8,1],[10200,9,1]]})");
}

TEST(Trade, ToJson) {
    Trade t{42, 17, 10005, 250, 123456789};
    EXPECT_EQ(to_json(t),
              R"({"taker":42,"maker":17,"price":10005,"qty":250,"ts":123456789})");
}

// --- Phase 2 refinement: IOC and FOK ---------------------------------------

namespace {
Order make_ioc(OrderId id, Side side, Price price, Quantity qty) {
    return Order{id, side, OrderType::ImmediateOrCancel, price, qty, 0};
}
Order make_fok(OrderId id, Side side, Price price, Quantity qty) {
    return Order{id, side, OrderType::FillOrKill, price, qty, 0};
}
}  // namespace

TEST(OrderBook, IocFillsAvailableAndDropsResidual) {
    // Maker has 5 @ 100. IOC buy for 10 @ 100 — fills 5, the other 5 is dropped.
    OrderBook book;
    book.submit(make_limit(1, Side::Sell, 100'00, 5));
    auto trades = book.submit(make_ioc(2, Side::Buy, 100'00, 10));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 5u);
    EXPECT_EQ(book.size(), 0u);                  // maker fully consumed
    EXPECT_FALSE(book.best_bid().has_value());   // IOC residual NOT rested
}

TEST(OrderBook, IocNonCrossingDoesNothing) {
    // Best ask is 101; IOC buy at 100 doesn't cross. No trade, no rest.
    OrderBook book;
    book.submit(make_limit(1, Side::Sell, 101'00, 5));
    auto trades = book.submit(make_ioc(2, Side::Buy, 100'00, 5));
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.size(), 1u);                  // only the original maker
    EXPECT_FALSE(book.best_bid().has_value());
}

TEST(OrderBook, FokFillsIfFullQuantityAvailable) {
    // Two ask levels totalling 10 @ ≤ 101. FOK buy 10 @ 101 fills entirely.
    OrderBook book;
    book.submit(make_limit(1, Side::Sell, 100'00, 4));
    book.submit(make_limit(2, Side::Sell, 101'00, 6));
    auto trades = book.submit(make_fok(99, Side::Buy, 101'00, 10));
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].quantity, 4u);
    EXPECT_EQ(trades[1].quantity, 6u);
    EXPECT_EQ(book.size(), 0u);
    EXPECT_FALSE(book.best_ask().has_value());
}

TEST(OrderBook, FokKillsIfShortByOne) {
    // Available crossing liquidity at price 101 is 9; FOK asks for 10 → kill.
    OrderBook book;
    book.submit(make_limit(1, Side::Sell, 100'00, 4));
    book.submit(make_limit(2, Side::Sell, 101'00, 5));
    book.submit(make_limit(3, Side::Sell, 102'00, 100));  // out of reach

    auto trades = book.submit(make_fok(99, Side::Buy, 101'00, 10));
    EXPECT_TRUE(trades.empty());
    // Critical invariant: NOTHING was mutated. All three makers still there.
    EXPECT_EQ(book.size(), 3u);
    EXPECT_EQ(*book.best_ask(), 100'00);
}

TEST(OrderBook, FokOnEmptyBookKills) {
    OrderBook book;
    auto trades = book.submit(make_fok(1, Side::Buy, 100'00, 1));
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.size(), 0u);
}

TEST(OrderBook, FokRespectsSelfCrossInPreCheck) {
    // Visible liquidity is 10 @ 100, but all from the same account. FOK from
    // that account asks for 5 — pre-check must subtract own-account liquidity
    // and conclude there's nothing to fill against.
    OrderBook book;
    book.submit(make_limit_acct(1, Side::Sell, 100'00, 10, /*account=*/7));
    Order o{99, Side::Buy, OrderType::FillOrKill, 100'00, 5, 0, /*account=*/7};
    auto trades = book.submit(o);
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.size(), 1u);                  // maker still resting
}

// --- Phase 2 refinement: self-cross prevention -----------------------------

TEST(OrderBook, SelfCrossSameAccountDoesNotMatch) {
    // Account 7 sells then buys at the same price — must not match itself.
    OrderBook book;
    book.submit(make_limit_acct(1, Side::Sell, 100'00, 5, /*account=*/7));
    auto trades = book.submit(make_limit_acct(2, Side::Buy, 100'00, 5, 7));
    EXPECT_TRUE(trades.empty());
    // Both orders rest. Yes, this produces a "crossed" book (own bid == own ask)
    // — that's the documented STP behaviour: a single account is allowed to
    // sit on both sides of the spread, just never trade against itself.
    EXPECT_EQ(book.size(), 2u);
    ASSERT_TRUE(book.best_bid().has_value());
    ASSERT_TRUE(book.best_ask().has_value());
    EXPECT_EQ(*book.best_bid(), 100'00);
    EXPECT_EQ(*book.best_ask(), 100'00);
}

TEST(OrderBook, SelfCrossDifferentAccountsStillMatch) {
    OrderBook book;
    book.submit(make_limit_acct(1, Side::Sell, 100'00, 5, /*account=*/7));
    auto trades = book.submit(make_limit_acct(2, Side::Buy, 100'00, 5, /*account=*/8));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].taker_id, 2u);
    EXPECT_EQ(trades[0].maker_id, 1u);
    EXPECT_EQ(book.size(), 0u);
}

TEST(OrderBook, SelfCrossSkipsOwnPreservesFifoAmongOthers) {
    // Level 100.00 ask queue: [#1 own, #2 other, #3 own, #4 other].
    // A buy from the same account should consume #2 then #4, FIFO among others,
    // and leave #1 and #3 in the book.
    OrderBook book;
    book.submit(make_limit_acct(1, Side::Sell, 100'00, 5, /*account=*/7, /*ts=*/1));
    book.submit(make_limit_acct(2, Side::Sell, 100'00, 5, /*account=*/8, /*ts=*/2));
    book.submit(make_limit_acct(3, Side::Sell, 100'00, 5, /*account=*/7, /*ts=*/3));
    book.submit(make_limit_acct(4, Side::Sell, 100'00, 5, /*account=*/9, /*ts=*/4));

    auto trades = book.submit(make_limit_acct(99, Side::Buy, 100'00, 10,
                                              /*account=*/7));
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].maker_id, 2u);  // first non-self maker
    EXPECT_EQ(trades[1].maker_id, 4u);  // second non-self maker
    EXPECT_EQ(book.size(), 2u);          // #1 and #3 still resting
    EXPECT_EQ(*book.best_ask(), 100'00);
}

TEST(OrderBook, SelfCrossAccountZeroDoesNotPreventMatch) {
    // account_id == 0 means "unspecified" — self-cross prevention disabled.
    OrderBook book;
    book.submit(make_limit_acct(1, Side::Sell, 100'00, 5, /*account=*/0));
    auto trades = book.submit(make_limit_acct(2, Side::Buy, 100'00, 5, /*account=*/0));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(book.size(), 0u);
}

TEST(OrderBook, SelfCrossBlockedBestLevelStopsMatching) {
    // Best ask is all account 7. A second non-self level exists deeper, but the
    // documented STP behaviour is "stop at the first fully-blocked level" so
    // the deeper level is NOT reached. (Defensible choice; tests pin it.)
    OrderBook book;
    book.submit(make_limit_acct(1, Side::Sell, 100'00, 5, /*account=*/7));
    book.submit(make_limit_acct(2, Side::Sell, 101'00, 5, /*account=*/8));

    auto trades = book.submit(make_limit_acct(99, Side::Buy, 102'00, 10,
                                              /*account=*/7));
    EXPECT_TRUE(trades.empty());           // blocked at 100.00, doesn't reach 101.00
    EXPECT_EQ(book.size(), 3u);            // both makers still resting + new bid residual
    EXPECT_EQ(*book.best_bid(), 102'00);   // taker rests
    EXPECT_EQ(*book.best_ask(), 100'00);
}

// --- Phase 2 refinement: out-param submit() --------------------------------

TEST(OrderBook, OutParamSubmitClearsBufferAndAppends) {
    OrderBook book;
    book.submit(make_limit(1, Side::Sell, 100'00, 10));

    std::vector<Trade> buf;
    buf.push_back(Trade{99, 99, 1, 1, 0});   // stale entry — must be cleared.

    book.submit(make_limit(2, Side::Buy, 100'00, 4), buf);
    ASSERT_EQ(buf.size(), 1u);
    EXPECT_EQ(buf[0].taker_id, 2u);
    EXPECT_EQ(buf[0].maker_id, 1u);
    EXPECT_EQ(buf[0].quantity, 4u);
}

TEST(OrderBook, OutParamSubmitPreservesCapacityAcrossCalls) {
    // The whole point: hot-loop callers should not allocate per submit.
    OrderBook book;
    std::vector<Trade> buf;
    buf.reserve(64);
    const auto reserved_cap = buf.capacity();
    ASSERT_GE(reserved_cap, 64u);

    // Pre-seed several maker levels so a sweep produces multiple trades.
    for (int i = 0; i < 8; ++i) {
        book.submit(make_limit(static_cast<OrderId>(100 + i),
                               Side::Sell, 100'00 + i * 100, 1));
    }

    // Sweep all of them with a single buy that crosses every level.
    book.submit(make_limit(999, Side::Buy, 200'00, 8), buf);
    EXPECT_EQ(buf.size(), 8u);
    EXPECT_GE(buf.capacity(), reserved_cap);

    // A subsequent zero-trade submit clears but must NOT shrink capacity.
    book.submit(make_limit(1000, Side::Buy, 50'00, 1), buf);
    EXPECT_TRUE(buf.empty());
    EXPECT_GE(buf.capacity(), reserved_cap);
}

TEST(OrderBook, OutParamSubmitMatchesValueReturn) {
    // Same flow into two books; the out-param form must produce identical trades.
    auto make_book = [] {
        OrderBook b;
        b.submit(make_limit(1, Side::Sell, 100'00, 5));
        b.submit(make_limit(2, Side::Sell, 101'00, 5));
        b.submit(make_limit(3, Side::Sell, 102'00, 5));
        return b;
    };

    OrderBook a = make_book();
    OrderBook c = make_book();

    const auto trades_value = a.submit(make_limit(99, Side::Buy, 102'00, 12));

    std::vector<Trade> trades_out;
    c.submit(make_limit(99, Side::Buy, 102'00, 12), trades_out);

    ASSERT_EQ(trades_value.size(), trades_out.size());
    for (std::size_t i = 0; i < trades_value.size(); ++i) {
        EXPECT_EQ(trades_value[i].taker_id, trades_out[i].taker_id);
        EXPECT_EQ(trades_value[i].maker_id, trades_out[i].maker_id);
        EXPECT_EQ(trades_value[i].price,    trades_out[i].price);
        EXPECT_EQ(trades_value[i].quantity, trades_out[i].quantity);
    }
    EXPECT_EQ(a.size(), c.size());
    EXPECT_EQ(a.best_ask(), c.best_ask());
}

TEST(OrderBook, OutParamSubmitZeroQuantityClears) {
    OrderBook book;
    std::vector<Trade> buf{Trade{42, 42, 0, 0, 0}};
    book.submit(make_limit(1, Side::Buy, 100'00, 0), buf);
    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(book.size(), 0u);
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

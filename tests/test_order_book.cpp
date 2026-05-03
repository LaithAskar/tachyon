#include <gtest/gtest.h>

#include "tachyon/order_book.hpp"

namespace tachyon {

TEST(OrderBook, EmptyOnConstruction) {
    OrderBook book;
    EXPECT_EQ(book.size(), 0u);
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
}

// TODO(week 1): real tests once submit() is implemented.
//
// TEST(OrderBook, SingleBuyLimitBecomesBestBid) {
//     OrderBook book;
//     Order o{1, Side::Buy, OrderType::Limit, 100'00, 10, 0};
//     auto trades = book.submit(o);
//     EXPECT_TRUE(trades.empty());
//     EXPECT_EQ(book.size(), 1u);
//     ASSERT_TRUE(book.best_bid().has_value());
//     EXPECT_EQ(*book.best_bid(), 100'00);
// }
//
// TEST(OrderBook, CrossingOrdersGenerateTrade) { /* ... */ }
// TEST(OrderBook, FifoAtSamePriceLevel)        { /* ... */ }
// TEST(OrderBook, CancelRemovesRestingOrder)   { /* ... */ }

}  // namespace tachyon

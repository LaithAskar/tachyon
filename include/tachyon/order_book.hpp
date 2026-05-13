#pragma once

#include <cstddef>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "tachyon/order.hpp"
#include "tachyon/trade.hpp"

namespace tachyon {

class OrderBook {
public:
    OrderBook() = default;

    // Submit a new order. Returns trades produced by matching against the book.
    // For Limit orders, residual quantity (if any) is added to the resting book.
    // For Market orders, residual is dropped.
    // Zero-quantity orders are rejected and return no trades.
    std::vector<Trade> submit(const Order& order);

    // Cancel a resting order by id. Returns true if found and removed.
    bool cancel(OrderId id);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;

    // Aggregated depth view of a single price level.
    struct LevelView {
        Price       price;
        Quantity    total_qty;
        std::size_t order_count;
    };

    // Top N levels of each side, ordered "best first".
    // bids: highest price first. asks: lowest price first.
    std::vector<LevelView> top_bids(std::size_t n) const;
    std::vector<LevelView> top_asks(std::size_t n) const;

    std::size_t size() const noexcept { return total_orders_; }

private:
    // Ascending price -> FIFO queue of resting orders at that price.
    // Best bid is rbegin() (highest price), best ask is begin() (lowest).
    using BookSide = std::map<Price, std::list<Order>>;

    struct LevelHandle {
        Side                       side;
        BookSide::iterator         level_it;
        std::list<Order>::iterator order_it;
    };

    BookSide bids_;
    BookSide asks_;
    std::unordered_map<OrderId, LevelHandle> id_index_;

    std::size_t total_orders_ = 0;
};

}  // namespace tachyon

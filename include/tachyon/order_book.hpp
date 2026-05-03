#pragma once

#include <cstddef>
#include <optional>
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
    std::vector<Trade> submit(const Order& order);

    // Cancel a resting order by id. Returns true if found and removed.
    bool cancel(OrderId id);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;

    std::size_t size() const noexcept { return total_orders_; }

private:
    // TODO(week 1): replace these stubs with real structures.
    //   std::map<Price, std::deque<Order>, std::greater<>> bids_;
    //   std::map<Price, std::deque<Order>>                 asks_;
    //   std::unordered_map<OrderId, /* iterator into level deque */> id_index_;
    //
    // Order of operations: implement insert (limit, no cross) first, then best_*,
    // then cancel via id_index_, then matching against opposite book.
    std::size_t total_orders_ = 0;
};

}  // namespace tachyon

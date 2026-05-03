#pragma once

#include "tachyon/order_book.hpp"

namespace tachyon {

class MatchingEngine {
public:
    MatchingEngine() = default;

    std::vector<Trade> on_order(const Order& order);
    bool               on_cancel(OrderId id);

    const OrderBook& book() const noexcept { return book_; }

private:
    OrderBook book_;
};

}  // namespace tachyon

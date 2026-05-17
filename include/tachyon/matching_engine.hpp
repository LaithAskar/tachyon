#pragma once

#include <functional>

#include "tachyon/order_book.hpp"

namespace tachyon {

class MatchingEngine {
public:
    using TradeListener = std::function<void(const Trade&)>;

    MatchingEngine() = default;

    void               on_order(const Order& order, std::vector<Trade>& out);
    std::vector<Trade> on_order(const Order& order);
    bool               on_cancel(OrderId id);

    // Install (or clear, by passing nullptr/{}) a per-trade callback. Fires
    // once per Trade emitted by on_order, in match order. Intended for
    // streaming/UI layers — not the hot path.
    void set_trade_listener(TradeListener cb) { listener_ = std::move(cb); }

    const OrderBook& book() const noexcept { return book_; }

private:
    OrderBook     book_;
    TradeListener listener_;
};

}  // namespace tachyon

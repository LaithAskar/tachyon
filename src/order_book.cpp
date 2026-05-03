#include "tachyon/order_book.hpp"

namespace tachyon {

std::vector<Trade> OrderBook::submit(const Order& /*order*/) {
    // TODO(week 1-2): matching logic.
    //   1. Walk the opposite side from best price.
    //   2. While prices cross and incoming quantity remains:
    //        match against resting orders FIFO at each level,
    //        emit Trade, decrement maker, decrement taker.
    //   3. If incoming quantity remains and type==Limit: insert into this side.
    //   4. If type==Market and book exhausted: drop residual.
    return {};
}

bool OrderBook::cancel(OrderId /*id*/) {
    // TODO: lookup id_index_, erase from level deque, decrement total_orders_.
    return false;
}

std::optional<Price> OrderBook::best_bid() const {
    // TODO: return bids_.begin()->first if non-empty.
    return std::nullopt;
}

std::optional<Price> OrderBook::best_ask() const {
    // TODO: return asks_.begin()->first if non-empty.
    return std::nullopt;
}

}  // namespace tachyon

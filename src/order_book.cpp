#include "tachyon/order_book.hpp"

#include <algorithm>
#include <iterator>

namespace tachyon {

namespace {

bool crosses(Side taker_side, OrderType taker_type, Price taker_price, Price best_opposite) {
    if (taker_type == OrderType::Market) return true;
    return taker_side == Side::Buy ? taker_price >= best_opposite
                                   : taker_price <= best_opposite;
}

}  // namespace

std::vector<Trade> OrderBook::submit(const Order& order) {
    std::vector<Trade> trades;
    if (order.quantity == 0) return trades;

    Order working = order;  // mutable copy for residual tracking

    BookSide& opposite = (working.side == Side::Buy) ? asks_ : bids_;

    while (working.quantity > 0 && !opposite.empty()) {
        // Best opposite level: lowest ask (begin) or highest bid (prev(end)).
        BookSide::iterator best_it =
            (working.side == Side::Buy) ? opposite.begin() : std::prev(opposite.end());
        const Price best_px = best_it->first;

        if (!crosses(working.side, working.type, working.price, best_px)) break;

        std::list<Order>& level = best_it->second;
        while (working.quantity > 0 && !level.empty()) {
            Order& resting = level.front();
            const Quantity fill = std::min(working.quantity, resting.quantity);

            trades.push_back(Trade{
                /*taker_id=*/working.id,
                /*maker_id=*/resting.id,
                /*price=*/best_px,
                /*quantity=*/fill,
                /*timestamp_ns=*/working.timestamp_ns,
            });

            working.quantity -= fill;
            resting.quantity -= fill;

            if (resting.quantity == 0) {
                id_index_.erase(resting.id);
                level.pop_front();
                --total_orders_;
            }
        }

        if (level.empty()) {
            opposite.erase(best_it);
        }
    }

    if (working.quantity > 0 && working.type == OrderType::Limit) {
        BookSide& own = (working.side == Side::Buy) ? bids_ : asks_;
        BookSide::iterator level_it = own.try_emplace(working.price).first;
        level_it->second.push_back(working);
        std::list<Order>::iterator order_it = std::prev(level_it->second.end());
        id_index_.emplace(working.id, LevelHandle{working.side, level_it, order_it});
        ++total_orders_;
    }

    return trades;
}

bool OrderBook::cancel(OrderId id) {
    auto idx_it = id_index_.find(id);
    if (idx_it == id_index_.end()) return false;

    const LevelHandle h = idx_it->second;
    BookSide& side_book = (h.side == Side::Buy) ? bids_ : asks_;

    h.level_it->second.erase(h.order_it);
    if (h.level_it->second.empty()) {
        side_book.erase(h.level_it);
    }
    id_index_.erase(idx_it);
    --total_orders_;
    return true;
}

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.rbegin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

}  // namespace tachyon

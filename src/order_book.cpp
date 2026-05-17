#include "tachyon/order_book.hpp"

#include <algorithm>
#include <iterator>
#include <string>

namespace tachyon {

std::string to_json(const Trade& t) {
    // Hand-built to avoid pulling in a JSON dep. All fields are integers, so
    // no escaping is needed.
    std::string s;
    s.reserve(96);
    s += R"({"taker":)"; s += std::to_string(t.taker_id);
    s += R"(,"maker":)"; s += std::to_string(t.maker_id);
    s += R"(,"price":)"; s += std::to_string(t.price);
    s += R"(,"qty":)";   s += std::to_string(t.quantity);
    s += R"(,"ts":)";    s += std::to_string(t.timestamp_ns);
    s += '}';
    return s;
}

namespace {

// Market is unconditional; Limit/IOC/FOK obey the price bound.
bool crosses(Side taker_side, OrderType taker_type, Price taker_price, Price best_opposite) {
    if (taker_type == OrderType::Market) return true;
    return taker_side == Side::Buy ? taker_price >= best_opposite
                                   : taker_price <= best_opposite;
}

// FOK pre-check: walk the opposite book best→worst with the same semantics as
// the real matching loop (skip same-account makers; stop at a level that still
// has same-account leftovers after consuming non-self). Returns true iff at
// least `need` quantity is available to the taker without any state mutation.
bool fok_can_fill(const Order& taker,
                  const std::map<Price, std::list<Order>>& opposite) {
    const Quantity need = taker.quantity;
    Quantity have = 0;
    auto eat_level = [&](Price px, const std::list<Order>& level) -> int {
        // 0 = continue, 1 = enough, 2 = stop (insufficient)
        const bool ok = (taker.side == Side::Buy) ? taker.price >= px
                                                  : taker.price <= px;
        if (!ok) return 2;
        Quantity non_self = 0;
        bool has_self = false;
        for (const Order& o : level) {
            if (taker.account_id != 0 && o.account_id == taker.account_id) {
                has_self = true;
            } else {
                non_self += o.quantity;
            }
        }
        have += non_self;
        if (have >= need) return 1;
        if (has_self) return 2;   // matching would stop at this level
        return 0;
    };
    if (taker.side == Side::Buy) {
        for (const auto& kv : opposite) {
            int r = eat_level(kv.first, kv.second);
            if (r == 1) return true;
            if (r == 2) return false;
        }
    } else {
        for (auto it = opposite.rbegin(); it != opposite.rend(); ++it) {
            int r = eat_level(it->first, it->second);
            if (r == 1) return true;
            if (r == 2) return false;
        }
    }
    return have >= need;
}

}  // namespace

void OrderBook::submit(const Order& order, std::vector<Trade>& out) {
    out.clear();
    if (order.quantity == 0) return;

    Order working = order;  // mutable copy for residual tracking

    BookSide& opposite = (working.side == Side::Buy) ? asks_ : bids_;

    // FOK: all-or-nothing. If the book can't fill the entire quantity at the
    // limit price (under our self-cross semantics), do nothing.
    if (working.type == OrderType::FillOrKill && !fok_can_fill(working, opposite)) {
        return;
    }

    while (working.quantity > 0 && !opposite.empty()) {
        // Best opposite level: lowest ask (begin) or highest bid (prev(end)).
        BookSide::iterator best_it =
            (working.side == Side::Buy) ? opposite.begin() : std::prev(opposite.end());
        const Price best_px = best_it->first;

        if (!crosses(working.side, working.type, working.price, best_px)) break;

        std::list<Order>& level = best_it->second;
        // Walk the level FIFO. Same-account orders are skipped (self-cross
        // prevention) without altering their queue position for other takers.
        auto it = level.begin();
        while (working.quantity > 0 && it != level.end()) {
            Order& resting = *it;
            const bool self_cross = working.account_id != 0 &&
                                    resting.account_id == working.account_id;
            if (self_cross) {
                ++it;
                continue;
            }

            const Quantity fill = std::min(working.quantity, resting.quantity);

            out.push_back(Trade{
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
                it = level.erase(it);
                --total_orders_;
            }
            // else: partial fill — `it` stays put, but `working.quantity` is
            // now 0 because we always fill min(taker,maker), so we'll exit.
        }

        if (level.empty()) {
            opposite.erase(best_it);
        } else if (working.quantity > 0) {
            // The level still has orders, but they're all this account's own
            // makers (self-cross blocked) and we couldn't consume any of them.
            // Stop matching: the cross condition would loop forever otherwise.
            break;
        }
    }

    // Only plain Limit rests. Market/IOC/FOK all drop their residual.
    if (working.quantity > 0 && working.type == OrderType::Limit) {
        BookSide& own = (working.side == Side::Buy) ? bids_ : asks_;
        BookSide::iterator level_it = own.try_emplace(working.price).first;
        level_it->second.push_back(working);
        std::list<Order>::iterator order_it = std::prev(level_it->second.end());
        id_index_.emplace(working.id, LevelHandle{working.side, level_it, order_it});
        ++total_orders_;
    }
}

std::vector<Trade> OrderBook::submit(const Order& order) {
    std::vector<Trade> trades;
    submit(order, trades);
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

std::vector<OrderBook::LevelView> OrderBook::top_bids(std::size_t n) const {
    std::vector<LevelView> out;
    out.reserve(std::min(n, bids_.size()));
    auto it = bids_.rbegin();
    for (std::size_t i = 0; i < n && it != bids_.rend(); ++i, ++it) {
        Quantity total = 0;
        for (const Order& o : it->second) total += o.quantity;
        out.push_back(LevelView{it->first, total, it->second.size()});
    }
    return out;
}

std::vector<OrderBook::LevelView> OrderBook::top_asks(std::size_t n) const {
    std::vector<LevelView> out;
    out.reserve(std::min(n, asks_.size()));
    auto it = asks_.begin();
    for (std::size_t i = 0; i < n && it != asks_.end(); ++i, ++it) {
        Quantity total = 0;
        for (const Order& o : it->second) total += o.quantity;
        out.push_back(LevelView{it->first, total, it->second.size()});
    }
    return out;
}

std::string OrderBook::snapshot_json(std::size_t depth) const {
    const auto bids = top_bids(depth);
    const auto asks = top_asks(depth);
    auto write_levels = [](std::string& s, const std::vector<LevelView>& lv) {
        s += '[';
        for (std::size_t i = 0; i < lv.size(); ++i) {
            if (i != 0) s += ',';
            s += '[';
            s += std::to_string(lv[i].price);     s += ',';
            s += std::to_string(lv[i].total_qty); s += ',';
            s += std::to_string(lv[i].order_count);
            s += ']';
        }
        s += ']';
    };

    std::string s;
    s.reserve(64 + (bids.size() + asks.size()) * 24);

    s += R"({"bid":)";
    if (auto bb = best_bid()) s += std::to_string(*bb);
    else                      s += "null";

    s += R"(,"ask":)";
    if (auto ba = best_ask()) s += std::to_string(*ba);
    else                      s += "null";

    s += R"(,"size":)";  s += std::to_string(total_orders_);
    s += R"(,"bids":)";  write_levels(s, bids);
    s += R"(,"asks":)";  write_levels(s, asks);
    s += '}';
    return s;
}

}  // namespace tachyon

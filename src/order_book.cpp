#include "tachyon/order_book.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <string>

#if defined(_MSC_VER)
#  include <intrin.h>
#endif

namespace tachyon {

// ---- Trade::to_json (lives here so we don't need a separate TU) -----------
std::string to_json(const Trade& t) {
    std::string s;
    s.reserve(96);
    s += R"({"taker":)"; s += std::to_string(t.taker_id);
    s += R"(,"maker":)"; s += std::to_string(t.maker_id);
    s += R"(,"price":)"; s += std::to_string(t.price);
    s += R"(,"qty":)";   s += std::to_string(t.quantity);
    s += R"(,"ts":)";    s += std::to_string(t.timestamp_ns);
    if (t.symbol_id != 0) {                       // omit for single-symbol callers
        s += R"(,"sym":)"; s += std::to_string(t.symbol_id);
    }
    s += '}';
    return s;
}

// ---- bit-scan helpers -----------------------------------------------------
namespace {

// Highest set bit position in a non-zero 64-bit word, 0..63.
inline int hi_bit(std::uint64_t w) {
#if defined(_MSC_VER)
    unsigned long idx;
    _BitScanReverse64(&idx, w);
    return static_cast<int>(idx);
#else
    return 63 - __builtin_clzll(w);
#endif
}

// Lowest set bit position in a non-zero 64-bit word, 0..63.
inline int lo_bit(std::uint64_t w) {
#if defined(_MSC_VER)
    unsigned long idx;
    _BitScanForward64(&idx, w);
    return static_cast<int>(idx);
#else
    return __builtin_ctzll(w);
#endif
}

bool crosses(Side taker_side, OrderType taker_type, Price taker_price, Price best_opposite) {
    if (taker_type == OrderType::Market) return true;
    return taker_side == Side::Buy ? taker_price >= best_opposite
                                   : taker_price <= best_opposite;
}

}  // namespace

// ---- ctor / span setup ----------------------------------------------------
namespace {

// The documented flat-ladder design budgets about 12 MB per book for the
// default 200,001 price levels (two ladders plus their bitmaps). Keep custom
// ranges within that same supported allocation and scan-time envelope.
constexpr std::uint64_t kMaxSupportedSpan = 200'001;

std::size_t checked_span(Price tick_min, Price tick_max) {
    if (tick_max < tick_min) {
        throw std::invalid_argument("OrderBook: tick_max < tick_min");
    }

    // Convert before subtracting so ranges crossing zero cannot overflow signed
    // Price arithmetic. The subtraction is modulo 2^64, which equals the
    // mathematical distance for an ordered pair of int64 prices.
    const auto distance = static_cast<std::uint64_t>(tick_max)
                        - static_cast<std::uint64_t>(tick_min);
    if (distance >= kMaxSupportedSpan) {
        throw std::invalid_argument("OrderBook: tick range exceeds supported span");
    }
    return static_cast<std::size_t>(distance + 1);
}

}  // namespace

OrderBook::OrderBook(Price tick_min, Price tick_max)
    : tick_min_(tick_min),
      tick_max_(tick_max),
      span_(checked_span(tick_min, tick_max)),
      bid_levels_(span_),
      ask_levels_(span_),
      bid_bitmap_((span_ + 63) / 64, 0ull),
      ask_bitmap_((span_ + 63) / 64, 0ull),
      best_ask_idx_(static_cast<std::int64_t>(span_)) {}

// ---- bitmap ops -----------------------------------------------------------
void OrderBook::set_bit(std::vector<std::uint64_t>& bm, std::size_t idx) {
    bm[idx >> 6] |= (1ull << (idx & 63));
}

void OrderBook::clear_bit(std::vector<std::uint64_t>& bm, std::size_t idx) {
    bm[idx >> 6] &= ~(1ull << (idx & 63));
}

// Find highest set bit at or below idx_hint (-1 if none).
std::int64_t OrderBook::find_prev_set(const std::vector<std::uint64_t>& bm,
                                      std::int64_t idx_hint) const {
    if (idx_hint < 0) return -1;
    std::size_t word_idx = static_cast<std::size_t>(idx_hint) >> 6;
    int         bit_off  = static_cast<int>(idx_hint & 63);
    // Mask off bits ABOVE bit_off in the starting word, then scan word-by-word.
    std::uint64_t mask = (bit_off == 63) ? ~0ull : ((1ull << (bit_off + 1)) - 1ull);
    std::uint64_t w    = bm[word_idx] & mask;
    while (true) {
        if (w != 0ull) {
            return static_cast<std::int64_t>(word_idx) * 64
                 + static_cast<std::int64_t>(hi_bit(w));
        }
        if (word_idx == 0) return -1;
        --word_idx;
        w = bm[word_idx];
    }
}

// Find lowest set bit at or above idx_hint (span_ if none).
std::int64_t OrderBook::find_next_set(const std::vector<std::uint64_t>& bm,
                                      std::int64_t idx_hint) const {
    if (idx_hint < 0) idx_hint = 0;
    if (static_cast<std::size_t>(idx_hint) >= span_) return static_cast<std::int64_t>(span_);
    std::size_t word_idx = static_cast<std::size_t>(idx_hint) >> 6;
    int         bit_off  = static_cast<int>(idx_hint & 63);
    std::uint64_t mask = (bit_off == 0) ? ~0ull : (~0ull << bit_off);
    std::uint64_t w    = bm[word_idx] & mask;
    const std::size_t last_word = bm.size() - 1;
    while (true) {
        if (w != 0ull) {
            std::int64_t pos = static_cast<std::int64_t>(word_idx) * 64
                             + static_cast<std::int64_t>(lo_bit(w));
            if (static_cast<std::size_t>(pos) >= span_) return static_cast<std::int64_t>(span_);
            return pos;
        }
        if (word_idx == last_word) return static_cast<std::int64_t>(span_);
        ++word_idx;
        w = bm[word_idx];
    }
}

// ---- intrusive list ops ---------------------------------------------------
void OrderBook::link_back(Level& lv, OrderNode* n) {
    n->prev = lv.tail;
    n->next = nullptr;
    if (lv.tail) lv.tail->next = n;
    else         lv.head       = n;
    lv.tail        = n;
    lv.total_qty  += n->order.quantity;
    ++lv.order_count;
}

void OrderBook::unlink(Level& lv, OrderNode* n) {
    if (n->prev) n->prev->next = n->next;
    else         lv.head       = n->next;
    if (n->next) n->next->prev = n->prev;
    else         lv.tail       = n->prev;
    lv.total_qty -= n->order.quantity;
    --lv.order_count;
    n->prev = n->next = nullptr;
}

// ---- submit ---------------------------------------------------------------
void OrderBook::submit(const Order& order, std::vector<Trade>& out) {
    out.clear();
    // IDs identify live orders. Reject duplicates before matching so a second
    // order cannot trade or rest while cancel(id) still points at the first.
    if (id_index_.find(order.id) != id_index_.end()) return;
    if (order.quantity == 0) return;
    // Limit orders outside the configured tick range are rejected silently
    // (would need an extension to the ladder; not in scope for v1).
    if (order.type != OrderType::Market && !price_in_range(order.price)) return;

    Order working = order;

    const bool taker_is_buy = (working.side == Side::Buy);
    auto&        opp_levels = taker_is_buy ? ask_levels_ : bid_levels_;
    auto&        opp_bitmap = taker_is_buy ? ask_bitmap_ : bid_bitmap_;
    std::int64_t& opp_best  = taker_is_buy ? best_ask_idx_ : best_bid_idx_;

    // FOK pre-check: simulate the walk without mutating state.
    if (working.type == OrderType::FillOrKill) {
        Quantity have = 0;
        std::int64_t cur = opp_best;
        const auto stop_sentinel = taker_is_buy
            ? static_cast<std::int64_t>(span_) : static_cast<std::int64_t>(-1);
        while (cur != stop_sentinel) {
            const Price level_px = idx_to_price(static_cast<std::size_t>(cur));
            if (!crosses(working.side, working.type, working.price, level_px)) break;
            const Level& lv = opp_levels[static_cast<std::size_t>(cur)];
            Quantity non_self = 0;
            bool     has_self = false;
            for (OrderNode* n = lv.head; n != nullptr; n = n->next) {
                if (working.account_id != 0 &&
                    n->order.account_id == working.account_id) {
                    has_self = true;
                } else {
                    non_self += n->order.quantity;
                }
            }
            have += non_self;
            if (have >= working.quantity) break;
            if (has_self) { have = 0; break; }   // matching would stop at this level
            cur = taker_is_buy
                ? find_next_set(opp_bitmap, cur + 1)
                : find_prev_set(opp_bitmap, cur - 1);
        }
        if (have < working.quantity) return;
    }

    // ---- main matching loop --------------------------------------------------
    const auto stop_sentinel = taker_is_buy
        ? static_cast<std::int64_t>(span_) : static_cast<std::int64_t>(-1);

    while (working.quantity > 0 && opp_best != stop_sentinel) {
        const std::size_t idx     = static_cast<std::size_t>(opp_best);
        const Price       best_px = idx_to_price(idx);

        if (!crosses(working.side, working.type, working.price, best_px)) break;

        Level& level = opp_levels[idx];
        OrderNode* n = level.head;
        bool       did_fill_at_level = false;

        while (working.quantity > 0 && n != nullptr) {
            OrderNode* next = n->next;
            const bool self_cross = working.account_id != 0 &&
                                    n->order.account_id == working.account_id;
            if (self_cross) { n = next; continue; }

            const Quantity fill = std::min(working.quantity, n->order.quantity);

            out.push_back(Trade{
                /*taker_id=*/working.id,
                /*maker_id=*/n->order.id,
                /*price=*/best_px,
                /*quantity=*/fill,
                /*timestamp_ns=*/working.timestamp_ns,
            });

            working.quantity   -= fill;
            n->order.quantity  -= fill;
            level.total_qty    -= fill;
            did_fill_at_level   = true;

            if (n->order.quantity == 0) {
                id_index_.erase(n->order.id);
                unlink(level, n);
                pool_.release(n);
                --total_orders_;
            }
            n = next;
        }

        if (level.empty()) {
            clear_bit(opp_bitmap, idx);
            // Advance best_idx to the next non-empty level on this side.
            opp_best = taker_is_buy
                ? find_next_set(opp_bitmap, static_cast<std::int64_t>(idx) + 1)
                : find_prev_set(opp_bitmap, static_cast<std::int64_t>(idx) - 1);
        } else if (!did_fill_at_level) {
            // Level non-empty AND we matched nothing this pass → fully blocked
            // by self-cross. Stop, matching deeper would violate price priority.
            break;
        }
        // else: level not empty but we did fill some — must be that taker is
        // fully consumed (working.quantity == 0), outer loop will exit.
    }

    // ---- residual handling --------------------------------------------------
    if (working.quantity > 0 && working.type == OrderType::Limit) {
        const std::size_t idx = price_to_idx(working.price);
        Level& own_level = taker_is_buy ? bid_levels_[idx] : ask_levels_[idx];
        OrderNode* n = pool_.acquire();
        n->order = working;

        // Establish the ID index before making the node visible in the book.
        // This keeps list/bitmap/size state unchanged if insertion is rejected.
        try {
            const auto inserted = id_index_.emplace(working.id, n).second;
            if (!inserted) {
                pool_.release(n);
                return;
            }
        } catch (...) {
            pool_.release(n);
            throw;
        }

        link_back(own_level, n);

        auto& own_bitmap = taker_is_buy ? bid_bitmap_ : ask_bitmap_;
        const bool was_first = (own_level.head == n);  // we link at back, so head==n only if list was empty
        if (was_first) set_bit(own_bitmap, idx);

        if (taker_is_buy) {
            if (static_cast<std::int64_t>(idx) > best_bid_idx_) {
                best_bid_idx_ = static_cast<std::int64_t>(idx);
            }
        } else {
            if (static_cast<std::int64_t>(idx) < best_ask_idx_) {
                best_ask_idx_ = static_cast<std::int64_t>(idx);
            }
        }
        ++total_orders_;
    }
}

std::vector<Trade> OrderBook::submit(const Order& order) {
    std::vector<Trade> trades;
    submit(order, trades);
    return trades;
}

// ---- cancel ---------------------------------------------------------------
bool OrderBook::cancel(OrderId id) {
    auto it = id_index_.find(id);
    if (it == id_index_.end()) return false;

    OrderNode* n     = it->second;
    const Side side  = n->order.side;
    const std::size_t idx = price_to_idx(n->order.price);

    auto& levels = (side == Side::Buy) ? bid_levels_ : ask_levels_;
    auto& bitmap = (side == Side::Buy) ? bid_bitmap_ : ask_bitmap_;
    Level& level = levels[idx];

    unlink(level, n);
    pool_.release(n);
    id_index_.erase(it);
    --total_orders_;

    if (level.empty()) {
        clear_bit(bitmap, idx);
        if (side == Side::Buy && static_cast<std::int64_t>(idx) == best_bid_idx_) {
            best_bid_idx_ = find_prev_set(bitmap, best_bid_idx_ - 1);
        } else if (side == Side::Sell && static_cast<std::int64_t>(idx) == best_ask_idx_) {
            best_ask_idx_ = find_next_set(bitmap, best_ask_idx_ + 1);
        }
    }
    return true;
}

// ---- best / depth / snapshot ---------------------------------------------
std::optional<Price> OrderBook::best_bid() const {
    if (best_bid_idx_ < 0) return std::nullopt;
    return idx_to_price(static_cast<std::size_t>(best_bid_idx_));
}

std::optional<Price> OrderBook::best_ask() const {
    if (static_cast<std::size_t>(best_ask_idx_) >= span_) return std::nullopt;
    return idx_to_price(static_cast<std::size_t>(best_ask_idx_));
}

std::vector<OrderBook::LevelView> OrderBook::top_bids(std::size_t n) const {
    std::vector<LevelView> out;
    if (best_bid_idx_ < 0 || n == 0) return out;
    out.reserve(n);
    std::int64_t cur = best_bid_idx_;
    while (out.size() < n && cur >= 0) {
        const Level& lv = bid_levels_[static_cast<std::size_t>(cur)];
        if (!lv.empty()) {
            out.push_back(LevelView{idx_to_price(static_cast<std::size_t>(cur)),
                                    lv.total_qty, lv.order_count});
            if (out.size() == n) break;
        }
        cur = find_prev_set(bid_bitmap_, cur - 1);
    }
    return out;
}

std::vector<OrderBook::LevelView> OrderBook::top_asks(std::size_t n) const {
    std::vector<LevelView> out;
    if (static_cast<std::size_t>(best_ask_idx_) >= span_ || n == 0) return out;
    out.reserve(n);
    std::int64_t cur = best_ask_idx_;
    while (out.size() < n && static_cast<std::size_t>(cur) < span_) {
        const Level& lv = ask_levels_[static_cast<std::size_t>(cur)];
        if (!lv.empty()) {
            out.push_back(LevelView{idx_to_price(static_cast<std::size_t>(cur)),
                                    lv.total_qty, lv.order_count});
            if (out.size() == n) break;
        }
        cur = find_next_set(ask_bitmap_, cur + 1);
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

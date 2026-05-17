#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "tachyon/order.hpp"
#include "tachyon/pool.hpp"
#include "tachyon/trade.hpp"

namespace tachyon {

// Limit order book with a flat tick-indexed ladder and intrusive linked lists
// per level, backed by a pool allocator. Designed so the hot path never
// touches a tree node, never allocates, and walks at most one cache line per
// level visit.
//
// Indexing: each side is a std::vector<Level> sized to (tick_max - tick_min + 1).
// A uint64 bitmap per side flags non-empty levels so we can find the next
// best level after a drain in O(span/64) words via _BitScanReverse/clz.
class OrderBook {
public:
    // Default range covers $0.00 - $2000.00 for $0.01-tick instruments.
    // Memory: ~5MB per side of mostly-empty Level slots. Empty slots are
    // never touched (bitmap routes around them) so they don't pollute cache.
    OrderBook() : OrderBook(/*tick_min=*/0, /*tick_max=*/200'000) {}
    OrderBook(Price tick_min, Price tick_max);

    void submit(const Order& order, std::vector<Trade>& out);
    std::vector<Trade> submit(const Order& order);

    bool cancel(OrderId id);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;

    struct LevelView {
        Price       price;
        Quantity    total_qty;
        std::size_t order_count;
    };

    std::vector<LevelView> top_bids(std::size_t n) const;
    std::vector<LevelView> top_asks(std::size_t n) const;

    std::string snapshot_json(std::size_t depth) const;

    std::size_t size() const noexcept { return total_orders_; }

private:
    struct Level {
        OrderNode*  head        = nullptr;
        OrderNode*  tail        = nullptr;
        Quantity    total_qty   = 0;
        std::size_t order_count = 0;

        bool empty() const noexcept { return head == nullptr; }
    };

    // -- bitmap helpers --
    void set_bit  (std::vector<std::uint64_t>& bm, std::size_t idx);
    void clear_bit(std::vector<std::uint64_t>& bm, std::size_t idx);
    // Find the highest set bit at or below idx_hint. Returns -1 if none.
    std::int64_t find_prev_set(const std::vector<std::uint64_t>& bm,
                               std::int64_t idx_hint) const;
    // Find the lowest set bit at or above idx_hint. Returns span_ if none.
    std::int64_t find_next_set(const std::vector<std::uint64_t>& bm,
                               std::int64_t idx_hint) const;

    // -- level list ops (intrusive doubly-linked) --
    void link_back (Level& lv, OrderNode* n);
    void unlink    (Level& lv, OrderNode* n);

    // -- index <-> price --
    std::size_t price_to_idx(Price p) const {
        return static_cast<std::size_t>(p - tick_min_);
    }
    Price idx_to_price(std::size_t i) const {
        return tick_min_ + static_cast<Price>(i);
    }
    bool price_in_range(Price p) const {
        return p >= tick_min_ && p <= tick_max_;
    }

    // -- members --
    Price       tick_min_;
    Price       tick_max_;
    std::size_t span_;                 // tick_max_ - tick_min_ + 1

    std::vector<Level>         bid_levels_;
    std::vector<Level>         ask_levels_;
    std::vector<std::uint64_t> bid_bitmap_;   // bit i = 1 iff bid_levels_[i].non-empty
    std::vector<std::uint64_t> ask_bitmap_;

    std::int64_t best_bid_idx_ = -1;          // -1 = empty side
    std::int64_t best_ask_idx_;               // == span_ when empty

    std::unordered_map<OrderId, OrderNode*> id_index_;

    Pool        pool_;
    std::size_t total_orders_ = 0;
};

}  // namespace tachyon

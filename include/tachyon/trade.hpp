#pragma once

#include <string>

#include "tachyon/types.hpp"

namespace tachyon {

struct Trade {
    OrderId     taker_id;
    OrderId     maker_id;
    Price       price;
    Quantity    quantity;
    TimestampNs timestamp_ns;
    SymbolId    symbol_id = 0;   // Exchange stamps this; OrderBook leaves it 0
};

// Compact one-line JSON form. Useful for streaming each Trade as a single
// WebSocket frame.
std::string to_json(const Trade& trade);

}  // namespace tachyon

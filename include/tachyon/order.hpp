#pragma once

#include "tachyon/types.hpp"

namespace tachyon {

struct Order {
    OrderId     id;
    Side        side;
    OrderType   type;
    Price       price;        // ignored for OrderType::Market
    Quantity    quantity;     // remaining quantity
    TimestampNs timestamp_ns;
};

}  // namespace tachyon

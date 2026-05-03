#pragma once

#include "tachyon/types.hpp"

namespace tachyon {

struct Trade {
    OrderId     taker_id;
    OrderId     maker_id;
    Price       price;
    Quantity    quantity;
    TimestampNs timestamp_ns;
};

}  // namespace tachyon

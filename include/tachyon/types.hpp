#pragma once

#include <cstdint>

namespace tachyon {

using OrderId     = std::uint64_t;
using Price       = std::int64_t;    // integer ticks, never floating point
using Quantity    = std::uint64_t;
using TimestampNs = std::uint64_t;

enum class Side : std::uint8_t {
    Buy,
    Sell,
};

enum class OrderType : std::uint8_t {
    Limit,
    Market,
};

}  // namespace tachyon

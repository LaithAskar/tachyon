#pragma once

#include <cstdint>

namespace tachyon {

using OrderId     = std::uint64_t;
using Price       = std::int64_t;    // integer ticks, never floating point
using Quantity    = std::uint64_t;
using TimestampNs = std::uint64_t;
using AccountId   = std::uint64_t;   // 0 means "no account specified"
using SymbolId    = std::uint32_t;   // index into Exchange's book vector; single-symbol callers use 0

enum class Side : std::uint8_t {
    Buy,
    Sell,
};

enum class OrderType : std::uint8_t {
    Limit,              // GTC limit: rests if not fully filled
    Market,             // crosses at any price; residual dropped
    ImmediateOrCancel,  // limit-priced; residual dropped (no resting)
    FillOrKill,         // all-or-nothing at limit price, else zero state change
};

}  // namespace tachyon

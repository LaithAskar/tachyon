#pragma once

#include <cstdint>

#include "tachyon/order.hpp"
#include "tachyon/types.hpp"

namespace tachyon {

// Wire format for messages crossing the SPSC boundary into the matcher thread.
// Hand-rolled tagged union (not std::variant) because:
//   1. SpscQueue<T> requires T to be default-constructible — the slot array is
//      value-initialised. std::variant's default is the first alternative,
//      which works but couples the tag layout to ordering.
//   2. Flat POD layout keeps the size predictable (~64B) and copy/move cheap;
//      the matcher path is the one place where allocator/variant overhead
//      would actually show up in a microbench.
//
// `order` is meaningful iff `kind == Submit`; `order_id` iff `kind == Cancel`.
// The unused field is left default-initialised. No invariant is enforced on
// the wrong-field-for-the-tag pattern — readers must dispatch on `kind`.
struct EngineMessage {
    enum class Kind : std::uint8_t {
        Submit = 0,
        Cancel = 1,
    };

    Kind     kind      = Kind::Submit;
    SymbolId symbol_id = 0;     // routed by Exchange; ignored by ThreadedMatcher
    Order    order     = {};
    OrderId  order_id  = 0;

    static EngineMessage make_submit(const Order& o, SymbolId sym = 0) noexcept {
        EngineMessage m;
        m.kind      = Kind::Submit;
        m.symbol_id = sym;
        m.order     = o;
        return m;
    }

    static EngineMessage make_cancel(OrderId id, SymbolId sym = 0) noexcept {
        EngineMessage m;
        m.kind      = Kind::Cancel;
        m.symbol_id = sym;
        m.order_id  = id;
        return m;
    }
};

}  // namespace tachyon

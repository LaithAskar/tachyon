#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

#include "tachyon/engine_message.hpp"
#include "tachyon/order.hpp"
#include "tachyon/order_book.hpp"
#include "tachyon/spsc_queue.hpp"
#include "tachyon/trade.hpp"
#include "tachyon/types.hpp"

namespace tachyon {

// Multi-symbol matching engine: a single worker thread fed by a lock-free
// inbound SPSC queue, dispatching each message to the right OrderBook by
// symbol_id, and emitting Trade events (stamped with their symbol) through a
// lock-free outbound SPSC queue. The N order books are themselves
// single-threaded — only the worker ever reaches into any of them.
//
// Threading and shutdown contracts are identical to ThreadedMatcher (see
// threaded_matcher.hpp); this class differs only in that try_submit /
// try_cancel take a SymbolId routing the message, and book(sym) selects which
// book to inspect after finalize().
//
// Why one worker thread for N books (instead of one thread per book): a
// single-symbol matcher saturates a core long before this codebase saturates
// a queue, and one worker keeps the cross-thread synchronisation footprint
// constant in symbol count. If we ever want per-symbol parallelism, the move
// is to shard by hash(symbol_id) % num_workers, but that's a phase-4 problem.
//
// OrderId scope: ids must be valid within the symbol they were submitted to;
// a cancel that names the wrong symbol is a silent no-op. The Exchange does
// not maintain a global id->symbol index.
template <std::size_t InCapacity = 4096, std::size_t OutCapacity = 4096>
class Exchange {
public:
    Exchange(std::size_t num_symbols,
             Price       tick_min = 0,
             Price       tick_max = 200'000) {
        assert(num_symbols > 0 && "Exchange needs at least one symbol");
        books_.reserve(num_symbols);
        for (std::size_t i = 0; i < num_symbols; ++i) {
            // unique_ptr indirection because OrderBook holds a Pool with a
            // vector<unique_ptr<...>>, and MSVC's vector<OrderBook> chokes on
            // the move-only chain even with reserve(). One pointer chase per
            // book lookup is invisible against the matching cost.
            books_.emplace_back(std::make_unique<OrderBook>(tick_min, tick_max));
        }
        local_trades_.reserve(64);
        worker_ = std::thread([this] { run(); });
    }

    ~Exchange() { finalize(); }

    Exchange(const Exchange&)            = delete;
    Exchange& operator=(const Exchange&) = delete;
    Exchange(Exchange&&)                 = delete;
    Exchange& operator=(Exchange&&)      = delete;

    std::size_t num_symbols() const noexcept { return books_.size(); }

    // ---- producer side ------------------------------------------------------

    bool try_submit(SymbolId sym, const Order& o) {
        return inbound_.try_push(EngineMessage::make_submit(o, sym));
    }

    bool try_cancel(SymbolId sym, OrderId id) {
        return inbound_.try_push(EngineMessage::make_cancel(id, sym));
    }

    // ---- consumer side ------------------------------------------------------

    bool try_pop_trade(Trade& out) { return outbound_.try_pop(out); }

    // ---- shutdown / inspection ---------------------------------------------

    // Signal the worker to stop after draining inbound, then join. Idempotent.
    // After finalize() returns the calling thread is the sole owner of the
    // books; book(sym) is then safe to call.
    void finalize() {
        stop_.store(true, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    // Read-only access to one symbol's book. Only safe after finalize() has
    // returned. The reference is valid for the lifetime of *this.
    const OrderBook& book(SymbolId sym) const {
        assert(sym < books_.size() && "symbol out of range");
        return *books_[sym];
    }

    // ---- diagnostics (approximate, racy) -----------------------------------

    std::size_t inbound_size_approx()  const noexcept { return inbound_.size_approx();  }
    std::size_t outbound_size_approx() const noexcept { return outbound_.size_approx(); }

private:
    void run() {
        EngineMessage msg;
        while (true) {
            if (inbound_.try_pop(msg)) {
                process(msg);
                continue;
            }
            if (stop_.load(std::memory_order_acquire)) {
                // Drain pending: any push that happened-before stop_ is
                // observable to a fresh try_pop (acquire-load on tail_).
                while (inbound_.try_pop(msg)) {
                    process(msg);
                }
                return;
            }
            std::this_thread::yield();
        }
    }

    void process(const EngineMessage& msg) {
        if (msg.symbol_id >= books_.size()) return;   // out-of-range = silent drop
        OrderBook& book = *books_[msg.symbol_id];

        switch (msg.kind) {
            case EngineMessage::Kind::Submit: {
                local_trades_.clear();
                book.submit(msg.order, local_trades_);
                for (Trade& t : local_trades_) {
                    t.symbol_id = msg.symbol_id;       // stamp before publishing
                    while (!outbound_.try_push(t)) {
                        std::this_thread::yield();
                    }
                }
                break;
            }
            case EngineMessage::Kind::Cancel: {
                book.cancel(msg.order_id);
                break;
            }
        }
    }

    std::vector<std::unique_ptr<OrderBook>> books_;
    SpscQueue<EngineMessage, InCapacity>    inbound_;
    SpscQueue<Trade,         OutCapacity>   outbound_;
    std::vector<Trade>                      local_trades_;  // reused, worker-only

    std::atomic<bool> stop_{false};
    std::thread       worker_;
};

}  // namespace tachyon

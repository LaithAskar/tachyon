#pragma once

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

#include "tachyon/engine_message.hpp"
#include "tachyon/order.hpp"
#include "tachyon/order_book.hpp"
#include "tachyon/spsc_queue.hpp"
#include "tachyon/trade.hpp"
#include "tachyon/types.hpp"

namespace tachyon {

// Wraps an OrderBook in a dedicated matcher thread fed by a lock-free inbound
// SPSC queue and producing trade events through a lock-free outbound SPSC
// queue. The OrderBook itself remains single-threaded — exactly one thread
// (the worker) ever touches it during normal operation. The locks the system
// avoids are not on the book; they are on the *boundaries* into and out of it.
//
// Threading contract
// ------------------
//   - Exactly one external thread may call try_submit / try_cancel (the
//     producer side of the inbound SPSC).
//   - Exactly one external thread may call try_pop_trade (the consumer side
//     of the outbound SPSC). It may be a different thread from the producer.
//   - The worker thread owns the book and the matching loop end-to-end.
//   - book() is only safe to call after finalize() has returned, because
//     finalize() joins the worker thread, making the calling thread the sole
//     remaining owner of the book.
//
// Shutdown
// --------
//   - The destructor calls finalize() if it hasn't been called yet. Pending
//     inbound messages are drained before exit; the destructor is *not*
//     instant if the inbound queue is non-empty.
//   - The outbound queue is not drained by the matcher on shutdown — if the
//     trade consumer has stopped popping and the outbound is full, the worker
//     will spin forever in try_push. This is by design: trade events must not
//     be silently dropped. Tests and callers must drain outbound to completion
//     before destructing.
template <std::size_t InCapacity = 4096, std::size_t OutCapacity = 4096>
class ThreadedMatcher {
public:
    ThreadedMatcher()
        : ThreadedMatcher(/*tick_min=*/0, /*tick_max=*/200'000) {}

    ThreadedMatcher(Price tick_min, Price tick_max)
        : book_(tick_min, tick_max) {
        local_trades_.reserve(64);  // typical fan-out for a single submit
        worker_ = std::thread([this] { run(); });
    }

    ~ThreadedMatcher() { finalize(); }

    ThreadedMatcher(const ThreadedMatcher&)            = delete;
    ThreadedMatcher& operator=(const ThreadedMatcher&) = delete;
    ThreadedMatcher(ThreadedMatcher&&)                 = delete;
    ThreadedMatcher& operator=(ThreadedMatcher&&)      = delete;

    // ---- producer side ------------------------------------------------------

    bool try_submit(const Order& o) {
        return inbound_.try_push(EngineMessage::make_submit(o));
    }

    bool try_cancel(OrderId id) {
        return inbound_.try_push(EngineMessage::make_cancel(id));
    }

    // ---- consumer side ------------------------------------------------------

    bool try_pop_trade(Trade& out) { return outbound_.try_pop(out); }

    // ---- shutdown / inspection ---------------------------------------------

    // Signal the worker thread to stop after draining inbound, then join.
    // Idempotent — safe to call from the destructor even after an explicit
    // call by the user. Returns the book for post-mortem inspection; the
    // reference is only valid for the lifetime of *this.
    //
    // Contract: the producer thread must complete its last try_submit /
    // try_cancel before calling finalize(). Calling finalize() concurrently
    // with try_submit / try_cancel races and may lose the in-flight message.
    const OrderBook& finalize() {
        stop_.store(true, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
        return book_;
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
            // Inbound is (transiently) empty. Check shutdown.
            if (stop_.load(std::memory_order_acquire)) {
                // Drain-after-stop. The producer's release-store on stop_
                // is sequenced after its last release-store on inbound's
                // tail_ (contract: finalize is called after the last push).
                // Because we just acquired stop_=true, any prior push is
                // now observable to a fresh try_pop, which does its own
                // acquire-load of tail_. Loop until empty to flush.
                while (inbound_.try_pop(msg)) {
                    process(msg);
                }
                return;
            }
            std::this_thread::yield();
        }
    }

    void process(const EngineMessage& msg) {
        switch (msg.kind) {
            case EngineMessage::Kind::Submit: {
                local_trades_.clear();
                book_.submit(msg.order, local_trades_);
                for (const Trade& t : local_trades_) {
                    // Spin until outbound has space. By contract the consumer
                    // is draining; if it isn't, we deadlock here rather than
                    // silently dropping trades. That's the correct failure
                    // mode for a matching engine.
                    while (!outbound_.try_push(t)) {
                        std::this_thread::yield();
                    }
                }
                break;
            }
            case EngineMessage::Kind::Cancel: {
                book_.cancel(msg.order_id);
                break;
            }
        }
    }

    OrderBook                              book_;
    SpscQueue<EngineMessage, InCapacity>   inbound_;
    SpscQueue<Trade,         OutCapacity>  outbound_;
    std::vector<Trade>                     local_trades_;  // reused, worker-only

    std::atomic<bool> stop_{false};
    std::thread       worker_;
};

}  // namespace tachyon

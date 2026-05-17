// NDJSON event streamer for the Tachyon matching engine.
//
// Runs a randomized order flow through MatchingEngine and emits one
// newline-delimited JSON object per submit on stdout. Intended to be piped
// to a dashboard (WebSocket bridge, file consumer, tail -f, ...) so the
// engine never has to know who is consuming its events.
//
// Output schema per line:
//   {
//     "t": <monotonic step counter>,
//     "event": "order",
//     "order": {"id":..,"side":"B"|"S","type":"L"|"M"|"IOC"|"FOK",
//               "price":<ticks>,"qty":<units>},
//     "trades": [<Trade>, ...],
//     "book": {<OrderBook::snapshot_json>}
//   }
//
// Cancels (when emitted) instead carry "event":"cancel" with an "id" and the
// resulting book snapshot.

#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "tachyon/matching_engine.hpp"

namespace {

using namespace tachyon;

const char* side_str(Side s) { return s == Side::Buy ? "B" : "S"; }

const char* type_str(OrderType t) {
    switch (t) {
        case OrderType::Limit:             return "L";
        case OrderType::Market:            return "M";
        case OrderType::ImmediateOrCancel: return "IOC";
        case OrderType::FillOrKill:        return "FOK";
    }
    return "?";
}

std::string order_to_json(const Order& o) {
    std::string s;
    s.reserve(96);
    s += R"({"id":)";    s += std::to_string(o.id);
    s += R"(,"side":")"; s += side_str(o.side);     s += '"';
    s += R"(,"type":")"; s += type_str(o.type);     s += '"';
    s += R"(,"price":)"; s += std::to_string(o.price);
    s += R"(,"qty":)";   s += std::to_string(o.quantity);
    s += '}';
    return s;
}

std::string trades_to_json(const std::vector<Trade>& trades) {
    std::string s;
    s.reserve(16 + trades.size() * 80);
    s += '[';
    for (std::size_t i = 0; i < trades.size(); ++i) {
        if (i != 0) s += ',';
        s += to_json(trades[i]);
    }
    s += ']';
    return s;
}

struct Generator {
    std::mt19937_64 rng;
    int             step       = 0;
    OrderId         next_id    = 1;
    std::vector<OrderId> live_limits;   // ids of resting limit orders

    Generator() : rng(0xD15EA5E) {}

    // Mostly limits in a tight range, ~5% market, ~10% IOC/FOK each, ~12%
    // cancels of recently rested orders. Account ids stay 0 so the existing
    // tests' notion of "no STP" is preserved.
    Order next_order() {
        std::uniform_int_distribution<int>      side_d(0, 1);
        std::uniform_int_distribution<int>      type_d(0, 99);
        std::uniform_int_distribution<int>      px_d(95'00, 105'00);
        std::uniform_int_distribution<Quantity> qty_d(1, 20);

        ++step;
        const Side side = side_d(rng) ? Side::Buy : Side::Sell;
        const int  bucket = type_d(rng);

        OrderType type;
        if      (bucket < 5)  type = OrderType::Market;
        else if (bucket < 15) type = OrderType::ImmediateOrCancel;
        else if (bucket < 25) type = OrderType::FillOrKill;
        else                  type = OrderType::Limit;

        const Price    price = (type == OrderType::Market) ? 0 : px_d(rng);
        const Quantity qty   = qty_d(rng);
        return Order{next_id++, side, type, price, qty,
                     static_cast<TimestampNs>(step), /*account=*/0};
    }

    bool should_cancel() {
        std::uniform_int_distribution<int> roll(0, 99);
        return !live_limits.empty() && roll(rng) < 12;
    }

    OrderId pick_cancel_id() {
        std::uniform_int_distribution<std::size_t> pick(0, live_limits.size() - 1);
        const std::size_t i  = pick(rng);
        const OrderId     id = live_limits[i];
        live_limits[i] = live_limits.back();
        live_limits.pop_back();
        return id;
    }
};

}  // namespace

int main(int argc, char** argv) {
    int n_events   = 200;
    std::size_t depth = 5;
    if (argc > 1) n_events = std::atoi(argv[1]);
    if (argc > 2) depth    = static_cast<std::size_t>(std::atoi(argv[2]));

    MatchingEngine engine;
    Generator      gen;
    std::vector<Trade> trades;
    trades.reserve(32);

    for (int i = 0; i < n_events; ++i) {
        const bool cancel = gen.should_cancel();
        if (cancel) {
            const OrderId id = gen.pick_cancel_id();
            const bool ok    = engine.on_cancel(id);
            std::cout << R"({"t":)" << ++gen.step
                      << R"(,"event":"cancel","id":)" << id
                      << R"(,"ok":)" << (ok ? "true" : "false")
                      << R"(,"book":)" << engine.book().snapshot_json(depth)
                      << "}\n";
        } else {
            const Order o = gen.next_order();
            engine.on_order(o, trades);
            if (o.type == OrderType::Limit) gen.live_limits.push_back(o.id);
            std::cout << R"({"t":)" << gen.step
                      << R"(,"event":"order","order":)" << order_to_json(o)
                      << R"(,"trades":)" << trades_to_json(trades)
                      << R"(,"book":)"   << engine.book().snapshot_json(depth)
                      << "}\n";
        }
    }
    return 0;
}

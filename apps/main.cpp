// Scripted demo of the Tachyon matching engine.
//
// Six scenes + a bonus, each narrated. Prices are integer ticks where
// 1 tick = 1 cent ($100.05 = 10005).

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "tachyon/matching_engine.hpp"

namespace {

using namespace tachyon;

constexpr std::int64_t kTicksPerDollar = 100;

std::string fmt_price(Price p) {
    char buf[32];
    const std::int64_t dollars = p / kTicksPerDollar;
    const std::int64_t cents   = p % kTicksPerDollar;
    std::snprintf(buf, sizeof(buf), "%3lld.%02lld",
                  static_cast<long long>(dollars), static_cast<long long>(cents));
    return std::string(buf);
}

std::string fmt_optprice(std::optional<Price> p) {
    return p ? fmt_price(*p) : "   --  ";
}

void print_bbo(const OrderBook& book) {
    std::cout << "        BBO: " << fmt_optprice(book.best_bid())
              << " / "           << fmt_optprice(book.best_ask())
              << "    book=" << book.size() << "\n";
}

void print_trades(const std::vector<Trade>& trades) {
    for (const Trade& t : trades) {
        std::cout << "        TRADE: " << t.quantity
                  << " @ " << fmt_price(t.price)
                  << "   taker=#" << t.taker_id
                  << "  maker=#" << t.maker_id << "\n";
    }
}

void print_depth(const OrderBook& book, std::size_t levels) {
    const auto bids = book.top_bids(levels);
    const auto asks = book.top_asks(levels);
    std::cout << "        DEPTH (top " << levels << " each side; "
                 "(n) = orders at that level)\n";
    std::cout << "             BIDS                ASKS\n";
    const std::size_t rows = std::max(bids.size(), asks.size());
    for (std::size_t i = 0; i < rows; ++i) {
        std::ostringstream bstr, astr;
        if (i < bids.size()) {
            bstr << bids[i].total_qty << " @ " << fmt_price(bids[i].price);
            if (bids[i].order_count > 1) bstr << " (" << bids[i].order_count << ")";
        }
        if (i < asks.size()) {
            astr << fmt_price(asks[i].price) << " @ " << asks[i].total_qty;
            if (asks[i].order_count > 1) astr << " (" << asks[i].order_count << ")";
        }
        std::printf("           %18s    %-18s\n",
                    bstr.str().c_str(), astr.str().c_str());
    }
}

struct Demo {
    MatchingEngine engine;
    OrderId        next_id     = 1;
    int            step        = 0;
    std::size_t    trade_count = 0;
    std::uint64_t  volume      = 0;
    std::uint64_t  notional    = 0;

    void section(const char* title) {
        std::cout << "\n========================================================\n"
                  << "  " << title
                  << "\n========================================================\n";
    }

    void note(const std::string& msg) {
        std::cout << "\n  " << msg << "\n";
    }

    std::vector<Trade> submit_limit(Side side, Price price, Quantity qty,
                                    const std::string& tag = "") {
        const OrderId id = next_id++;
        ++step;
        std::printf("\n[t=%03d] submit %-4s LIMIT  %5llu @ %s   (id=#%llu)",
                    step,
                    side == Side::Buy ? "BUY" : "SELL",
                    static_cast<unsigned long long>(qty),
                    fmt_price(price).c_str(),
                    static_cast<unsigned long long>(id));
        if (!tag.empty()) std::cout << "  " << tag;
        std::cout << "\n";

        Order o{id, side, OrderType::Limit, price, qty,
                static_cast<TimestampNs>(step)};
        auto trades = engine.on_order(o);
        account_trades(trades);
        print_trades(trades);
        print_bbo(engine.book());
        return trades;
    }

    std::vector<Trade> submit_market(Side side, Quantity qty,
                                     const std::string& tag = "") {
        const OrderId id = next_id++;
        ++step;
        std::printf("\n[t=%03d] submit %-4s MARKET %5llu          (id=#%llu)",
                    step,
                    side == Side::Buy ? "BUY" : "SELL",
                    static_cast<unsigned long long>(qty),
                    static_cast<unsigned long long>(id));
        if (!tag.empty()) std::cout << "  " << tag;
        std::cout << "\n";

        Order o{id, side, OrderType::Market, 0, qty,
                static_cast<TimestampNs>(step)};
        auto trades = engine.on_order(o);
        account_trades(trades);
        print_trades(trades);
        print_bbo(engine.book());
        return trades;
    }

    bool cancel(OrderId id) {
        ++step;
        std::printf("\n[t=%03d] cancel  #%llu\n",
                    step, static_cast<unsigned long long>(id));
        const bool ok = engine.on_cancel(id);
        std::cout << "        " << (ok ? "cancelled" : "NOT FOUND") << "\n";
        print_bbo(engine.book());
        return ok;
    }

    void account_trades(const std::vector<Trade>& trades) {
        for (const Trade& t : trades) {
            ++trade_count;
            volume   += t.quantity;
            notional += static_cast<std::uint64_t>(t.price) * t.quantity;
        }
    }

    void summary() {
        std::cout << "\n========================================================\n"
                  << "  FINAL STATS\n"
                  << "========================================================\n"
                  << "  orders submitted : " << (next_id - 1) << "\n"
                  << "  trades printed   : " << trade_count   << "\n"
                  << "  volume traded    : " << volume        << " units\n";
        const std::uint64_t cents = notional;  // notional already in tick units = cents
        std::cout << "  notional traded  : $"
                  << (cents / kTicksPerDollar) << "."
                  << (cents % kTicksPerDollar < 10 ? "0" : "")
                  << (cents % kTicksPerDollar) << "\n"
                  << "  book size        : " << engine.book().size() << "\n";
        print_bbo(engine.book());
    }
};

}  // namespace

int main() {
    Demo d;

    std::cout << "tachyon demo - scripted scenario\n";
    std::cout << "(prices are dollars; ticks = cents; ids are #N)\n";

    // ─── Scene 1 ────────────────────────────────────────────────────────
    d.section("Scene 1: build a one-sided book of bids");
    d.note("Three resting buys at different prices. No asks yet, so no trades. "
           "Best bid rises to the highest price as each one rests.");
    d.submit_limit(Side::Buy, 100'00, 10);           // #1
    d.submit_limit(Side::Buy,  99'50, 20);           // #2
    d.submit_limit(Side::Buy, 101'00,  5,
                   "<-- becomes new best bid");      // #3

    // ─── Scene 2 ────────────────────────────────────────────────────────
    d.section("Scene 2: add the ask side and show depth");
    d.note("Three resting sells; #4 and #6 both sit at 102.00, so that "
           "level will queue two orders. Watch the depth view: the 102.00 "
           "row shows 'qty (2)' meaning two orders aggregated.");
    d.submit_limit(Side::Sell, 102'00,  8);          // #4
    d.submit_limit(Side::Sell, 103'00, 12);          // #5
    d.submit_limit(Side::Sell, 102'00,  7,
                   "<-- joins level 102.00 behind #4"); // #6
    print_depth(d.engine.book(), 5);

    // ─── Scene 3 ────────────────────────────────────────────────────────
    d.section("Scene 3: crossing trade with price improvement");
    d.note("A BUY limit at 105.00 is willing to pay up to 105.00 -- but the "
           "best ask is 102.00, so the trade prints at the MAKER's price "
           "(102.00). The taker gets a $3.00 price improvement per unit.");
    d.submit_limit(Side::Buy, 105'00, 5,
                   "<-- aggressive, crosses");        // #7
    d.note("After: level 102.00 still has #4 (with 3 units left after the "
           "5-unit fill) and #6 (untouched, 7 units).");
    print_depth(d.engine.book(), 5);

    // ─── Scene 4 ────────────────────────────────────────────────────────
    d.section("Scene 4: same-level FIFO + partial fill in one trade");
    d.note("BUY 9 @ 102.00. The 102.00 ask queue is [#4: 3 units, #6: 7 units]. "
           "FIFO eats #4 entirely first (3), then partials #6 (6 of its 7). "
           "Two trades from one submit, both at 102.00.");
    d.submit_limit(Side::Buy, 102'00, 9);             // #8
    d.note("Level 102.00 now has only #6 with 1 unit remaining.");
    print_depth(d.engine.book(), 5);

    // ─── Scene 5 ────────────────────────────────────────────────────────
    d.section("Scene 5: cancel");
    d.note("Cancel #2 (the 99.50 bid from scene 1). It should disappear.");
    d.cancel(2);
    d.note("Cancelling the same id again must be a no-op (returns false).");
    d.cancel(2);
    d.note("Unknown id is also a no-op.");
    d.cancel(9999);

    // ─── Scene 6 ────────────────────────────────────────────────────────
    d.section("Scene 6: market order sweeps the book, residual dropped");
    d.note("Snapshot before the sweep:");
    print_depth(d.engine.book(), 5);
    d.note("MARKET BUY for 1000 units. The ask side has only 13 units total "
           "(1 @ 102.00 + 12 @ 103.00). Two trades will print. The 987-unit "
           "residual is DROPPED -- market orders never rest.");
    d.submit_market(Side::Buy, 1000,
                    "<-- sweeps both ask levels");    // #9
    d.note("Ask side should be empty:");
    print_depth(d.engine.book(), 5);

    // ─── Bonus ──────────────────────────────────────────────────────────
    d.section("Bonus: zero-quantity order is silently rejected");
    d.submit_limit(Side::Buy, 100'00, 0,
                   "<-- no trade, no rest, no error"); // #10

    d.summary();
    return 0;
}

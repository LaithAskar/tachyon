// Per-order submit latency histogram. Standalone — no Google Benchmark dep.
//
// Methodology:
//   1. Pre-populate the book with `prewarm` non-crossing limits so the data
//      structures are at steady state (allocator warmed, tree balanced).
//   2. Pre-generate `measured` mixed-flow orders so RNG cost is excluded.
//   3. Time each submit() individually with steady_clock, store nanos.
//   4. Sort and report percentiles.
//
// Caveats: steady_clock resolution on Windows is ~100ns; sub-100ns ops will
// quantise. For finer resolution, swap in QueryPerformanceCounter or rdtsc.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <vector>

#include "tachyon/order_book.hpp"

namespace {

using namespace tachyon;

Order gen_non_crossing(std::mt19937_64& rng, OrderId id, TimestampNs ts) {
    std::uniform_int_distribution<int>      bid_px(900'00, 990'00);
    std::uniform_int_distribution<int>      ask_px(1'010'00, 1'100'00);
    std::uniform_int_distribution<Quantity> qty(1, 100);
    const bool buy = (id & 1) == 0;
    return Order{id,
                 buy ? Side::Buy : Side::Sell,
                 OrderType::Limit,
                 buy ? bid_px(rng) : ask_px(rng),
                 qty(rng),
                 ts};
}

Order gen_mixed(std::mt19937_64& rng, OrderId id, TimestampNs ts) {
    std::uniform_int_distribution<int>      px(950'00, 1'050'00);
    std::uniform_int_distribution<Quantity> qty(1, 50);
    std::uniform_int_distribution<int>      side(0, 1);
    std::uniform_int_distribution<int>      type(0, 19);  // ~5% market
    const bool market = type(rng) == 0;
    return Order{id,
                 side(rng) ? Side::Buy : Side::Sell,
                 market ? OrderType::Market : OrderType::Limit,
                 market ? 0 : px(rng),
                 qty(rng),
                 ts};
}

void report(const char* label, std::vector<std::int64_t>& ns) {
    std::sort(ns.begin(), ns.end());
    auto pct = [&](double p) -> std::int64_t {
        if (ns.empty()) return 0;
        std::size_t idx = static_cast<std::size_t>(p * (ns.size() - 1));
        return ns[idx];
    };
    double sum = 0.0;
    for (auto x : ns) sum += static_cast<double>(x);

    std::cout << "\n== " << label << " (n=" << ns.size() << ") ==\n";
    std::cout << std::fixed << std::setprecision(0);
    std::cout << "  mean   : " << (sum / static_cast<double>(ns.size())) << " ns\n";
    std::cout << "  p50    : " << pct(0.50)  << " ns\n";
    std::cout << "  p90    : " << pct(0.90)  << " ns\n";
    std::cout << "  p99    : " << pct(0.99)  << " ns\n";
    std::cout << "  p99.9  : " << pct(0.999) << " ns\n";
    std::cout << "  max    : " << ns.back()  << " ns\n";
    if (sum > 0.0) {
        const double secs = sum / 1e9;
        std::cout << "  throughput (this loop): "
                  << (static_cast<double>(ns.size()) / secs) << " ops/sec\n";
    }
}

std::vector<std::int64_t> measure(OrderBook& book, const std::vector<Order>& flow) {
    std::vector<std::int64_t> ns;
    ns.reserve(flow.size());
    for (const Order& o : flow) {
        const auto t0 = std::chrono::steady_clock::now();
        auto trades   = book.submit(o);
        const auto t1 = std::chrono::steady_clock::now();
        // Force the compiler to keep the call.
        volatile std::size_t sink = trades.size();
        (void)sink;
        ns.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }
    return ns;
}

}  // namespace

int main(int argc, char** argv) {
    int prewarm  = 50'000;
    int measured = 200'000;
    if (argc > 1) prewarm  = std::atoi(argv[1]);
    if (argc > 2) measured = std::atoi(argv[2]);

    std::cout << "tachyon latency harness\n"
              << "  prewarm orders : " << prewarm  << "\n"
              << "  measured orders: " << measured << "\n";

    // --- Scenario 1: pure non-crossing inserts ------------------------------
    {
        OrderBook book;
        std::mt19937_64 rng(0xBEEF);
        for (int i = 0; i < prewarm; ++i) {
            book.submit(gen_non_crossing(rng, static_cast<OrderId>(i + 1),
                                         static_cast<TimestampNs>(i)));
        }

        std::vector<Order> flow;
        flow.reserve(measured);
        for (int i = 0; i < measured; ++i) {
            flow.push_back(gen_non_crossing(rng,
                                            static_cast<OrderId>(prewarm + i + 1),
                                            static_cast<TimestampNs>(prewarm + i)));
        }

        auto ns = measure(book, flow);
        report("insert (no cross)", ns);
    }

    // --- Scenario 2: mixed flow with frequent crossings ---------------------
    {
        OrderBook book;
        std::mt19937_64 rng(0xCAFE);
        for (int i = 0; i < prewarm; ++i) {
            book.submit(gen_non_crossing(rng, static_cast<OrderId>(i + 1),
                                         static_cast<TimestampNs>(i)));
        }

        std::vector<Order> flow;
        flow.reserve(measured);
        std::mt19937_64 rng2(0xFACE);
        for (int i = 0; i < measured; ++i) {
            flow.push_back(gen_mixed(rng2,
                                     static_cast<OrderId>(prewarm + i + 1),
                                     static_cast<TimestampNs>(prewarm + i)));
        }

        auto ns = measure(book, flow);
        report("mixed flow (~50% cross)", ns);
    }

    return 0;
}

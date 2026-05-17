#include <benchmark/benchmark.h>

#include <cstdint>
#include <random>
#include <vector>

#include "tachyon/order_book.hpp"

namespace tachyon {
namespace {

struct Stream {
    std::vector<Order> orders;
};

// Generate `n` non-crossing limit orders centred so bids stay below asks.
// Seeded for reproducibility.
Stream make_non_crossing(int n, std::uint64_t seed) {
    Stream s;
    s.orders.reserve(n);
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int>      bid_px(900'00, 990'00);
    std::uniform_int_distribution<int>      ask_px(1'010'00, 1'100'00);
    std::uniform_int_distribution<Quantity> qty(1, 100);

    for (int i = 0; i < n; ++i) {
        const bool buy = (i & 1) == 0;
        s.orders.push_back(Order{
            static_cast<OrderId>(i + 1),
            buy ? Side::Buy : Side::Sell,
            OrderType::Limit,
            buy ? bid_px(rng) : ask_px(rng),
            qty(rng),
            static_cast<TimestampNs>(i),
        });
    }
    return s;
}

// Generate `n` mixed-flow orders that frequently cross. Bids and asks share
// a price range so ~half the flow generates trades.
Stream make_mixed(int n, std::uint64_t seed) {
    Stream s;
    s.orders.reserve(n);
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int>      px(950'00, 1'050'00);
    std::uniform_int_distribution<Quantity> qty(1, 50);
    std::uniform_int_distribution<int>      side(0, 1);
    std::uniform_int_distribution<int>      type(0, 19);  // ~5% market

    for (int i = 0; i < n; ++i) {
        const bool market = type(rng) == 0;
        s.orders.push_back(Order{
            static_cast<OrderId>(i + 1),
            side(rng) ? Side::Buy : Side::Sell,
            market ? OrderType::Market : OrderType::Limit,
            market ? 0 : px(rng),
            qty(rng),
            static_cast<TimestampNs>(i),
        });
    }
    return s;
}

void BM_InsertNoCross(benchmark::State& state) {
    const int n = static_cast<int>(state.range(0));
    const Stream s = make_non_crossing(n, /*seed=*/42);

    for (auto _ : state) {
        state.PauseTiming();
        OrderBook book;
        state.ResumeTiming();

        for (const Order& o : s.orders) {
            auto trades = book.submit(o);
            benchmark::DoNotOptimize(trades);
        }
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_InsertNoCross)->Arg(100'000)->Unit(benchmark::kMillisecond);

void BM_MixedFlow(benchmark::State& state) {
    const int n = static_cast<int>(state.range(0));
    const Stream s = make_mixed(n, /*seed=*/43);

    for (auto _ : state) {
        state.PauseTiming();
        OrderBook book;
        state.ResumeTiming();

        for (const Order& o : s.orders) {
            auto trades = book.submit(o);
            benchmark::DoNotOptimize(trades);
        }
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_MixedFlow)->Arg(100'000)->Unit(benchmark::kMillisecond);

// Same workload as BM_MixedFlow but uses the out-param submit() so the Trade
// buffer is allocated once and reused. Designed to be diff'd against
// BM_MixedFlow to show the cost of value-return on a trade-heavy workload.
void BM_MixedFlow_OutParam(benchmark::State& state) {
    const int n = static_cast<int>(state.range(0));
    const Stream s = make_mixed(n, /*seed=*/43);

    for (auto _ : state) {
        state.PauseTiming();
        OrderBook book;
        std::vector<Trade> trades;
        trades.reserve(32);            // typical max trades-per-submit for this flow
        state.ResumeTiming();

        for (const Order& o : s.orders) {
            book.submit(o, trades);
            benchmark::DoNotOptimize(trades);
        }
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_MixedFlow_OutParam)->Arg(100'000)->Unit(benchmark::kMillisecond);

void BM_Cancel(benchmark::State& state) {
    const int n = static_cast<int>(state.range(0));
    const Stream s = make_non_crossing(n, /*seed=*/44);

    for (auto _ : state) {
        state.PauseTiming();
        OrderBook book;
        for (const Order& o : s.orders) book.submit(o);
        state.ResumeTiming();

        for (const Order& o : s.orders) {
            bool ok = book.cancel(o.id);
            benchmark::DoNotOptimize(ok);
        }
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_Cancel)->Arg(100'000)->Unit(benchmark::kMillisecond);

}  // namespace
}  // namespace tachyon

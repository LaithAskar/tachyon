// rdtsc-based per-operation latency histogram for the OrderBook hot path.
//
// Why this exists (vs bench_latency.cpp's steady_clock version):
//   steady_clock on Windows quantises around the QPC tick (~100ns) which is
//   the same order of magnitude as the operations we're trying to measure.
//   At the bottom of the tail this turns sub-100ns ops into 0s and 100s and
//   loses the distribution shape entirely. __rdtscp counts cycles directly,
//   converts to nanoseconds via a calibrated TSC frequency, and resolves
//   single-cycle differences.
//
// Methodology, in order of importance:
//   1. Pin the measuring thread to one core. TSC is per-core; even if it's
//      "invariant" across cores in practice, migrating mid-measurement gives
//      garbage.
//   2. THREAD_PRIORITY_TIME_CRITICAL — best Windows can do without DPC-level
//      hacks. Not the same as preemption-free; do not pretend it is.
//   3. lfence + __rdtscp(&aux) bracketing each operation. __rdtscp serialises
//      on retirement (waits for prior instructions), lfence on dispatch
//      (stops later instructions from leaking in). The pair brackets the
//      operation tightly.
//   4. Pre-generate all input. No RNG, no allocation, no logging inside the
//      measurement loop. The out-param overload of submit() keeps the trade
//      vector from reallocating.
//   5. Calibrate TSC frequency once at startup, against QPC. ~100ms anchor.
//
// What this does NOT control for:
//   - SMT siblings stealing execution resources. We pin to CPU 0; if the
//     siblng on the same core is busy, we contend.
//   - Turbo boost. The TSC is "invariant" (same rate regardless of P-state)
//     on all CPUs we care about, but the *work* speed varies. Run plugged in.
//   - Interrupts and OS housekeeping. They show up as fat tail. Don't
//     hand-wave them away — that's why p99.9 and max exist.
//
// Caveats on the numbers it prints:
//   - These are bare OrderBook::submit / cancel costs. They do NOT include
//     SPSC enqueue/dequeue, thread handoff, JSON serialisation, or anything
//     else the production-shaped path would do.
//   - Sub-percentile precision is meaningful; sub-cycle precision is not.
//   - If "max" is >100x the "p99.9", something jittered the measurement —
//     usually a context switch. Re-run rather than trust the outlier.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

#if defined(_MSC_VER)
#  include <intrin.h>
#  define NOMINMAX                  // keep std::min / std::max usable
#  include <windows.h>
#else
#  include <x86intrin.h>
#endif

#include "tachyon/order_book.hpp"

namespace {

using namespace tachyon;

// --- TSC helpers -----------------------------------------------------------

inline std::uint64_t rdtscp_serialised() {
    // The canonical pattern: lfence stops later µops from issuing before
    // the rdtscp reads. rdtscp itself stops prior µops from retiring after.
    unsigned aux = 0;
    std::uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
}

inline std::uint64_t rdtscp_start() {
    // Mirror of the above for the *start* of a measured region: lfence first
    // so prior code is fully retired, then read the counter.
    _mm_lfence();
    unsigned aux = 0;
    return __rdtscp(&aux);
}

// Cycles per second, calibrated against the OS high-res clock. Run once;
// reuse across all scenarios. ~100ms anchor is a sweet spot — long enough
// to be precise (>1e8 cycles), short enough not to be annoying at startup.
double calibrate_tsc_hz() {
    using clk = std::chrono::steady_clock;
    const auto wall_start = clk::now();
    const std::uint64_t tsc_start = rdtscp_start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const std::uint64_t tsc_end = rdtscp_serialised();
    const auto wall_end = clk::now();

    const double secs =
        std::chrono::duration<double>(wall_end - wall_start).count();
    const double cycles = static_cast<double>(tsc_end - tsc_start);
    return cycles / secs;
}

// --- Thread pinning / priority (Windows) -----------------------------------

void pin_and_boost() {
#if defined(_WIN32)
    // CPU 0. Anything stable works; the point is "pick one and stay there."
    if (SetThreadAffinityMask(GetCurrentThread(), 1ULL) == 0) {
        std::cerr << "warning: SetThreadAffinityMask failed; results are jitter-prone\n";
    }
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
        std::cerr << "warning: SetThreadPriority failed; results are jitter-prone\n";
    }
#endif
}

// --- Stats -----------------------------------------------------------------

struct Stats {
    double mean_ns;
    double p50_ns;
    double p90_ns;
    double p99_ns;
    double p99_9_ns;
    double p99_99_ns;
    double min_ns;
    double max_ns;
};

Stats summarise(std::vector<std::uint64_t>& cycles, double tsc_hz) {
    std::sort(cycles.begin(), cycles.end());
    const double ns_per_cycle = 1e9 / tsc_hz;
    auto cyc_to_ns = [&](std::uint64_t c) {
        return static_cast<double>(c) * ns_per_cycle;
    };
    auto pct = [&](double p) {
        const std::size_t idx =
            static_cast<std::size_t>(p * (cycles.size() - 1));
        return cyc_to_ns(cycles[idx]);
    };
    double sum_cycles = 0.0;
    for (auto c : cycles) sum_cycles += static_cast<double>(c);
    Stats s;
    s.mean_ns   = (sum_cycles / static_cast<double>(cycles.size())) * ns_per_cycle;
    s.p50_ns    = pct(0.50);
    s.p90_ns    = pct(0.90);
    s.p99_ns    = pct(0.99);
    s.p99_9_ns  = pct(0.999);
    s.p99_99_ns = pct(0.9999);
    s.min_ns    = cyc_to_ns(cycles.front());
    s.max_ns    = cyc_to_ns(cycles.back());
    return s;
}

void print_stats(const char* label, std::size_t n, const Stats& s) {
    std::cout << "\n== " << label << " (n=" << n << ") ==\n";
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "  mean    : " << s.mean_ns    << " ns\n";
    std::cout << "  min     : " << s.min_ns     << " ns\n";
    std::cout << "  p50     : " << s.p50_ns     << " ns\n";
    std::cout << "  p90     : " << s.p90_ns     << " ns\n";
    std::cout << "  p99     : " << s.p99_ns     << " ns\n";
    std::cout << "  p99.9   : " << s.p99_9_ns   << " ns\n";
    std::cout << "  p99.99  : " << s.p99_99_ns  << " ns\n";
    std::cout << "  max     : " << s.max_ns     << " ns\n";
}

// --- Order generators (pre-computed; never run in the timed loop) ----------

std::vector<Order> gen_non_crossing_inserts(int n, OrderId id0) {
    std::vector<Order> v;
    v.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const bool buy = (i & 1) == 0;
        const Price px = buy ? (900'00 + (i % 90))
                             : (1'010'00 + (i % 90));
        v.push_back(Order{id0 + static_cast<OrderId>(i),
                          buy ? Side::Buy : Side::Sell,
                          OrderType::Limit, px, /*qty=*/10,
                          static_cast<TimestampNs>(i)});
    }
    return v;
}

// Pre-seed crossing makers, then takers that consume them 1:1 at the same px.
struct CrossingScenario {
    OrderBook          book;
    std::vector<Order> takers;
};

CrossingScenario build_submit_with_match(int n) {
    CrossingScenario s;
    // 1:1 ask makers; each taker buy at the maker's price.
    OrderId next = 1;
    for (int i = 0; i < n; ++i) {
        s.book.submit(Order{next++, Side::Sell, OrderType::Limit,
                            /*px=*/1'000'00 + (i % 50),
                            /*qty=*/10, static_cast<TimestampNs>(i)});
    }
    s.takers.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        s.takers.push_back(Order{next++, Side::Buy, OrderType::Limit,
                                 /*px=*/1'000'00 + (i % 50),
                                 /*qty=*/10,
                                 static_cast<TimestampNs>(n + i)});
    }
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    int n = 200'000;
    if (argc > 1) n = std::atoi(argv[1]);
    if (n < 1000) {
        std::cerr << "n too small for meaningful tail stats; using 1000 min\n";
        n = 1000;
    }

    pin_and_boost();

    std::cout << "tachyon rdtsc latency harness\n";
    std::cout << "  samples per scenario: " << n << "\n";

    std::cout << "  calibrating TSC ... " << std::flush;
    const double tsc_hz = calibrate_tsc_hz();
    std::cout << std::fixed << std::setprecision(3)
              << (tsc_hz / 1e9) << " GHz\n";

    // Scratch buffer for trades — pre-reserved so submit() never allocates.
    std::vector<Trade> trades;
    trades.reserve(256);

    // --- 1) submit (no match) -------------------------------------------------
    {
        OrderBook book;
        auto      flow = gen_non_crossing_inserts(n, /*id0=*/1);

        // Warm: same flow shape but smaller; primes pool + page table.
        const int warmup = std::min(n / 10, 5000);
        for (int i = 0; i < warmup; ++i) {
            trades.clear();
            book.submit(flow[i % flow.size()], trades);
        }

        // Fresh book for the actual measurement so warmup state doesn't bias.
        OrderBook fresh;
        std::vector<std::uint64_t> cycles;
        cycles.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            trades.clear();
            const std::uint64_t t0 = rdtscp_start();
            fresh.submit(flow[i], trades);
            const std::uint64_t t1 = rdtscp_serialised();
            cycles.push_back(t1 - t0);
        }
        const Stats s = summarise(cycles, tsc_hz);
        print_stats("submit (no match)", cycles.size(), s);
    }

    // --- 2) submit (every order matches) -------------------------------------
    {
        CrossingScenario sc = build_submit_with_match(n);
        std::vector<std::uint64_t> cycles;
        cycles.reserve(static_cast<std::size_t>(n));
        for (const Order& taker : sc.takers) {
            trades.clear();
            const std::uint64_t t0 = rdtscp_start();
            sc.book.submit(taker, trades);
            const std::uint64_t t1 = rdtscp_serialised();
            cycles.push_back(t1 - t0);
        }
        const Stats s = summarise(cycles, tsc_hz);
        print_stats("submit (with match)", cycles.size(), s);
    }

    // --- 3) cancel ------------------------------------------------------------
    {
        OrderBook book;
        auto      flow = gen_non_crossing_inserts(n, /*id0=*/1);
        for (const Order& o : flow) {
            trades.clear();
            book.submit(o, trades);
        }
        // Cancel them in submission order. Cancelling in reverse / random would
        // also be a valid scenario — submission order is the easiest to defend.
        std::vector<std::uint64_t> cycles;
        cycles.reserve(static_cast<std::size_t>(n));
        for (const Order& o : flow) {
            const std::uint64_t t0 = rdtscp_start();
            const bool ok = book.cancel(o.id);
            const std::uint64_t t1 = rdtscp_serialised();
            cycles.push_back(t1 - t0);
            // Prevent the compiler from optimising the cancel away.
            if (!ok) {
                std::cerr << "unexpected: cancel failed for id " << o.id << "\n";
                std::abort();
            }
        }
        const Stats s = summarise(cycles, tsc_hz);
        print_stats("cancel", cycles.size(), s);
    }

    return 0;
}

`# Tachyon — Limit Order Book Matching Engine

Low-latency limit order book and matching engine in C++17. Single-threaded core,
price-time priority, designed to be hot-path for a simulated exchange.

## Why

Most undergrad quant projects train an ML model on price data. Few build the
infrastructure that exchanges run on. Tachyon is the latter: the data structures,
matching logic, and performance discipline that interview shops like Citadel
Securities, Jane Street, HRT, Optiver, and IMC ask candidates to demonstrate.

## Targets

- **Throughput**: 1M+ orders/sec sustained on commodity hardware
- **Latency**: p99 submit→ack under 20µs (single-threaded)
- **Correctness**: strict price-time priority, no allocator surprises in the hot path

## Architecture (planned)

```
apps/main.cpp                  # demo driver
include/tachyon/
  types.hpp                    # OrderId, Price (ticks), Quantity, Side, OrderType
  order.hpp                    # Order struct (POD)
  trade.hpp                    # Trade struct (POD)
  order_book.hpp               # OrderBook: bids + asks, submit / cancel / best
  matching_engine.hpp          # MatchingEngine: dispatch on incoming messages
src/
  order_book.cpp
  matching_engine.cpp
tests/                         # GoogleTest unit tests
benchmarks/                    # (later) Google Benchmark perf harness
```

## Roadmap

| Week | Goal |
|------|------|
| 1 | Types + OrderBook skeleton compiling + first 3 tests passing. Add a single buy-limit, verify best_bid. |
| 2 | submit() matches a crossing order against opposite book, generates Trades. Add cancel(). FIFO within a price level. |
| 3 | Market orders. Partial fills. Edge cases (self-cross prevention, zero-quantity reject, price/qty bounds). Replay test against a recorded log. |
| 4 | Benchmark harness. Throughput + latency percentiles. README with a chart. Integrate as Meridian's simulated exchange backend. |

Stretch (only if ahead of schedule):
- FIX-like binary message format for order entry
- Lock-free SPSC queue (Boost.Lockfree) for ingestion thread → matching thread
- Event-sourcing persistence + deterministic replay
- WebSocket viewer (reuses Next.js skill from Meridian)

## Build (Windows, VS 2022)

Open **"Developer PowerShell for VS 2022"** from the Start menu. That sets up the
MSVC environment. Then from this directory:

```powershell
# Generate solution + build
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# Run the demo
.\build\bin\Release\tachyon.exe

# Run tests (downloads GoogleTest the first time)
ctest --test-dir build -C Release --output-on-failure
```

For faster iteration, generate Ninja or use the VS solution directly
(`build\tachyon.sln`).

## Design notes — read before writing matching logic

- **Price as integer ticks, never `double`.** A tick is a configurable unit (e.g.
  $0.01 → multiplier 100; $0.0001 → 10000). Floating-point compare in a hot path
  is a bug waiting to happen.
- **Price-time priority**: at the same price, earlier orders match first. Use a
  FIFO container per level (deque or intrusive list).
- **O(1) cancel** matters: maintain an `unordered_map<OrderId, iterator>` so
  cancel doesn't scan a level.
- **No allocations on the match path.** Reuse `std::vector<Trade>` capacity. Do
  not return by `std::vector` from inner loops once you optimize.
- **Decide your data structure intentionally**. The default is
  `std::map<Price, std::deque<Order>>` (bids reverse-ordered). For the perf
  target, you'll later want a flat sorted vector or custom linked-level scheme.
  Start with `std::map`; benchmark; replace.

## References

- Larry Harris — *Trading and Exchanges*, ch. 6–9 (order book mechanics)
- QuantCup matching engine challenge (open-source reference impls)
- Optiver / IMC / Jane Street public talks on exchange architecture (YouTube)

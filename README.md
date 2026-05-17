# Tachyon

Single-symbol limit order book and matching engine in C++17, with a live
WebSocket dashboard. Built to demonstrate the data structures and
allocation discipline that exchange-grade systems run on.

<!-- TODO: capture a screenshot of the running dashboard, save as docs/dashboard.png -->

![dashboard](docs/dashboard.png)

## Numbers

Single-threaded core, commodity Windows desktop, MSVC `/O2`:

| metric | value |
|---|---|
| submit throughput (out-param hot path, mixed flow) | **~4.5M ops/sec** |
| cancel throughput | ~5.0M ops/sec |
| p50 submit latency | **300 ns** |
| p99 submit latency | **1.6 µs** |
| tests passing | 43 / 43 |

Latency is `steady_clock`-measured on Windows — see `design.md §7` for
the methodology and the honest disclaimer about scheduler-bound tails.

## What it does

- Limit order book with strict **price-time priority**
- Four order types: `Limit` (GTC), `Market`, `ImmediateOrCancel`,
  `FillOrKill` (with all-or-nothing pre-check)
- **Self-trade prevention** via per-order `account_id` — same-account
  makers are skipped during matching without disturbing FIFO for others
- O(1) `submit` / `cancel`; best-level advance after a drain is a
  uint64 bitmap scan over the flat tick ladder
- **No allocator traffic on the hot path** — pool-backed intrusive
  doubly-linked list per level; the `id_index_` stores raw
  `OrderNode*` pointers that stay valid for the life of the book
- Out-of-process **live dashboard** over WebSocket: depth bars, trade
  tape, BBO/spread/event-rate stats

## Architecture in one paragraph

Each side of the book is a `std::vector<Level>` indexed by tick offset
from a configured `tick_min`. A `uint64` bitmap per side flags
non-empty levels; the best-bid / best-ask cursors are `int64` indices
that get advanced via `_BitScanReverse64` / `_BitScanForward64` when a
level drains. Each `Level` is an intrusive doubly-linked list of
`OrderNode`s drawn from a block-growing `Pool` — once warmed, no
allocator call happens during matching. The `MatchingEngine` exposes a
trade-listener hook; a separate `tachyon_stream` binary uses it to
emit one NDJSON event per submit on stdout, which a 100-line Node
script bridges to a vanilla-JS WebSocket dashboard.

Full reasoning, trade-offs, and the property-based test invariants
live in [`design.md`](design.md).

## Build (Windows, VS 2022)

Open **Developer PowerShell for VS 2022**, then from this directory:

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# unit + property tests
ctest --test-dir build -C Release --output-on-failure

# scripted CLI demo
.\build\bin\Release\tachyon.exe

# throughput / latency benchmarks
cmake -B build -DTACHYON_BUILD_BENCHMARKS=ON
cmake --build build --config Release
.\build\bin\Release\bench_throughput.exe
.\build\bin\Release\bench_latency.exe
```

## Run the live dashboard

```powershell
# one-time
cmake --build build --config Release --target tachyon_stream
npm --prefix dashboard install

# every time
node dashboard\server.js
# open http://localhost:8080
```

The Node script spawns the C++ `tachyon_stream` binary, reads its
NDJSON stdout, and rebroadcasts each event as a WebSocket frame. The
browser renders depth bars, trade tape, BBO, and rolling events/sec.
Tune `DELAY_MS=20 node dashboard\server.js` for a faster stream.

## API surface

```cpp
tachyon::OrderBook book;                       // default range $0-$2000 (cent ticks)
std::vector<tachyon::Trade> trades;
trades.reserve(32);                            // amortise allocation

book.submit(tachyon::Order{
    /*id=*/1, tachyon::Side::Buy, tachyon::OrderType::Limit,
    /*price=*/100'00, /*qty=*/10, /*ts_ns=*/0, /*account=*/0
}, trades);

book.cancel(/*id=*/1);
auto bid = book.best_bid();
std::string snap = book.snapshot_json(/*depth=*/5);
```

## What it deliberately is not

- **Not a multi-symbol exchange.** One book per process, single symbol.
- **Not multithreaded.** A single-threaded matcher fed by a lock-free
  SPSC queue is a textbook pattern — the engine is ready for it
  (clean trade-listener seam), but adding threads would be cosmetic
  until network ingestion or multi-symbol matching exists.
- **Not production-grade for storage.** No persistence, no
  audit-log replay, no recovery story.
- **Not microbenchmarked with `rdtsc` / pinned cores.** Latency
  numbers above are honest `steady_clock` measurements on Windows;
  sub-microsecond tails are scheduler-bound, not engine-bound. See
  `design.md §7`.

## References

- Larry Harris — *Trading and Exchanges*, ch. 6–9 (book mechanics)
- QuantCup matching engine challenge (open-source reference impls)
- LMAX Disruptor paper (cache-friendly queue patterns)

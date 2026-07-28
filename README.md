# Tachyon

Single-symbol limit order book and matching engine in C++17, with a live
WebSocket dashboard. Built to demonstrate the data-structure and
allocation discipline that real exchange matching engines run on.

Local dashboard screenshot/GIF: pending capture from `http://localhost:8080`.
For the shortest reviewer path, see [`docs/demo.md`](docs/demo.md), then run the
five-minute demo below.

## In plain English

A stock exchange is a giant order desk: thousands of traders shout "I'll
buy 100 at \$50" and "I'll sell 50 at \$51," and someone has to keep a
tidy list of every offer in price order and pair up buyers with sellers
the instant their prices meet — fairly, first-come-first-served. That
bookkeeper is called a **matching engine**, and it's the beating heart
of every stock, futures, and crypto exchange.

Tachyon is a from-scratch matching engine for one symbol at a time,
built as a learning and portfolio project. Think of it as a fully
working model engine for a Formula 1 car: not the whole car, won't
race at Monaco, but every piston is the real thing.

Two things make it interesting:

- **It's fast.** On one CPU core of a normal Windows desktop, it
  processes about **4.5 million orders per second**, and each order is
  handled in roughly **300 nanoseconds**. A blink of an eye is about
  100 million nanoseconds — so one order is about one three-hundred-thousandth
  of a blink. That speed comes from using the same techniques
  high-frequency trading firms use: memory laid out so the CPU never
  has to fetch from far away, no memory allocations happening while
  trades are flowing, lookups that take the same fixed time no matter
  how busy the book gets.
- **It's fair and predictable.** It obeys "price-time priority" — best
  price wins, ties go to whoever arrived first — and refuses to let a
  trader accidentally trade with themselves. It supports the order
  types real traders actually use ("fill it now or kill it," "fill
  what you can right now and cancel the rest," etc.).

**What it honestly isn't:** it handles only one symbol, doesn't talk to
a real market, and has no permanent storage — pull the plug and
everything vanishes. The numbers above are the *core matching loop in
memory*, not a full exchange system. The point is to show I can build
the core to the same engineering standard as the people who do this
professionally.

## Numbers

Single-threaded core, commodity Windows desktop, MSVC `/O2`. These
measure the in-memory matching loop only — no network, no persistence,
single symbol:

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

## Five-minute local demo

Prerequisites: a C++17 compiler. CMake is recommended for the full test suite;
Node/npm are only needed for the dashboard. A more reviewer-oriented checklist,
including screenshot/GIF capture guidance and benchmark caveats, lives in
[`docs/demo.md`](docs/demo.md).

```sh
# Headless path: compile the two demo binaries directly and run bounded checks.
bash scripts/smoke_demo.sh

# Full CMake path: build apps + unit/property tests.
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure

# Scripted CLI walkthrough.
./build/bin/tachyon

# Bounded NDJSON stream for dashboard/file consumers.
./build/bin/tachyon_stream 5 2 0
```

Dashboard demo (local only, not a public hosted demo):

```sh
npm --prefix dashboard install
npm --prefix dashboard run check
TACHYON_STREAM=../build/smoke/tachyon_stream PORT=8080 N_EVENTS=500 DELAY_MS=20 npm --prefix dashboard start
# open http://localhost:8080
# health check from another shell: curl http://localhost:8080/health
```

On Windows with Visual Studio generators, the app binaries usually live under
`build/bin/Release/` and use `.exe` suffixes. The dashboard auto-detects both
single-config (`build/bin/tachyon_stream`) and Visual Studio-style paths; set
`TACHYON_STREAM=/path/to/tachyon_stream` to override it.

Configuration knobs are documented in [`docs/configuration.md`](docs/configuration.md).
Current project status and non-goals are summarized in [`docs/status.md`](docs/status.md).

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

```sh
# one-time after building tachyon_stream
npm --prefix dashboard install
npm --prefix dashboard run check

# every time
PORT=8080 DELAY_MS=20 npm --prefix dashboard start
# open http://localhost:8080
# health: curl http://localhost:8080/health
```

The Node script spawns the C++ `tachyon_stream` binary, reads its
NDJSON stdout, and rebroadcasts each event as a WebSocket frame. The
browser renders depth bars, trade tape, BBO, and rolling events/sec.
Set `TACHYON_STREAM` if the binary is not under `build/bin`; tune
`DELAY_MS=20` for a faster stream.

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

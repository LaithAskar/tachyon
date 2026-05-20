# Tachyon — design notes

Notes on how the matching engine is built and the decisions behind it.
Meant to record the bits that aren't obvious from reading the code.

## 1. Shape of the problem

A limit order book is two sorted ladders of price levels — `bids_` and
`asks_`. At each level, orders queue in arrival order (price-time
priority). Three operations have to be fast:

| op | what it does | target |
|----|--------------|--------|
| `submit` | match against opposite book, rest residual | `O(matched + log L)` |
| `cancel` | remove a specific resting order by id | `O(log L)` |
| `best_bid` / `best_ask` | read top of either side | `O(1)` |

Where `L` = number of distinct active price levels.

## 2. Data-structure choices

From `include/tachyon/order_book.hpp` and `include/tachyon/pool.hpp`:

```cpp
std::vector<Level>         bid_levels_;   // size = span_ = tick_max - tick_min + 1
std::vector<Level>         ask_levels_;
std::vector<std::uint64_t> bid_bitmap_;   // bit i = 1 iff bid_levels_[i] non-empty
std::vector<std::uint64_t> ask_bitmap_;
std::int64_t               best_bid_idx_;
std::int64_t               best_ask_idx_;
std::unordered_map<OrderId, OrderNode*> id_index_;
Pool                       pool_;          // block-growing OrderNode allocator
```

### Flat tick-indexed ladder

Each side is a `std::vector<Level>` sized once to the symbol's full tick
range. Looking up a price level is a direct index — no tree walk, no
hash. Indices into the vector are stable for the lifetime of the book,
so `id_index_` can store `OrderNode*` directly without iterator-stability
gymnastics.

The price for the O(1) lookup is space: 200k Level slots × 32 bytes ≈
6 MB per side of mostly-empty memory. We mitigate that with a bitmap
(below) so the hot path only ever touches occupied cells; the unused
slots never get pulled into cache.

### Per-side uint64 bitmap

`bid_bitmap_[k]` has bit `i` set iff `bid_levels_[k*64 + i]` is
non-empty. Finding the new best after a level drains becomes a
backward bit-scan: at most `span_/64 ≈ 3125` words, fast on
`_BitScanReverse64` / `__builtin_clzll`. In practice the scan finds the
next non-empty word in the first few iterations because tight markets
cluster around the BBO.

This is the structure that lets the flat ladder beat the tree in the
worst case as well as the average case. Without the bitmap, drain-on-best
would scan the wide vector cell by cell — pathological on sparse books.
With it, the upper bound is independent of how many empty cells sit
between active levels.

### Intrusive linked list per level, backed by a pool

Each `Level` is an intrusive doubly-linked list of `OrderNode`:

```cpp
struct OrderNode {
    Order      order;
    OrderNode* prev = nullptr;
    OrderNode* next = nullptr;
};
struct Level {
    OrderNode*  head        = nullptr;
    OrderNode*  tail        = nullptr;
    Quantity    total_qty   = 0;
    std::size_t order_count = 0;
};
```

`std::list<Order>` allocates per node through the default allocator,
which costs us a `new` on every resting order. The intrusive list with
a pool means the hot path never sees `malloc`: `acquire()` pops from a
free-list, `release()` pushes back. When the free-list is empty we
allocate a fresh fixed-size block (`std::unique_ptr<OrderNode[]>`) and
thread its nodes onto the free-list — adding capacity without
invalidating any previously-handed-out `OrderNode*` (the key property
that lets `id_index_` store raw pointers).

### `id_index_` cancellation cost

Cancel is now: one hash lookup → one `unlink()` from the doubly-linked
list (O(1)) → `pool_.release()` (O(1)) → if the level drained, one
bitmap update plus a backward bit-scan to find the new best (O(span/64)
words, almost always one). No tree erase, no level lookup. Real markets
cancel >90% of submitted orders; this is where the perf gain mostly
lands.

## 3. The bid-ordering decision (resolved)

The `std::map`-era asymmetry (asks via `begin()`, bids via `rbegin()`)
is gone. Both sides are `std::vector<Level>` indexed identically; the
asymmetry is now confined to "best advance" direction: after a level
drain, bids scan backward (`find_prev_set`), asks scan forward
(`find_next_set`). That asymmetry is one line each in `submit()` and
`cancel()`, gated on `Side`.

## 4. The matching algorithm

`src/order_book.cpp:18-67`. The flow of `submit()`:

1. Zero-quantity reject (defensive — a real exchange would NACK back).
2. Copy the input into a mutable `working` order so the caller's
   `const Order&` is untouched while quantity is decremented.
3. While the taker has residual and the opposite book is non-empty:
   - Pick the best opposite level. For a buy that's `asks_.begin()`;
     for a sell, `std::prev(bids_.end())`. Forward iterator on purpose
     so it can be passed to `map::erase` later.
   - Check the cross condition. Markets always cross. Limits cross
     when the buyer's price ≥ the maker's, or the seller's price ≤
     the maker's.
   - Sweep the level FIFO from front, emitting one `Trade` per maker
     touched. Trade price = **maker's** price (price improvement goes
     to the taker). On a fully filled maker, pop from the list and
     erase from `id_index_`. When the level drains, erase it from the
     map.
4. If the taker has residual and is a Limit, insert into its own side
   and record in `id_index_`. Market residuals fall through unhandled
   — the absence of an insert *is* the "drop residual" behaviour.

## 5. `cancel()` flow

After the v2 rewrite there's no `LevelHandle` indirection. `id_index_`
maps `OrderId → OrderNode*` directly. The flow:

1. Hash lookup → `OrderNode*` (or `false` if unknown).
2. Derive side and tick index from the node's order.
3. `unlink()` from the level's intrusive doubly-linked list — pure
   pointer swaps on `prev` / `next`, no allocator traffic.
4. `pool_.release(node)` — pushes node onto the free-list head, so the
   next `acquire()` returns this still-warm cache line.
5. If the level emptied: clear its bit in the bitmap, and if it was the
   best on its side, run a bit-scan to advance to the new best.

`OrderNode` pointers stay valid across unrelated insert/cancel because
the pool only ever appends new blocks — it never reallocates an
existing block. This is the property `std::list<Order>` used to give us
(stable iterators), now provided more cheaply.

## 6. Test design — why the replay test is there

The hand-written tests in `tests/test_order_book.cpp` cover named
scenarios: single insert, multi-level sweep, FIFO at a level, partial
fill, market sweep. They're good but they only catch the cases I
thought of when writing them.

`OrderBook.ReplayInvariants` takes the opposite approach: 5000 random
orders into a fresh book, and after every submit assert four
**invariants** that must hold regardless of the input:

1. **Per-trade balance** — `buy_volume_filled == sell_volume_filled` at
   the end. Every `Trade` represents one buyer meeting one seller, so
   filled buy volume must always equal filled sell volume. Divergence
   means double-counted or lost quantity.
2. **Never crossed** — if both sides are non-empty, `*best_bid <
   *best_ask` strictly. A crossed book after a submit means the
   matching loop terminated early.
3. **Bounded growth per submit** — a single submit can add at most one
   resting order (the residual). Matches only shrink the book. If size
   grew by more than 1, residual handling is double-firing.
4. **Full drain** — after the run, cancelling every still-live limit id
   must drain the book to zero, and the count of successful cancels
   must equal the final book size. Catches stale entries in
   `id_index_` and live orders missing from it.

Property-based-testing style: don't enumerate inputs, enumerate the
invariants the system should always preserve, then throw entropy at it.

## 7. Benchmark methodology

Two binaries because they answer two questions.

**`bench_throughput.cpp`** (Google Benchmark) — *how many orders/sec*.
Three workloads:

| benchmark | what it stresses |
|-----------|------------------|
| `BM_InsertNoCross` | pure inserts, book grows monotonically |
| `BM_MixedFlow` | frequent matches, book stays small |
| `BM_Cancel` | post-build cancel-everything |

Per-iteration setup (fresh `OrderBook`) is excluded via
`state.PauseTiming()` / `ResumeTiming()`. `benchmark::DoNotOptimize` is
the compiler barrier — without it, the optimiser deletes the work in a
Release build because the result is unread.

**`bench_latency.cpp`** (standalone) — *per-op latency distribution*.
Google Benchmark reports means; matching-engine work is about tails.
Methodology:

1. Pre-warm to steady state (50k non-crossing inserts) so the data
   structures and allocator are hot.
2. Pre-generate the timed flow so RNG cost is outside the loop.
3. Time each `submit()` with `steady_clock::now()`, store nanos in a
   flat vector.
4. Sort, report p50/p90/p99/p99.9/max.

The `volatile std::size_t sink = trades.size();` is a
no-Google-Benchmark equivalent of `DoNotOptimize` — a write to a
`volatile` can't be elided.

On Windows `steady_clock` has roughly 100ns resolution and the OS
scheduler introduces multi-millisecond pauses at the high end of the
distribution. Measuring sub-microsecond tails honestly needs core
pinning, real-time priority, and a higher-resolution clock — see
`bench_latency_rdtsc.cpp` below.

**`bench_latency_rdtsc.cpp`** (standalone) — *per-op latency with cycle
resolution*. Same shape as the steady_clock version but:

1. Pin the measuring thread to CPU 0 via `SetThreadAffinityMask`. TSC
   is per-core; migrations mid-measurement give garbage.
2. `SetThreadPriority(THREAD_PRIORITY_TIME_CRITICAL)`. Best Windows can
   do without DPC-level hacks — it does **not** make the thread
   preempt-immune.
3. `lfence` + `__rdtscp(&aux)` bracketing each operation. `__rdtscp`
   serialises on retire (waits for prior µops); `lfence` stops later
   µops from issuing before the read. The pair brackets the op tightly.
4. Calibrate the TSC against `steady_clock` once at startup (~100ms
   anchor), then convert cycles → ns.
5. Three scenarios measured separately — submit-no-match,
   submit-with-match, cancel — instead of the mixed-flow blend.
   Split lets us see which path dominates which percentile.

What this still doesn't control for: SMT siblings, turbo-boost
P-state changes, interrupts, DPCs. All show up as tail. If `max` is
>100× `p99.9`, something jittered — re-run rather than trust it.

### Current numbers (post-v2 internals, commodity Windows desktop)

### Current numbers (post-v2 internals, commodity Windows desktop)

| benchmark | throughput | notes |
|-----------|------------|-------|
| `BM_MixedFlow` (value-return) | ~3.1M ops/sec | dominated by per-call `vector<Trade>` allocation |
| `BM_MixedFlow_OutParam`       | **~4.5M ops/sec** | the real hot-path number |
| `BM_Cancel`                    | ~5.0M ops/sec | cancel is now O(1) hash + O(1) unlink |
| `BM_InsertNoCross`             | ~2.6M ops/sec | grows the book; touches one fresh level per insert |

Latency from `bench_latency` (50k prewarm, 100k measured, mixed flow,
steady_clock):

| percentile | latency |
|------------|---------|
| p50        | **300 ns** |
| p90        | 700 ns |
| p99        | **1.6 µs** |
| p99.9      | 11 µs (scheduler-bound) |
| max        | 4.3 ms (single OS preemption) |

p99 sits 10× under the README's original 20µs target.

Latency from `bench_latency_rdtsc` (200k samples per scenario, TSC
2.918 GHz, pinned to CPU 0, `TIME_CRITICAL` priority):

| percentile | submit (no match) | submit (with match) | cancel |
|------------|-------------------|---------------------|--------|
| min        | 61 ns             | 64 ns               | 60 ns  |
| p50        | **149 ns**        | **193 ns**          | **108 ns** |
| p90        | 337 ns            | 303 ns              | 182 ns |
| p99        | **567 ns**        | **438 ns**          | **299 ns** |
| p99.9      | 21 µs             | 671 ns              | 438 ns |
| p99.99     | 176 µs            | 17 µs               | 6.8 µs |
| max        | 4.6 ms            | 277 µs              | 120 µs |

Reading the table honestly:

- **The steady_clock p50 (300 ns) was high.** With cycle-resolution
  measurement the body-of-distribution submit cost is ~150 ns
  (no-match) or ~190 ns (with-match), not 300. The earlier number was
  partly real and partly the ~100 ns floor of `steady_clock` showing
  up as a fixed offset.
- **Cancel is the cheapest path.** p50 108 ns — one hash lookup, one
  unlink, one pool release, occasionally one bit-scan. The earlier
  ~5M ops/sec throughput number is the corresponding number from the
  other direction (200 ns/op).
- **submit-no-match has the worst p99.9** by a wide margin (21 µs vs
  670 ns for submit-with-match). The likely reason is Pool block
  growth: when the free-list is exhausted, `acquire()` allocates a
  fresh `OrderNode[4096]` block, which is a `new` and pays the cost
  of however long the page allocator decides to take. With-match
  paths don't grow the pool. Removing that tail would need either
  pre-warm of more pool blocks or a fallback path that's allocation-
  free in the bad case.
- **The 4.6 ms max is one preemption.** TIME_CRITICAL is best-effort,
  not real-time. On commodity Windows there is no way to make this
  go away without driver-level intervention; the number is a
  statement about the OS, not the engine.

## 8. Known limitations (post-v2)

The map → flat ladder and list → intrusive-list-with-pool refactors
landed. The remaining honest gap list:

- **`std::vector<Trade>` returned by value** still allocates per call on
  the convenience overload. The out-param `submit(const Order&,
  std::vector<Trade>& out)` is the production path; the value-return
  form exists for callers that don't care about steady-state cost.
- ~~**Single-threaded by design.**~~ As of phase 3 the ingest/matching
  split has landed: `SpscQueue` (header-only, lock-free,
  cache-line-padded), `ThreadedMatcher` (single-symbol OrderBook behind
  a worker thread), and `Exchange` (N symbols, one worker dispatching by
  `symbol_id`). The OrderBook itself stays single-threaded inside the
  worker; the queues only synchronise the boundaries. The rdtsc numbers
  above are bare-book costs and don't include SPSC enqueue/dequeue.
- **Default allocator only at startup.** The pool grows in blocks via
  `std::make_unique<OrderNode[]>`; once warmed, the hot path is
  malloc-free. Cold start still pays per-block.
- **No advanced order types beyond IOC/FOK.** Iceberg, post-only, GTD,
  stop-limit — all unsupported. Each is a localised matching-loop
  change but not free to implement and test.
- **Tick range is configured per-book at construction.** Default
  `(0, 200'000)` covers `$0.00–$2000.00` for cent-tick instruments. A
  symbol whose range shifts during the trading day (a stock split, a
  halt-and-reopen) needs a different scheme (sliding window over the
  BBO, or a `unordered_map<Price, Level*>` overflow tier).

Complexity, post-v2 internals:

| op | complexity |
|----|------------|
| `submit` (no match) | `O(1)` |
| `submit` (matches k orders, drains d levels) | `O(k + d · span/64)` |
| `cancel` | `O(1)` amortised; worst `O(span/64)` to find new best |
| `best_bid` / `best_ask` | `O(1)` |

`span` is the tick range (200k by default), so `span/64 ≈ 3125` words —
a bounded constant. Active books rarely require more than a one-word
scan because non-empty levels cluster around the BBO. The asymptotic
gain over the v1 (`O(log L)` tree walks) is real but the bigger win is
constant-factor: no allocator on the hot path, no tree-node
pointer chasing, one cache line per level access.

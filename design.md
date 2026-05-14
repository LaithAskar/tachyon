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

From `include/tachyon/order_book.hpp`:

```cpp
using BookSide = std::map<Price, std::list<Order>>;

BookSide bids_;
BookSide asks_;
std::unordered_map<OrderId, LevelHandle> id_index_;
```

### `std::map` for the price ladder

Chose `std::map` (red-black tree) over `std::unordered_map` and over a
sorted `std::vector` because of one property the alternatives don't
have: **iterator stability under unrelated insert/erase**. The
`id_index_` stores iterators *into* this map, and those iterators need
to stay valid when the surrounding code modifies other levels.
`unordered_map` doesn't guarantee that across rehashes; `vector`
invalidates anything after the insertion point.

The price I pay is `O(log L)` where I'd want `O(1)`, and pointer chasing
where I'd want contiguous memory. Right tradeoff for a v1, wrong for
production — see §7.

### `std::list` per level

Same reasoning. Each level needs a FIFO queue: push_back on submit,
pop_front when the head is fully filled, and erase-from-the-middle on
cancel. `std::list` is `O(1)` for all three given an iterator, and its
iterators are stable. `std::deque` invalidates iterators on erase, which
would break the id index immediately.

### `id_index_` for `O(log L)` cancel

```cpp
struct LevelHandle {
    Side                       side;       // which book
    BookSide::iterator         level_it;   // which price level
    std::list<Order>::iterator order_it;   // which order in the level
};
```

Without this index, every cancel scans the affected level's list looking
for the id — `O(depth_at_level)`. With it, cancel is one hash lookup,
one `O(1)` list erase, and at most one `O(log L)` level erase when the
level drains.

Cancel speed matters because real markets cancel more than 90% of
submitted orders. If cancel is slow, the whole engine is slow.

## 3. The bid-ordering decision

`std::map` defaults to ascending order, so `asks_.begin()` is the best
ask (lowest price). For bids, "best" is the *highest*, so
`bids_.begin()` is wrong. Two options:

| approach | type effect |
|----------|-------------|
| (a) Two typedefs: `std::map<Price, ..., std::greater<>>` for bids | `LevelHandle::level_it` can't be a single iterator type — needs variant or two LevelHandle types |
| (b) One typedef, ascending, use `rbegin()` for bids | `LevelHandle` stays a single POD-like struct |

Picked (b). The asymmetry is annoying but localised to two lines in
`best_bid()`/`best_ask()`, and keeping `LevelHandle` uniform keeps
`id_index_` simple. If `std::map` gets swapped for a flat ladder later
(it should — §7), the typedef discipline matters less because there'll
be a custom Level type either way.

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

## 5. `cancel()` — one subtle point

```cpp
const LevelHandle h = idx_it->second;  // copy, not reference
```

The copy is deliberate. The function calls `id_index_.erase(idx_it)` at
the end. If `h` were a reference into the index entry, it would dangle
between the index erase and the function return. The current code
doesn't read `h` after the erase, but holding a reference is the kind
of subtle detail a later refactor would break silently.

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
distribution. Measuring sub-microsecond tails honestly would need core
pinning, real-time priority, and probably `QueryPerformanceCounter`
direct. Out of scope for v1.

## 8. Known limitations

The implementation hits the README's stated targets (1M+ ops/sec
throughput, p99 < 20µs) but it isn't production-grade. The honest gap
list:

- **`std::map` is cache-hostile.** Level accesses pointer-chase through
  tree nodes. A flat array indexed by tick number (or a sorted
  `vector<Level*>` plus a price→slot hash) would be measurably faster.
- **`std::list` is also pointer-chasing.** An intrusive linked list
  with prev/next pointers embedded in `Order`, backed by a pool
  allocator, would keep nodes contiguous in memory.
- **Default allocator on the hot path.** Every rested order is two
  trips through `new`. A pre-allocated pool at startup avoids that.
- **`std::vector<Trade>` returned by value** allocates on every call
  that produces trades. Better signature: `void submit(const Order&,
  std::vector<Trade>& out)` so the caller reuses one vector across
  millions of calls.
- **No self-cross prevention.** `Order` has no `account_id`, so two
  orders from the same account can match each other. Needs a trader id
  before this goes near anything real.
- **No advanced order types.** Iceberg, IOC, FOK, post-only, GTD — all
  unsupported. Each is a relatively localised change to the matching
  loop but they're not free.

Complexity, for reference:

| op | complexity |
|----|------------|
| `submit` (no match) | `O(log L)` |
| `submit` (matches k orders) | `O(k + log L)` |
| `cancel` | `O(log L)` |
| `best_bid` / `best_ask` | `O(1)` |

`L` is bounded in practice (a tight book has hundreds to low thousands
of active levels), so `log L` is small. The wins from the changes above
are constant factors and tail latency, not big-O.

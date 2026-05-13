# whatidid.md — Tachyon weeks 1–4, taught

This is the explanation you'd get if I were walking a study partner through
the code. Open the referenced files alongside it.

---

## 1. The 30-second version

A limit order book holds two sorted ladders of price levels — `bids_` (people
who want to buy) and `asks_` (people who want to sell). At each price level,
orders queue in arrival order (FIFO). When a new order arrives, the engine
walks the opposite ladder from its best price inward, matching against the
queue at each level until either the incoming order is fully filled or the
prices stop crossing.

Three operations matter and they must all be fast:

| op           | what it does                            | target complexity |
|--------------|-----------------------------------------|-------------------|
| `submit`     | match against opposite book, rest residual | `O(matched + log L)` |
| `cancel`     | remove a specific resting order by id   | `O(log L)` |
| `best_bid` / `best_ask` | read top of either side       | `O(1)` amortised |

Where `L` is the number of distinct price levels currently in the book.

---

## 2. The data structures, and why each was chosen

From `include/tachyon/order_book.hpp:41-46`:

```cpp
using BookSide = std::map<Price, std::list<Order>>;

BookSide bids_;
BookSide asks_;
std::unordered_map<OrderId, LevelHandle> id_index_;
```

Let's pull this apart piece by piece.

### 2a. `std::map<Price, ...>` for the price ladder

`std::map` is a balanced binary search tree (red-black in libstdc++/MSVC).
Three properties make it right here:

1. **Sorted by key.** `begin()` is the lowest price, `rbegin()` is the highest.
   That gives us `best_ask` in O(1) (it's `asks_.begin()`) and `best_bid` in
   O(1) (it's `bids_.rbegin()`).
2. **O(log L) insert / erase / lookup by key.** When a new price level needs
   to be created, or an emptied one removed, we pay log L.
3. **Iterator stability.** This is the part that's easy to miss but is the
   whole reason we don't use a vector. When you insert or erase one element
   in a `std::map`, iterators to *other* elements remain valid. We rely on
   this because the `id_index_` stores iterators into the map.

Why not `std::unordered_map<Price, ...>`? It's faster per op (O(1) hash)
but **unordered** — you can't ask "what's the highest bid?" without scanning
every level. Top-of-book is the most-read quantity in any book, so we sort.

Why not `std::vector<std::pair<Price, ...>>` kept sorted? Insert in the middle
is O(L), and any insert/erase invalidates iterators after the touched
position. That would break `id_index_` immediately. (Real high-performance
books *do* use sorted vectors — but they pay for it with custom indirection
schemes. See §8.)

### 2b. `std::list<Order>` for the FIFO at each level

At a single price level, "price-time priority" means earlier arrivals match
first. So each level needs a FIFO queue. We need:

- Cheap push at the back (new orders arrive at the end of the queue).
- Cheap pop at the front (the next maker to be filled is at the head).
- Cheap erase in the middle (a `cancel` of a non-head order).

`std::list` is a doubly-linked list. All three operations are O(1) **if you
already have an iterator to the element**. And — same key property as the
map — `std::list` iterators are stable under unrelated insert/erase.

That iterator stability is what `id_index_` exploits.

Why not `std::deque`? It supports push_back and pop_front in O(1), but
erasing from the middle is O(n) and **invalidates all iterators**. The
moment you cancel order #5 in the middle of a 10-deep level, every iterator
in `id_index_` for that level is corrupt.

Why not `std::vector`? Same problem, worse.

### 2c. `id_index_`: the O(1) cancel trick

```cpp
struct LevelHandle {
    Side                       side;       // bid book or ask book?
    BookSide::iterator         level_it;   // which price level
    std::list<Order>::iterator order_it;   // which order in that level's list
};
std::unordered_map<OrderId, LevelHandle> id_index_;
```

Without this, cancelling order #42 would mean *scanning every level of both
sides* looking for it. That's O(N) on the book size — completely unacceptable
when cancels are routine (real markets cancel >90% of submitted orders).

With it, `cancel(42)`:

1. Hash-looks up `42` in `id_index_` → O(1) amortised
2. Erases the order from its level's list using the stored list iterator → O(1)
3. If the level is now empty, erases the level from the map → O(log L)
4. Erases from `id_index_` → O(1)

The whole thing is O(log L), dominated by the optional level erase. This is
exactly the trade real exchanges make.

---

## 3. Walking through `submit()`

`src/order_book.cpp:18-67`. Read it open while you read this.

### 3a. The pre-checks

```cpp
if (order.quantity == 0) return trades;
Order working = order;
```

Zero-quantity orders are silently rejected (no trades, no insert). A real
exchange would `NACK` back; we just drop them.

The copy into `working` matters — we mutate `working.quantity` as we fill,
but the caller's `order` is `const`. The taker's residual lives in
`working.quantity` until either the loop ends or we insert it.

### 3b. The matching loop

```cpp
BookSide& opposite = (working.side == Side::Buy) ? asks_ : bids_;

while (working.quantity > 0 && !opposite.empty()) {
    BookSide::iterator best_it =
        (working.side == Side::Buy) ? opposite.begin() : std::prev(opposite.end());
    const Price best_px = best_it->first;

    if (!crosses(working.side, working.type, working.price, best_px)) break;
    ...
}
```

Two things to notice:

**Which iterator points at "best"?** If we're a buyer matching against asks,
the best ask is the **lowest**, which is `asks_.begin()`. If we're a seller
matching against bids, the best bid is the **highest**, which is
`std::prev(bids_.end())` (the last element of an ascending-ordered map).
Note we use `std::prev(end())` and not `rbegin()` — we need a *forward*
iterator because we may want to `erase` this level later, and
`std::map::erase` takes a forward iterator.

**The `crosses` check** (`src/order_book.cpp:11-15`):

```cpp
if (taker_type == OrderType::Market) return true;
return taker_side == Side::Buy ? taker_price >= best_opposite
                               : taker_price <= best_opposite;
```

- A market order crosses by definition — it'll take whatever's there.
- A buy limit crosses an ask if buyer's price ≥ asking price.
- A sell limit crosses a bid if seller's price ≤ bidding price.

If we *don't* cross, the loop breaks and we fall through to the insert.

### 3c. Sweeping a level

```cpp
std::list<Order>& level = best_it->second;
while (working.quantity > 0 && !level.empty()) {
    Order& resting = level.front();
    const Quantity fill = std::min(working.quantity, resting.quantity);

    trades.push_back(Trade{working.id, resting.id, best_px, fill, ...});

    working.quantity -= fill;
    resting.quantity -= fill;

    if (resting.quantity == 0) {
        id_index_.erase(resting.id);
        level.pop_front();
        --total_orders_;
    }
}
```

Each iteration eats the head-of-queue maker. The fill quantity is the
**minimum** of how much the taker still wants and how much this maker still
has — that's how partial fills naturally fall out. Three outcomes per
iteration:

1. Maker fully filled, taker still wants more → pop maker, continue at same
   level (next maker in queue).
2. Taker fully filled, maker has residual → leave maker in place, exit loop.
3. Both exhausted simultaneously → maker is popped, taker has 0 quantity, exit.

Note the trade price is **`best_px`**, the *maker's* posted price — not the
taker's. This is "price improvement": if a buyer is willing to pay $105 and
the best ask is $100, the trade prints at $100. The taker gets a better
deal than they asked for. (Test `AggressiveBuyTakesPriceImprovement` checks
exactly this — `tests/test_order_book.cpp:86-94`.)

When the level empties, we drop out of the inner loop and:

```cpp
if (level.empty()) {
    opposite.erase(best_it);
}
```

remove the empty level from the map. The outer loop then re-evaluates
`opposite.empty()` and (if still non-empty) picks a new best.

### 3d. Resting the residual

After the matching loop:

```cpp
if (working.quantity > 0 && working.type == OrderType::Limit) {
    BookSide& own = (working.side == Side::Buy) ? bids_ : asks_;
    BookSide::iterator level_it = own.try_emplace(working.price).first;
    level_it->second.push_back(working);
    std::list<Order>::iterator order_it = std::prev(level_it->second.end());
    id_index_.emplace(working.id, LevelHandle{working.side, level_it, order_it});
    ++total_orders_;
}
```

If there's residual *and* this is a Limit order, we add it to our own side.
`try_emplace` creates the level if it doesn't exist or returns the existing
one — either way you get the iterator. We push_back (FIFO), grab the
iterator to what we just pushed, and record everything in `id_index_`.

Market orders skip this block entirely — that's how "market residual is
dropped" is enforced. There's no explicit `else` clause; the absence of an
insert *is* the drop.

---

## 4. Walking through `cancel()`

`src/order_book.cpp:69-83`. Much shorter:

```cpp
auto idx_it = id_index_.find(id);
if (idx_it == id_index_.end()) return false;

const LevelHandle h = idx_it->second;  // copy, NOT reference
BookSide& side_book = (h.side == Side::Buy) ? bids_ : asks_;

h.level_it->second.erase(h.order_it);
if (h.level_it->second.empty()) {
    side_book.erase(h.level_it);
}
id_index_.erase(idx_it);
--total_orders_;
return true;
```

The one subtle line is `const LevelHandle h = idx_it->second;` — that's a
**copy**, not a reference. If we held a reference and then called
`id_index_.erase(idx_it)` at the end, the reference would dangle in
between. Even though we don't read `h` after the erase, this is the kind of
mistake that bites later when someone "helpfully" reorders the function.

Same pattern: erase the list node first (uses `h.order_it`), then conditionally
erase the level (uses `h.level_it`), then erase the index entry.

---

## 5. The bid-ordering decision, properly

The header you originally wrote used a single `BookSide` typedef for both
sides. That collapses the bid map and ask map to the **same type**, which
in turn means `LevelHandle::level_it` can be a single iterator type. Nice
and uniform.

But it means `bids_` uses the default `std::less<Price>` comparator, so
`bids_.begin()` is the **lowest** bid — the *worst* price for the buyer.
The README's hint was `std::map<Price, ..., std::greater<>>` for bids
specifically so `begin()` would be the highest. Two ways out:

| approach | pros | cons |
|----------|------|------|
| (a) two typedefs, `std::greater` for bids | `bids_.begin()` is best, symmetric "begin = best" pattern | `LevelHandle::level_it` can't be a single type → variant, std::any, or two LevelHandle types |
| (b) one typedef, ascending, use `rbegin()` for bids' best | `LevelHandle` stays trivial | "best" idiom isn't symmetric (begin for asks, rbegin for bids) |

I picked (b). The asymmetry is annoying but localised (`best_bid()` is two
lines), and `LevelHandle` staying as a single PODlike struct keeps
`id_index_` simple. If you replace `std::map` later (you should — see §8),
the typedef discipline matters less because you'll have a custom Level
type anyway.

---

## 6. The tests — what each one is actually checking

`tests/test_order_book.cpp`. They're organised in three blocks matching the
weekly milestones.

### Week 1 (lines 25-64): the skeleton works
- `EmptyOnConstruction` — default-constructed book reports no top-of-book.
- `SingleBuyLimitBecomesBestBid` — a single resting bid round-trips through
  `submit` → `best_bid`.
- `MultipleBids/AsksBestIsHighest/Lowest` — top-of-book picks the right end
  of the ladder. This is the test that would have **failed** on your
  original header before the bid-ordering fix.
- `CancelRemovesRestingOrder` — cancel works, and cancelling a non-existent
  id returns false (not a crash).
- `CancelEmptiesLevel` — cancelling the last order at a level removes the
  level entirely (otherwise top-of-book lies).

### Week 2 (lines 68-141): matching is correct
- `CrossingLimitGeneratesTrade` — the simplest possible match.
- `AggressiveBuyTakesPriceImprovement` — checks that the print is the
  maker's price, not the taker's.
- `PartialFill...` — verifies both halves of a partial fill: leftover maker
  on one test, leftover taker (residual inserted) on the other.
- `FifoAtSamePriceLevel` — three sells at the same price, the buyer takes
  exactly 7 units; the test asserts the first sell is fully filled and the
  *second* sell is partially filled. If the FIFO were broken (LIFO, or
  random), the assertion on `trades[0].maker_id == 1` would fail.
- `SweepMultipleLevels` — a taker eats through level after level until its
  limit price stops it. Catches the case where the matching loop forgets
  to re-evaluate the new best after eating a level.
- `NonCrossingLimitJustRests` — both sides resting, no trade. The
  symmetric base case for the matching loop.

### Week 3 (lines 145-200): edge cases + replay
- `MarketBuyConsumesBook` — markets cross at any price.
- `MarketOrderResidualIsDropped` — the test that proves market orders are
  NOT inserted when the book runs out.
- `MarketOrderOnEmptyBookDoesNothing` — degenerate but easy to break.
- `ZeroQuantityRejected` — the explicit reject path.
- `ReplayInvariants` — this is the interesting one.

### The replay test, explained

Five thousand random orders. Mixed bids/asks, ~10% market. After every
submit we check four invariants. Each one catches a different class of bug:

1. **Per-trade balance**: `buy_volume_filled == sell_volume_filled` at the
   end. Every Trade increments **both** sides equally because every Trade
   is a meeting of one buyer and one seller. If this ever diverges, you've
   double-counted or lost quantity somewhere. (Catches: forgetting to
   decrement the maker, double-emit on partial fill.)
2. **Never crossed**: after any submit, if both sides are non-empty, the
   best bid must be strictly less than the best ask. (Catches: the
   matching loop terminating before it should — e.g., breaking on the
   wrong comparator.)
3. **Bounded growth**: a single submit can create **at most one** new
   resting order (the residual). Matches only *reduce* `size()`. (Catches:
   accidentally inserting the residual on top of inserting the full
   incoming order. Subtle but possible.)
4. **Full drain**: after the random stream, every still-live limit id must
   be cancellable, and the count of successful cancels must equal the
   final book size. (Catches: stale entries in `id_index_`, or live orders
   that aren't in `id_index_`. This is the canary for index leaks.)

This is the same pattern as property-based testing: don't hand-craft
inputs, hand-craft *invariants the system should always satisfy*, then
beat it with random inputs.

---

## 7. The benchmarks

Two binaries because they answer two different questions.

### `bench_throughput.cpp` (Google Benchmark)

Question: **"How many orders/sec can the engine process?"** Google
Benchmark runs the timed loop as many times as needed to reach statistical
confidence (default: ~1 sec of wall time per benchmark).

Key technique: `state.PauseTiming() / ResumeTiming()` to exclude the
per-iteration setup (creating a fresh `OrderBook`) from the measured time.
Otherwise allocating an empty book would skew small `N`.

`benchmark::DoNotOptimize(trades)` is a compiler barrier — without it, the
optimiser might notice `trades` is unused and elide the work entirely.
That happens in a Release build of a benchmark *constantly*.

`SetItemsProcessed(...)` turns wall-time into the `items_per_second`
counter you see in the output.

Three workloads:

| benchmark | what it stresses |
|---|---|
| `BM_InsertNoCross` | pure inserts, book grows monotonically — worst case for map size |
| `BM_MixedFlow` | matches frequently, book stays small |
| `BM_Cancel` | post-build, cancel-everything — pure `id_index_` + list erase |

### `bench_latency.cpp` (standalone)

Question: **"What's the per-operation latency distribution?"** Google
Benchmark reports means, not percentiles, and the means hide what an
interview shop actually cares about — the *tail*.

Method:

1. Pre-warm with 50k non-crossing inserts (steady-state book, ~100 levels
   per side, hot allocator).
2. Pre-generate 200k mixed orders so the RNG cost is outside the timed
   loop.
3. Time each `submit()` individually with `std::chrono::steady_clock`.
4. Sort the array of latencies, report the indices at 50/90/99/99.9 percent.

The `volatile std::size_t sink = trades.size();` is the no-Google-Benchmark
equivalent of `DoNotOptimize`: a write to a `volatile` cannot be elided.

Why two scenarios (insert-only vs mixed): the insert path stresses tree
inserts and the allocator; the mixed path stresses the matching loop and
the index-erase path. Different bottlenecks → different tails.

**Caveats baked into the code as comments at the top**: on Windows
`steady_clock` has ~100 ns resolution and the OS scheduler will introduce
multi-millisecond pauses at p99.9+. To measure sub-microsecond tails
honestly you'd want core pinning, real-time priority, and possibly
`QueryPerformanceCounter` directly. None of that is in scope here.

---

## 8. What's actually wrong with this design (read this before any interview)

This passes the README's stated targets but you should not walk into a
Citadel interview claiming this is fast. Honest assessment:

### 8a. `std::map` is the wrong data structure for top-of-book

Trees pointer-chase. Every `submit` that walks levels triggers cache
misses at every tree node visit. The numbers you see (~600 ns p50) are
*fine for an undergrad project* and *bad for a serious matching engine*.

What real engines use: a **dense array indexed by tick number** (one slot
per representable price), or a **flat sorted vector of pointers to active
levels** with a parallel hash from price to slot. Both are cache-friendly
in a way the tree isn't.

### 8b. `std::list` per level is also pointer-chasing

Same problem at the level scale. A linked list of orders means each
`pop_front` during a sweep is a cache miss into a separately-allocated
node. Real engines use an **intrusive linked list** (the prev/next
pointers live inside the `Order` itself) with a **pool allocator** so all
nodes live contiguously in memory.

### 8c. The default allocator is on the hot path

Every `submit` that rests an order does one `std::map` insert (allocates a
tree node) and one `std::list::push_back` (allocates a list node). That's
two trips through the global allocator per insert. For a high-rate feed
that's a giant fraction of the cost. Real engines pre-allocate a fixed
pool at startup and never call `new` in steady state.

### 8d. Returning `std::vector<Trade>` by value allocates on the match path

Look at `submit`'s return type. Every call constructs a vector, every call
that produces trades grows it (more allocations), every caller takes
ownership and eventually destroys it. The README literally calls this out
(`README.md:79-81`). The right move is to take an output reference
(`void submit(const Order&, std::vector<Trade>& out)`) so the caller can
reuse the same vector across millions of calls.

### 8e. Complexity in big-O terms

| op | this implementation | what an interviewer expects you to know |
|----|---------------------|----------------------------------------|
| `submit` (no match) | O(log L) | dominated by tree insert |
| `submit` (matches k orders) | O(k + log L) | k for the sweep, log L for level/index updates |
| `cancel` | O(log L) | dominated by potential level erase |
| `best_bid` / `best_ask` | O(1) amortised | constant — top is cached at end of tree |

If they ask "what if cancels happen 100× more often than inserts?", the
answer is: this design is still fine, because cancel is already O(log L).

If they ask "what if you need 10M ops/sec?", you say: replace the map
with a flat ladder, the list with an intrusive list + pool, the return
vector with an out-param, and pin the matching thread to a core. You're
not there. You know how to get there.

### 8f. Things you didn't implement that real books need

- **Self-cross prevention** — orders from the same account shouldn't trade
  against each other. `Order` has no `account_id`. Out of scope here, but
  you'd want to add it before this code goes near anything real.
- **Iceberg orders** — show only part of the size publicly, reveal more as
  it gets eaten. No support.
- **Post-only / fill-or-kill / immediate-or-cancel** — order type
  modifiers that change matching semantics. No support.
- **Time-in-force** — orders that auto-cancel at a deadline. No support.

These aren't in your roadmap. If an interviewer asks "what would you add
next", these are the right next things.

---

## 9. Suggested study order

If you want this to stick before an interview:

1. Re-read `submit()` end-to-end with the test cases open. Trace the
   working quantity through a partial fill by hand.
2. Cover up `cancel()` and try to rewrite it. The mistakes you'll make
   (dangling reference into `id_index_`, forgetting to erase the level,
   wrong `total_orders_` update) are the ones interviewers probe.
3. Take the replay test and add one more invariant. Make it fail
   intentionally by introducing a bug, then watch which invariant catches
   it.
4. Read `bench_latency.cpp` and explain to yourself why each design
   choice (pre-warm, pre-generate, volatile sink) is needed.
5. Implement §8a — replace `std::map` with a `std::vector<std::list<Order>>`
   indexed by price (you'll need a min/max price range). Run the
   benchmarks. Compare. *That's* the interesting exercise.

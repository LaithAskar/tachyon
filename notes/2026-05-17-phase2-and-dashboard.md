# Tachyon — study notes, session of 2026-05-17

Purpose of this file: a re-readable cheat-sheet for everything that
landed today, framed so you can defend each decision in interview
questioning. For every change there's WHAT it is, WHY this approach,
the KEY DECISION points that have plausible alternatives, and PROBES
an interviewer can throw at you.

If you cannot answer a probe without looking at the code, that's a hole
to fix. The order of sections matches the order we built them in.

---

## 0. Where we started, where we ended

- **Start of session.** Phase 1 done since 2026-05-13. `std::map<Price,
  std::list<Order>>` per side, `LevelHandle` indirection in
  `id_index_`, 21 unit tests, `bench_throughput` reporting ~3.3M
  ops/sec mixed flow, README still describing planned work.
- **End of session.** Flat tick-indexed ladder with bitmap-routed best
  advance, intrusive doubly-linked list per level, pool allocator with
  stable pointers. STP, IOC, FOK shipped. JSON snapshot, NDJSON event
  stream, Node WS bridge, live vanilla-JS dashboard. 43 tests,
  ~4.5M ops/sec out-param mixed flow, p50=300 ns, p99=1.6 µs, README
  and design.md both rewritten.

The diff at a glance is in three pushed commits:
`a1d4373` (refinements) → `64dbcdd` (dashboard) → `5534e2c`
(flat-ladder + pool rewrite).

---

## 1. Out-param `submit(const Order&, std::vector<Trade>& out)`

### What
Added an overload to `OrderBook::submit` whose Trade output goes into a
caller-owned vector. The vector is `clear()`'d on entry, then appended
to. The value-return version (`std::vector<Trade> submit(const
Order&)`) became a one-line wrapper.

### Why
Returning `std::vector<Trade>` by value allocates per call on any
mixed-flow workload that produces trades. Real exchanges call `submit`
millions of times per second. A caller that reuses one Trade buffer
across the whole stream pays exactly one allocation total.

Measured: `BM_MixedFlow` 3.27M ops/sec → `BM_MixedFlow_OutParam` 4.04M
ops/sec on the same workload, std::map era. +23%.

### Key decision
Should `submit` `clear()` the buffer on entry, or require the caller to
do it? Chose to clear inside. The caller usually doesn't care about old
contents — making `clear()` part of `submit`'s contract removes a
foot-gun. Capacity survives `clear()`, so the allocation-free property
is preserved.

### Probes
- *Why does `clear()` not free the vector's buffer?* `std::vector::clear`
  is required to destroy elements but is allowed to keep the underlying
  allocation. For trivially-destructible `Trade` it's effectively a
  pointer-rewind to size 0.
- *What guarantees the value-return overload doesn't regress when used?*
  Nothing — the value-return form pays per-call allocation. That's the
  point; it exists for callers who don't care. If you wrote a benchmark
  showing the value-return path getting slower after the refactor, the
  default-ctor cost of the new flat ladder bleeding into the timed
  region is the explanation, not the engine.
- *Why not return `std::span<Trade>` from an internal buffer?* Lifetime
  hazard — the next `submit` would invalidate the span. Caller-owned
  storage is the right ownership model.

---

## 2. Self-trade prevention via `account_id`

### What
Added `AccountId Order::account_id` (default 0 = "unspecified", STP
off). In the inner matching loop, a maker is skipped if it shares a
non-zero `account_id` with the taker. Skipped makers stay in the queue
at their original position; FIFO among other accounts is preserved.

When a level's remaining orders are all same-account, the matching
engine stops — it does NOT try the next-worse level.

### Why
Real exchanges enforce STP because regulators (and self-preservation)
forbid an account from wash-trading against itself. Without it, your
own order can match your own resting orders.

### Key decisions
- **Where to encode "no account specified."** Picked `account_id = 0`
  as wildcard so every pre-existing test (which doesn't set account)
  keeps its semantics. Cheap default that avoids a flag day.
- **What to do when the best level is fully blocked by same-account
  makers.** Three valid behaviours: cancel-newest (drop the taker),
  cancel-oldest (cancel the conflicting resters), or skip-and-stop
  (taker partially fills against what it can, residual rests if Limit).
  Picked skip-and-stop because it preserves price priority — you
  cannot legally trade through your own orders at the best price to
  reach a worse level.

### Probes
- *Could you walk through what happens if a Buy from account 7
  arrives, the best ask is [account 7 sell, account 8 sell, account 7
  sell, account 9 sell]?* Loop skips the first, fills the second, skips
  the third, fills the fourth. The two account-7 sells stay where they
  were in FIFO. Test `SelfCrossSkipsOwnPreservesFifoAmongOthers` pins
  exactly this.
- *What if all orders at the best level are blocked, but a lower level
  has matchable orders? Does the taker reach the lower level?* No, by
  design. See `SelfCrossBlockedBestLevelStopsMatching`. The argument:
  price priority is sacred — skipping a level to reach a worse price
  is the same kind of violation as a stale book.
- *What does account_id=0 do?* Disables STP for that order entirely. A
  taker with id 0 will match any maker regardless of account; a maker
  with id 0 is matched by any taker. This is how the pre-existing
  tests stay green.

---

## 3. `ImmediateOrCancel` and `FillOrKill` order types

### What
Two new `OrderType` enum values.

- **IOC**: limit-priced, matches what it can, residual is dropped (not
  rested).
- **FOK**: matches all-or-nothing. The engine pre-walks the opposite
  book at the order's limit price; if the available crossing liquidity
  (accounting for STP) is less than the order's quantity, **no state
  changes happen** and zero trades are emitted.

Implementation: `crosses()` treats IOC/FOK identically to Limit
(price-bounded). The "rests residual" tail only runs for `Limit`. FOK
gets a `fok_can_fill()` pre-check at the top of `submit`.

### Why
These are the two most common "time-in-force" qualifiers after GTC
(plain Limit). Tens of percent of real-exchange flow is IOC. FOK is
rarer but is the canonical example of an order that needs a
side-effect-free pre-check — it's the cleanest test of whether you can
reason about transactional matching.

### Key decisions
- **FOK pre-check semantics under STP.** `fok_can_fill` must walk levels
  the same way real matching would — skipping same-account makers and
  stopping at the first level fully blocked. Otherwise the pre-check
  could green-light a fill that the real matcher would refuse to
  produce. See `FokRespectsSelfCrossInPreCheck`.
- **Encode time-in-force in `OrderType`, not a separate `TimeInForce`
  field.** Real exchanges separate the two (an IOC market vs IOC limit
  are different). For v1 they're conflated into one enum. Honest:
  upgrading later is mechanical.

### Probes
- *Why does FOK need a pre-walk and IOC doesn't?* IOC is a fill-then-drop:
  if you fill 3 of 10, the other 7 are dropped. State mutates and
  that's fine. FOK is all-or-nothing: if you can only fill 9 of 10, the
  9 fills must not happen. Without a pre-check, you'd have to roll back
  on failure — pre-check is simpler and avoids the rollback path.
- *What's the asymptotic cost of the FOK pre-walk?* `O(k)` where k is
  the number of makers up to and including the level that satisfies
  the order quantity. Walks the same orders the real match would, so
  it's not free — but it's bounded by the matching cost itself, not by
  book size.
- *What about Iceberg, Post-only, GTD?* Not implemented. Each is a
  localised matching-loop change. Post-only is the easiest (refuse if
  it would cross). Iceberg is the trickiest (you need a "visible
  quantity" concept and a refill rule).

---

## 4. JSON snapshot API + per-Trade JSON

### What
`OrderBook::snapshot_json(depth)` returns a compact JSON string with
the BBO, total book size, and the top-N levels per side. `to_json(const
Trade&)` returns the same for a single trade. Both are hand-rolled —
no JSON library dependency.

```json
{"bid":10000,"ask":10100,"size":2,"bids":[[10000,5,1]],"asks":[[10100,7,1]]}
```

### Why
The dashboard is the consumer, but the API is generic — anything that
wants to serialize book state for a wire format starts here. Avoiding
a JSON library keeps the build trivial and lets us tune allocations
(`reserve()` based on level count).

### Key decision
- **Why arrays-of-tuples for levels instead of objects?** `[[price,
  qty, orders], ...]` is half the bytes of `[{"price":..., "qty":...,
  "orders":...}, ...]`. WebSocket frames go over the wire many times
  per second; the size matters more than the readability.

### Probes
- *Why are you not worried about escaping?* Every field is an integer
  (`int64` or `uint64`). No strings, no special chars in the output.
  If we ever added a `symbol` field we'd need to escape.
- *Would you reach for `nlohmann::json` next?* Only if the schema got
  complex enough to warrant it. Right now the hand-rolled writer is
  faster, dependency-free, and inspectable.

---

## 5. `MatchingEngine::set_trade_listener`

### What
A `std::function<void(const Trade&)>` hook on `MatchingEngine`. After
each `on_order` populates the trade buffer, the engine iterates the
buffer and fires the callback for each trade. Setting the listener to
`{}` clears it. No listener installed = zero overhead.

### Why
The matching engine should not know about WebSockets, files, or
dashboards. It emits trades; downstream code subscribes to them. The
listener is the seam.

### Key decision
- **Why call the listener AFTER `submit` completes, not interleaved
  inside the inner matching loop?** Two reasons. (1) Keeping the
  matching loop free of indirect calls — `std::function` is an
  indirect call and can defeat inlining. (2) Atomicity — the listener
  sees the post-match state, which is more useful than a half-finished
  match.
- **Why `std::function` and not a template parameter?** A template
  would be zero-overhead but force `MatchingEngine` to be
  header-only / templated. `std::function` keeps the type uniform and
  matches the fact that the listener is a non-hot-path concern.

### Probes
- *What's the overhead of `std::function`?* One indirect call (vtable-ish
  dispatch) per trade. For the dashboard path (~20 events/sec) this is
  nothing; for a million trades/sec it's measurable.
- *Why not a vector of listeners (pub/sub)?* YAGNI right now. One
  consumer is enough. If we needed two, a `vector<TradeListener>`
  with a for-loop is a trivial change.

---

## 6. NDJSON event stream binary

### What
`apps/stream_demo.cpp` builds `tachyon_stream.exe`. It drives a
randomized order flow through `MatchingEngine` and emits one JSON
object per line on stdout: order details, trades produced, and the
post-state book snapshot. Cancels emit a separate event shape. CLI:
`tachyon_stream [n_events=200] [depth=5] [delay_ms=0]`; `n_events=0`
means run forever; `delay_ms` slows the stream so a human can watch.

### Why
The Unix philosophy seam: one program produces a stream of events on
stdout, another consumes it. Lets you pipe events to a file, a Node
script, `tail`, `jq`, or a future C++ replay tool — without the engine
knowing about any of them.

### Key decisions
- **NDJSON, not framed binary.** Easy to debug (`tail tachyon_stream`
  is a valid demo). Big tech audience expects "just newline-delimited
  JSON". Wire-efficiency loss is acceptable for a portfolio piece.
- **`std::cout.flush()` after every line.** Without it, stdout is fully
  buffered when piped to another process; the consumer would see
  events in bursts. Flushing per line is the single line of code that
  makes the dashboard's live feel work.

### Probes
- *What happens on a backed-up pipe (consumer can't drink fast
  enough)?* `cout` blocks. The engine pauses. For a demo this is fine;
  for production you'd want a bounded queue and a drop policy.
- *Why generate the flow inside this binary instead of taking it on
  stdin?* Simpler v1. A future change would feed `Order`s from stdin
  (e.g. from a recorded log), turning `tachyon_stream` into a pure
  matcher + serializer.

---

## 7. Node WebSocket bridge (`dashboard/server.js`)

### What
~100 lines. Spawns `tachyon_stream` as a child process, reads its
stdout via `readline`, broadcasts each line to all connected
WebSocket clients. Also serves `dashboard/public/index.html` on the
same HTTP port. Single dependency: `ws`.

### Why
Two options were on the table: (a) embed a WebSocket server directly in
C++ via `cpp-httplib` or similar, (b) write a tiny Node bridge. Picked
(b) because:

1. Keeps the C++ engine pure — no networking code in the matching
   binary, no risk of network code polluting the matcher's L1/L2 cache.
2. Node has the `ws` library, battle-tested. Writing a correct WS
   handshake in C++ on Windows is real work.
3. The bridge is ~100 lines vs ~300+ for an embedded WS server.

### Key decision
- **Same HTTP port for both static files and WebSocket.** Different
  protocols on the same port. The `ws` library does this by attaching
  the WS server to the same `http.Server` instance. Less to remember
  for the user — one URL, no port juggling.

### Probes
- *What's the failure mode if `tachyon_stream` crashes?* The child
  process exits; `child.on('exit')` calls `process.exit(code)`. Server
  goes down with it. Browser sees WS close, retries every 1.5s.
- *Why not stream over plain TCP?* WebSocket gives you framing for
  free, browser support, and trivial JS clients. Plain TCP works but
  needs a parser on both sides.
- *Why `readline` and not splitting buffers by hand?* `readline` deals
  with partial reads — a single `data` event can deliver half a line
  plus a chunk of the next. Doing this by hand is the kind of thing
  that works in dev and breaks at high event rate.

---

## 8. Vanilla-JS dashboard (`dashboard/public/index.html`)

### What
Single self-contained HTML file. Three panels: stats row (BBO,
spread, book size, events/sec, total trades), horizontal-bar depth
chart (bids left, asks right), scrolling trade tape (last 200 trades,
freshest at top). Auto-reconnects to the WebSocket on disconnect.

### Why
Vanilla JS instead of React/Next.js was a deliberate cut. The
dashboard is ~200 lines of HTML+JS+CSS — adding a framework would mean
a build step, `node_modules` on every machine, and an explanation in
the README about Webpack/Vite. None of that adds signal.

### Key decisions
- **Horizontal bars filled proportional to max-qty across both
  sides.** This is the standard order-book visualization. The bar
  width tells you depth at a glance.
- **No price chart.** Tempting to add, but order-book viewers and
  price charts are different views. A price chart is a 1D time series;
  this dashboard is a 2D depth view.
- **No persistence on the client.** Refresh = lose history. Acceptable
  for a live viewer; if we wanted "last 60 seconds" we'd add a ring
  buffer.

### Probes
- *Why not React?* The whole dashboard updates ~20 times per second
  from one data source. Vanilla DOM manipulation is faster than any
  diff-and-reconcile here, and React's value (component reuse, large
  team coordination) doesn't apply to a 200-line single-file UI.
- *What's the perf risk of innerHTML on every depth update?* For 8
  levels × 2 sides = 16 rows at 20 Hz, it's negligible. If we pushed
  to 100 levels × 100 Hz we'd want individual node updates.

---

## 9. Flat tick-indexed ladder (the big refactor)

### What
Replaced `std::map<Price, std::list<Order>>` per side with:

- `std::vector<Level>` per side, sized once to `span_ = tick_max -
  tick_min + 1`. Direct array index instead of tree walk.
- `std::vector<uint64_t>` bitmap per side — bit `i` is 1 iff
  `levels[i]` is non-empty.
- `int64_t best_bid_idx_`, `int64_t best_ask_idx_` — index of the
  current best on each side. `-1` (bids) / `span_` (asks) means empty.

When a level drains, we clear its bit and bit-scan backward (bids) or
forward (asks) to find the new best — `_BitScanReverse64` on MSVC,
`__builtin_clzll` on others. Bounded `O(span/64)` words but in
practice almost always one word.

### Why
`std::map` is a red-black tree. Every level access pointer-chases
through three to four heap allocations (root → child → child → leaf).
On a tight book where levels cluster around the BBO, you're walking
the same cache-cold nodes over and over. The flat ladder makes that
access an array index — one cache line per `Level`, no allocator.

The bitmap is the trick. Without it, finding "the next non-empty level
below this one" would scan the wide `Level` vector cell by cell — slow
on sparse books. With the bitmap, you scan 64 cells per word of bitmap.
For `span_ = 200000`, that's at most `200000/64 ≈ 3125` words; in
practice almost always the first word.

### Key decisions
- **Tick range bounded at construction.** Default `(0, 200'000)`
  covers `$0–$2000` for cent-tick instruments. Memory: 200k Level slots
  × ~32 bytes ≈ 6 MB per side ≈ 12 MB per book. Acceptable.
- **Memory cost of mostly-empty Levels.** Wasted? Only if the
  empty slots get pulled into cache. The bitmap ensures they
  don't — the hot path only touches cells we know are occupied.
- **Iterator stability for `id_index_`.** Vector indices into a
  vector that we never resize are stable forever, which is the
  property `std::map` iterators gave us. Free win.
- **Why not a sliding window over the BBO?** More complex. The fixed
  range works for any equity-like symbol with a known price band.
  Listed in design.md §8 as a known limitation.

### Probes
- *Why is `O(span/64)` "almost always one word" in practice?* Because
  active levels cluster around the BBO. A typical tight book has 50-500
  active levels in a narrow band; one or two uint64 words of bitmap
  cover all of them.
- *What happens if a Limit order comes in at a price outside
  `[tick_min, tick_max]`?* Rejected silently. v1 limitation — a real
  exchange would NACK back; we'd extend with a `RejectReason` enum.
- *How does the cache footprint compare to `std::map`?* `std::map`
  touches one tree node (~64 bytes) per level access plus its leaf
  data. The flat ladder touches one Level (~32 bytes) plus one bitmap
  uint64 (8 bytes). For a 10-level walk that's ~400 bytes (flat) vs
  ~800-1200 bytes (map) of distinct cache lines.
- *Why a 64-bit bitmap word and not 8 or 32?* Because the bit-scan
  intrinsics target a register width. On x86-64 that's 64. Smaller
  words would mean more iterations through the scan loop without any
  cache or latency benefit.

---

## 10. Intrusive doubly-linked list + pool allocator

### What
`Level::head` and `Level::tail` are `OrderNode*`. `OrderNode` is
`{ Order, prev, next }`. The list is intrusive — the linking pointers
live inside the node, no separate `std::list<Order>` indirection.

`OrderNode`s are drawn from a `Pool` that grows in blocks of 4096
(`std::unique_ptr<OrderNode[]>`) and maintains a free-list of unused
nodes. `acquire()` pops from the free-list head; `release()` pushes
back. Crucially, the pool **never reallocates an existing block**, so
every `OrderNode*` ever handed out stays valid for the lifetime of
the pool. `id_index_` therefore stores raw `OrderNode*` directly —
one less pointer chase than the old `LevelHandle` indirection.

### Why
`std::list<Order>` allocates per node through the default allocator.
On every `submit` that rests an order, you pay `malloc` (or `new`). On
every cancel or full fill, you pay `free`. The matching hot path
shouldn't see the allocator at all. The pool achieves that.

### Key decisions
- **Block-growing, not single-vector.** A `std::vector<OrderNode>`
  would invalidate `OrderNode*` on resize. We use a `vector<unique_ptr
  <OrderNode[]>>` of blocks — adding a new block doesn't move existing
  ones. Trade-off: each block is a separate heap allocation.
- **Doubly-linked, not singly-linked.** Single-linked makes "remove
  this specific node" `O(n)` unless you also track the previous node.
  We do `cancel` on internal nodes a lot (real markets cancel >90% of
  orders), so `O(1)` removal via `prev`/`next` is non-negotiable.
- **Block size = 4096.** One memory page on Windows. Each block is one
  page-aligned allocation; pre-warming the pool with one block of 4096
  nodes covers any book of reasonable depth.

### Probes
- *Why is the free-list LIFO (push to head, pop from head) instead of
  FIFO?* Cache temperature. The most recently `release()`'d node is
  the most likely to still be in L1/L2; popping it next on `acquire`
  reuses a warm cache line.
- *What happens when the free-list runs out?* `grow_one_block()`
  allocates a fresh block and threads all 4096 of its nodes onto the
  free-list. This is the only place in the matching path that calls
  `new`, and it amortises across 4096 orders.
- *Could you use `boost::intrusive::list`?* Yes, semantically the same.
  Avoiding the Boost dependency was a deliberate choice — interview-
  defensible code that compiles with just MSVC + the standard library
  is a feature.
- *What's the lifetime contract on `OrderNode*`?* Valid from
  `pool_.acquire()` until the matching `pool_.release()`. Storing one
  across a `release()` is a use-after-free. `id_index_` enforces this
  by erasing the id at exactly the moment the node is released.

---

## 11. The "before vs after" numbers (cite these in interviews)

### Throughput (`bench_throughput`, mixed flow)

| variant | std::map era | flat-ladder era |
|---------|--------------|-----------------|
| value-return submit | 3.27M ops/sec | 3.13M ops/sec |
| out-param submit | 4.04M ops/sec | **4.47M ops/sec** |

The value-return regression is the new default-ctor cost
(`vector<Level>(200000)` = 6 MB zeroed) bleeding into the benchmark's
timed region despite `PauseTiming`. The out-param number is the real
hot-path comparison; +11%.

### Latency (`bench_latency`, 50k prewarm, 100k measured)

| percentile | flat-ladder era |
|------------|-----------------|
| p50 | **300 ns** |
| p90 | 700 ns |
| p99 | **1.6 µs** |
| p99.9 | 11 µs (Windows scheduler) |
| max | 4.3 ms (single OS preemption) |

The original README target was p99 < 20 µs. We are 10× under.

### Honesty about the methodology
- `steady_clock::now()` on Windows has ~100 ns resolution. p50 = 300 ns
  is two ticks of the clock. The true p50 is probably ≤ 200 ns but the
  clock can't resolve it.
- Sub-microsecond latency on a Windows desktop without core pinning and
  real-time priority is impressive but not directly comparable to
  exchange-published numbers (Linux, pinned, isolated CPUs, RDTSC).
- design.md §7 says this out loud. Saying it yourself in an interview
  is a credibility multiplier.

---

## 12. What to study next (the (c) gap)

You can recite this file. That doesn't mean you can defend the code.
The "learning week" from memory was supposed to be re-deriving the
matching loop from scratch against the tests. We never actually did
that. Do it before any further code work:

1. Delete `src/order_book.cpp`.
2. Re-implement `OrderBook::submit` (out-param), `cancel`, `best_bid`,
   `best_ask`, `top_bids`, `top_asks`, `snapshot_json` against the
   header. Run `ctest`. Repeat until 43/43.
3. Re-implement `Pool` from scratch in the same way.
4. Then check yourself against the committed source. The places you
   diverged are the places to dig deeper.

The interview signal for "I wrote this" is being able to write it
again. Anything less is "I read this," which is much weaker.

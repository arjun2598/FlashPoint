# Performance Notes

> **Status: measured.** Numbers below are from Milestone 10. Read the caveats
> before quoting any of them.

## Measurement rules

These are fixed now, before any number is produced, so that results cannot be
retrofitted to a preferred conclusion.

1. **Release builds only.** Sanitizer builds are 2–20× slower and are never
   benchmarked. The `release` preset exists for this, and the root `CMakeLists.txt`
   defaults an unset build type to Release for the same reason.
2. **Report distributions, not averages.** For a matching engine the tail is the
   product. Report p50 / p99 / p99.9 / max.
3. **Separate throughput from latency.** Orders/second under saturation and
   per-operation latency under load are different questions with different
   answers.
4. **State the machine.** CPU model, core count, clock policy, OS, compiler and
   version, and build flags accompany every number.
5. **Measure before optimising.** No optimisation is committed without a
   before/after measurement in this file.
6. **Warm up, and account for allocation.** The first N operations touch cold
   caches and grow containers. Report steady-state separately from cold-start,
   and say which is which.

## Known design factors to measure later

Recorded as hypotheses, not claims. Each is to be confirmed or refuted by
measurement at Milestone 11.

- **Price level lookup — the open question, and now the only one.** Currently a
  `std::map` per side: O(log L) with a pointer chase per node. A direct-indexed
  ladder is O(1) but costs memory proportional to the tick range; a sorted vector
  is cache-friendly but O(n) to insert mid-book. Real books are dense near the
  touch and sparse in the tail, which favours a hybrid.

  Milestone 4 deliberately left this alone (DD-015) so that Milestone 10 has a
  baseline to measure against and Milestone 11 has an oracle to differential-test
  the replacement against. Worth noting what the book's structure now
  guarantees: **every remaining O(log L) term in the book comes from this one
  component.** `add`, `remove` and all three depth queries each pay exactly one
  level lookup and nothing else. That makes the size of the prize measurable
  before the work is done.
- ~~**Order storage within a level.**~~ Resolved at Milestone 4: an intrusive
  doubly-linked FIFO queue over a pooled `std::vector`, with freed slots on a
  free list. No per-order allocation in steady state, and O(1) unlink from any
  position. Nodes are addressed by 32-bit index rather than pointer, which is half the
  size and stable across pool growth.
- **Allocation on the hot path.** The target is zero allocation during steady-
  state matching. Anything else introduces an unbounded tail. The order pool now
  meets this once warmed; the price-level `std::map` still allocates a node per
  new level, which is a second reason to revisit it.
- **The common case is not a trade.** In real markets, the overwhelming majority
  of messages tend to be non-marketable adds and cancels. Optimising the crossing path
  while the add path chases pointers is the classic mistake. The engine's trade
  sink (DD-019) is shaped for this: an order producing no executions never
  invokes it and allocates nothing.
- **Repeated level lookups during a sweep.** Filling against N resting orders
  currently performs N price-level lookups rather than one per level, because the
  engine re-enters the book for each fill (DD-022). The efficient shape is a
  cursor held across a level, but that is a handle into the book's internals and
  would forfeit the swappability DD-018 exists to protect. Deliberately left
  until a profile says it matters, but should still be done as an opaque handle.
- **Top-of-book access.** Best bid/ask is read on essentially every operation.
  It should be O(1) and ideally in a hot cache line.
- **`Order` is 32 bytes, and could plausibly be 24.** `Price::Rep` and
  `Quantity::Rep` are 64-bit. Tick counts and share quantities may fit
  comfortably in 32 bits for any realistic instrument, which would take the
  payload from 25 bytes to 17 and the object from 32 to 24, improving cache density and allowing
  more orders to fit in higher cache levels. However, the choice of 64-bit is intentional
  here to allow a greater range, and latency should be measured under realistic order-book workloads
  before reducing the representation size, since smaller objects improve cache locality but may not
  materially affect end-to-end matching performance.

  Recorded here rather than acted on. The point worth noting is that because
  each type names its representation as a nested `Rep` alias, testing the
  hypothesis is a two-line change plus updating the `sizeof` assertion in
  `tests/order_test.cpp` — not a repo-wide edit. That cheapness is a deliberate
  payoff of the strong-type wrapper (DD-010) and is what makes it reasonable to
  defer the question instead of guessing now.

## Guard rails already in place

Not measurements, but decisions made at Milestone 3 that constrain what a later
measurement can find:

- **`Order` is pinned at 32 bytes** by a test, so a field addition in a later
  milestone fails loudly instead of silently doubling the book's memory traffic.
- **`Order` is asserted trivially copyable**, so no member can introduce an
  allocation on the hot path without breaking the build.
- **Validation is at the engine boundary, never in the matching loop** (DD-012).
  The one internal check, the unsigned-wraparound `assert` in
  `Quantity::operator-=`, is compiled out entirely under `NDEBUG`.
- **No floating point anywhere near the book.** Prices are integer ticks
  (DD-009), so comparison is exact and cheap.
- **Level aggregates are cached, not computed.** `quantity_at` and
  `order_count_at` never walk a queue; they read fields maintained incrementally
  on every mutation. Depth is queried far more often than it changes.
- **The book's public API is handle-based** (DD-018), asserted by test. Nothing
  outside can hold an iterator into the price-level container, which is what
  keeps replacing it a private change rather than a rewrite.
- **Trades are delivered without allocation.** The sink is a template parameter,
  so the call is direct and inlinable rather than an indirect dispatch through
  `std::function`, and no per-order container is built (DD-019, DD-023).
- **A partial fill costs no relinking.** `OrderBook::reduce` updates two integers
  and touches neither the queue links nor the level's head and tail.
- **Publishing an event allocates nothing.** The sink is a template parameter, so
  the call is direct and inlinable, and `Event` is a fixed-size trivially
  copyable record passed by reference (DD-034, DD-035).
- **Depth snapshots allocate nothing.** `OrderBook::snapshot` writes into a
  buffer the caller owns, so a publisher reuses one array across thousands of
  snapshots (DD-038). Aggregates are already cached per level, so a snapshot
  never walks an order queue.

## Environment used for benchmarking

| | |
|---|---|
| CPU | Apple M2, 8 cores (4 performance + 4 efficiency) |
| Memory | 8 GB |
| OS | macOS, Darwin 25.3.0 |
| Compiler | AppleClang 21.0.0, arm64 |
| Build | `release` preset, `-O3 -DNDEBUG`, no sanitizers |
| Load during the run | load average 3–6 (the machine was not idle) |

**Read these caveats before quoting any number.**

1. **This is a laptop, not a server.** macOS offers no straightforward way to pin
   a thread to a core or to disable frequency scaling, and the run competed with
   other work. Treat the figures as the *relative* cost of operations against
   each other, not as absolute production latencies.
2. **There is a measurement floor of about 42 ns.** `steady_clock` on this
   machine ticks at roughly 41.67 ns, so any operation faster than that reads as
   one tick. Several do. This is the reason for two harnesses: Google Benchmark
   amortises across millions of iterations and resolves below the floor, while
   the latency harness gives the distribution above it.
3. **The `max` column is the operating system, not the engine.** Values in the
   tens of microseconds are the thread being descheduled. They are reported
   rather than trimmed, because silently discarding outliers is how benchmarks
   become dishonest, but they say nothing about the code.

## Results

Two book shapes, both holding **5,000 resting orders**, differing only in how
many distinct price levels those orders occupy:

- **shallow** — 10 levels × 500 orders
- **deep** — 1,000 levels × 5 orders

Everything in the book that is not O(1) scales with the level count, so the gap
between the two columns is precisely the cost the Milestone 11 container change
is meant to remove.

### Latency distribution, nanoseconds

Nearest-rank percentiles, so every figure is a measurement that actually
occurred. Includes roughly 42 ns of instrumentation (two clock reads).

Shallow book, 10 levels:

| Operation | samples | p50 | p90 | p99 | p99.9 |
|---|---:|---:|---:|---:|---:|
| add, non-marketable | 200,000 | 42 | 84 | 125 | 1,833 |
| cancel, random resting order | 200,000 | 42 | 84 | 125 | 292 |
| submit, crosses one resting order | 200,000 | 42 | 42 | 84 | 125 |
| submit, sweeps ten resting orders | 50,000 | 417 | 459 | 542 | 750 |
| modify, priority retained | 200,000 | 42 | 42 | 42 | 83 |
| modify, priority lost | 200,000 | 125 | 125 | 167 | 292 |
| read top of book | 200,000 | 41 | 42 | 42 | 84 |
| snapshot, ten levels | 200,000 | 42 | 42 | 42 | 83 |

Deep book, 1,000 levels:

| Operation | samples | p50 | p90 | p99 | p99.9 |
|---|---:|---:|---:|---:|---:|
| add, non-marketable | 200,000 | 84 | 125 | 167 | 1,250 |
| cancel, random resting order | 200,000 | 125 | 166 | 208 | 333 |
| submit, crosses one resting order | 200,000 | 83 | 125 | 125 | 292 |
| submit, sweeps ten resting orders | 50,000 | 667 | 709 | 917 | 1,334 |
| modify, priority retained | 200,000 | 42 | 84 | 125 | 208 |
| modify, priority lost | 200,000 | 208 | 250 | 292 | 417 |
| read top of book | 200,000 | 41 | 42 | 42 | 125 |
| snapshot, ten levels | 200,000 | 83 | 84 | 84 | 167 |

### Throughput, Google Benchmark

Amortised mean per operation, which resolves below the clock floor.

| Benchmark | shallow | deep | ratio |
|---|---:|---:|---:|
| add then cancel (one pair) | 129 ns | 195 ns | 1.51× |
| submit crossing one resting order | 20.2 ns | 35.4 ns | 1.75× |
| read top of book | 4.23 ns | 7.33 ns | 1.73× |
| snapshot, ten levels | 42.4 ns | 73.8 ns | 1.74× |

Headline sustained rate: **15.5 M add-and-cancel pairs per second** on the
shallow book, **10.3 M** on the deep one.

There is deliberately no add-only throughput benchmark. Google Benchmark picks
its own iteration count, and an add-only loop pushed six million orders into a
book that started with five thousand, so it measured node-pool growth and hash
rehashing rather than the add path. Add in isolation is measured by the latency
harness, where the sample count is fixed and the pool is reserved up front.

### What the numbers say

1. **Every operation that touches a price level costs 1.5–1.75× more on the deep
   book.** The ratio is remarkably consistent across unrelated operations, which
   is what you would expect if a single shared component (the price-level
   container) is responsible. This is the prize Milestone 11 is chasing, and it
   is now sized rather than assumed.

2. **Reading top of book is 4.2 ns and genuinely O(1)**, but still 1.73× slower
   on the deep book. That is not algorithmic; `rbegin()` on a `std::map` is O(1)
   either way. It is the pointer chase into a tree node that is less likely to be
   in cache when the tree is a hundred times larger. A flat structure would fix
   this even though the complexity is already optimal.

3. **The multi-level sweep is the most expensive operation and scales worst in
   absolute terms** (417 → 667 ns, +250 ns). This is DD-022 showing up exactly
   where it was predicted: the engine re-enters the book once per fill, so ten
   fills pay ten level lookups. A cursor held across a level would collapse that
   to one per level.

4. **A retained modify is the cheapest mutation**, at or near the clock floor.
   It updates two integers and does one level lookup, with no relinking. The
   priority-lost path costs about 3× more (125 vs 42 ns shallow), which is the
   remove-and-re-add showing its price.

5. **The p99.9 on `add` is an outlier at 1,250–1,833 ns**, far above its p99 of
   125–167 ns. That is the node pool and the order index growing: 200,000 adds
   onto a 5,000-order book means occasional reallocation. It is the one place in
   the results where an unbounded tail traces to something real in the engine
   rather than to the operating system, and it argues for reserving capacity up
   front in any latency-sensitive deployment. `MatchingEngine` already accepts an
   expected order count for exactly this.

### Two benchmark bugs found and fixed during this milestone

Recorded because they are the reason to trust the final numbers, and because
both would have produced plausible-looking but wrong results.

- **`modify, priority retained` measured the wrong thing twice.** It first passed
  a fixed price rather than the order's own, which made most iterations a reprice
  (the priority-*lost* path) and migrated the whole book onto one level part
  way through the run. Corrected to use the order's price, it then passed the
  *same* quantity back, which skips the reduce entirely and measured a lookup and
  an event. It now genuinely reduces the quantity.
- **`bm_add_non_marketable` measured pool growth.** Described above; removed.

Neither bug would have failed anything. They were caught by noticing that a
figure did not move when the book shape changed, which it should have.

## What Milestone 11 should target

In priority order, based on the above rather than on intuition:

1. **Replace the price-level container.** Worth roughly 1.5–1.75× on nearly
   every operation, and the effect is measured, not assumed. The differential
   test against the current implementation is the safety net (DD-015).
2. **Give the sweep a level cursor** (DD-022). Worth roughly 250 ns on a
   ten-order sweep against a deep book. Smaller and narrower than the above, and
   only worth the API cost if the container change does not already absorb it.
3. **Nothing else yet.** `Order` shrinking to 24 bytes and other hypotheses in
   this document have no measurement behind them, and the two items above
   dominate everything measured so far.

# Performance Notes

> **Status: measured and tuned.** Baseline numbers from Milestone 10, current
> numbers after the Milestone 11 tuning pass. Read the caveats before quoting
> any of them.

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

  Milestone 4 left this alone (DD-015) so that Milestone 10 has a baseline to
  measure against and Milestone 11 has an oracle to differential-test the
  replacement against. **Every remaining O(log L) term in the book comes from
  this one component:** `add`, `remove` and all three depth queries each pay one
  level lookup and nothing else, so the available gain can be sized before the
  work starts.
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
  payload from 25 bytes to 17 and the object from 32 to 24. The 64-bit choice is
  intentional, for range. Smaller objects improve cache density, but whether that
  shows up in end-to-end matching has to be measured under a realistic workload
  before the representation shrinks.

  Recorded rather than acted on. Because each type names its representation as a
  nested `Rep` alias, testing the hypothesis is a two-line change plus the
  `sizeof` assertion in `tests/order_test.cpp`, not a repo-wide edit (DD-010).

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
   rather than trimmed, but they say nothing about the code.

## Results

Two book shapes, both holding **5,000 resting orders**, differing only in how
many distinct price levels those orders occupy:

- **shallow** — 10 levels × 500 orders
- **deep** — 1,000 levels × 5 orders

Everything in the book that is not O(1) scales with the level count, so the gap
between the two columns is the cost the Milestone 11 container change is meant
to remove.

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

Headline sustained rate: **7.8 M add-and-cancel pairs per second** on the
shallow book, **5.1 M** on the deep one. Each pair is two operations, so the
same measurement is 15.5 M and 10.3 M operations per second.

There is deliberately no add-only throughput benchmark. Google Benchmark picks
its own iteration count, and an add-only loop pushed six million orders into a
book that started with five thousand, so it measured node-pool growth and hash
rehashing rather than the add path. Add in isolation is measured by the latency
harness, where the sample count is fixed and the pool is reserved up front.

### What the numbers say

1. **Every operation that touches a price level costs 1.5–1.75× more on the deep
   book.** The ratio holds across unrelated operations, which points at a single
   shared component: the price-level container. That sizes what Milestone 11 has
   to gain.

2. **Reading top of book is 4.2 ns and O(1)**, but still 1.73× slower
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

Both would have produced plausible-looking but wrong results.

- **`modify, priority retained` measured the wrong thing twice.** It first passed
  a fixed price rather than the order's own, which made most iterations a reprice
  (the priority-*lost* path) and migrated the whole book onto one level part
  way through the run. Corrected to use the order's price, it then passed the
  *same* quantity back, which skips the reduce entirely and measured a lookup and
  an event. It now reduces the quantity.
- **`bm_add_non_marketable` measured pool growth.** Described above; removed.

Neither bug would have failed anything. They were caught by noticing that a
figure did not move when the book shape changed, which it should have.

## Milestone 11: the tuning pass

Two changes were attempted. One was reverted, one shipped.

### Attempt 1: a pooled allocator for the level maps. Reverted.

The Milestone 10 data said the problem was cache, not complexity: top of book was
1.73× slower on the deep book despite already being O(1). Giving the maps their
nodes from a contiguous arena is the cheapest change that attacks that.

It changed nothing. Every benchmark stayed within noise: `add_then_cancel`
195 → 198 ns, `top_of_book` 7.33 → 7.37 ns, `snapshot` 73.8 → 73.9 ns.

The reason is a property of the benchmark, not the engine. Each book is built in
one burst, so a general-purpose allocator servicing a run of same-sized requests
already returned near-contiguous nodes. There was no scattering to fix. The
fragmentation a pool prevents happens in a book that churns levels for hours,
which nothing here simulates.

Reverted under rule 5. Keeping it would have meant shipping a change justified
only by a story about a workload we do not measure, and it cost API surface:
`OrderBook` had to become move-only. See DD-041.

**Settled at Milestone 12.** The demo's synthetic feed provides the workload the
benchmarks lacked: two million orders around a drifting mid, with 30–160 levels
per side created and destroyed throughout. Throughput is flat across the whole
run (176 ns/op at 100k orders, 176 ns/op at 2M), so no fragmentation develops
even under sustained level churn. The revert stands on evidence rather than on
the absence of a benchmark (DD-045).

### Attempt 2: bids stored descending. Shipped.

Ruling out node scattering left the tree itself. Splitting `top_of_book` into its
two halves found it at once:

| | 10 levels | 1,000 levels |
|---|---:|---:|
| `best_ask()` — `begin()` | 1.54 ns | 1.55 ns |
| `best_bid()` — `rbegin()` | 4.14 ns | 7.32 ns |

`std::map::begin()` is O(1) because the leftmost node is cached. Nothing caches
the rightmost, so `rbegin()` walks the tree. **`best_bid()` was O(log L)** while
`order_book.hpp` documented it as O(1).

Fixed by ordering the bid side with `std::greater<Price>`, so both sides put the
best price at `begin()`.

### Before and after

Google Benchmark, amortised mean per operation.

| Benchmark | shallow before | shallow after | deep before | deep after |
|---|---:|---:|---:|---:|
| `best_bid` | 4.14 ns | **1.56 ns** | 7.32 ns | **1.55 ns** |
| `best_ask` | 1.54 ns | 1.55 ns | 1.55 ns | 1.55 ns |
| `top_of_book` | 4.23 ns | **3.11 ns** | 7.33 ns | **3.11 ns** |
| `snapshot`, ten levels | 42.4 ns | **33.9 ns** | 73.8 ns | **30.0 ns** |
| `add_then_cancel` | 129 ns | 131 ns | 195 ns | 204 ns |
| `cross_one_level` | 20.2 ns | 19.7 ns | 35.4 ns | 35.8 ns |

Latency p50, deep book: `snapshot` fell from 83 ns to 42 ns, and its p99.9 from
250 ns to 84 ns.

**The three read paths are now flat across book depth.** They were 1.7× worse on
the deep book; they are now within noise of each other. The mutation paths are
unchanged, since they never read the touch.

Both sides now order best-first, so every walk over levels runs forward from
`begin()`. `snapshot` and `quantity_available` lost their reverse-iteration
special cases and got shorter.

### What is left, and what would fix it

The remaining deep-book penalty is on mutation, not reads:

| Benchmark | deep / shallow |
|---|---:|
| `add_then_cancel` | 1.56× |
| `cross_one_level` | 1.82× |

Both are dominated by `map::find` on the level lookup path, which is roughly four node
visits on ten levels against ten visits on a thousand. That is tree depth, and
neither an allocator nor a comparator touches it. Only a flatter structure does.

**Parked, with the diagnosis recorded**, so a future attempt starts from evidence:

1. **A direct-indexed ladder** over a configured tick window, with a map fallback
   outside it. O(1) lookup and O(1) level creation. The largest change
   in the project, and it reopens DD-009's unbounded signed price range.
2. **A level cursor for sweeps** (DD-022), worth roughly 250 ns on a ten-order
   deep-book sweep. Only worth its API cost if the container change does not
   already absorb it.

A sorted vector was considered and **eliminated by measurement**. It looked
excellent on everything Milestone 10 measured, so Milestone 11 first added the
scenario Milestone 10 lacked: creating and destroying price levels. Level
creation costs 83 ns and does not scale with depth at all, because a map insert
is dominated by allocation rather than by tree size. A sorted vector would have
to shift every element past the insertion point — an estimated 200–500 ns at a
thousand levels. It would have fixed a 1.7× read problem by creating a 3–6×
insertion problem, in exactly the book we are trying to improve.

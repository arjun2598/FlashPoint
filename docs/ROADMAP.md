# Roadmap

Milestones are completed in order, each ending in a
single logical commit. This file is updated as part of the milestone it closes.

| #   | Milestone                                                           | Status                         |
| --- | ------------------------------------------------------------------- | ------------------------------ |
| 1   | Project setup & build system                                        | ✔ Complete                     |
| 2   | Unit test harness & CI                                              | ✔ Complete (delivered with M1) |
| 3   | Core domain types (`Price`, `Quantity`, `OrderId`, `Side`, `Order`) | ✔ Complete                     |
| 4   | Limit order book (price levels, FIFO priority)                      | ✔ Complete                     |
| 5   | Matching engine — limit orders                                      | ✔ Complete                     |
| 6   | Market orders & time-in-force (IOC / FOK)                           | ✔ Complete                     |
| 7   | Cancel orders                                                       | ✔ Complete                     |
| 8   | Modify / cancel-replace                                             | ✔ Complete                     |
| 9   | Event stream & market data (trades, L2 snapshot, top-of-book)       | ✔ Complete                     |
| 10  | Benchmarks (throughput & latency percentiles)                       | ✔ Complete                     |
| 11  | Measured performance tuning pass                                    | ✔ Complete                     |
| 12  | Demo application (order feed replay)                                | ✔ Complete                     |
| 13  | Documentation polish & architecture diagrams                        | ⬜ Not started                 |

## Milestone 1 — Project setup & build system ✔

**Objective:** a repository that builds and tests in one command, with the
quality gates in place _before_ any domain code exists, so that every later
milestone lands into a rig that already enforces correctness.

Delivered:

- CMake 3.25+ build: a `flashpoint` static library plus a thin test executable.
- `CMakePresets.json` — `dev` (Debug + ASan/UBSan), `release`, `relwithdebinfo`.
- Warning policy as an INTERFACE target, `-Werror` on by default, including
  `-Wconversion` / `-Wsign-conversion`.
- Opt-in AddressSanitizer + UndefinedBehaviorSanitizer with
  `-fno-sanitize-recover=all`.
- GoogleTest v1.17.0 via `FetchContent`, pinned, registered per-test with CTest.
- Generated `version.hpp` from a single source of truth (the `project()` version).
- GitHub Actions: 5-way matrix (Linux GCC/Clang, macOS AppleClang; Debug with
  sanitizers and Release) plus a `clang-format` gate.
- `.clang-format`, `.clang-tidy`, `.gitignore`.
- Documentation skeleton: README, architecture, design decisions, performance
  notes, developer setup.

**Verification performed:**

- `dev` and `release` presets both configure, build and pass 3/3 tests locally
  (AppleClang 21, arm64).
- Negative controls: `-Werror` rejects an implicit narrowing conversion as an
  error; ASan aborts on a heap-buffer-overflow. The gates are not merely present,
  they demonstrably fail bad code.

## Milestone 3 — Core domain types ✔

**Objective:** define the value vocabulary the whole engine speaks, before any
container or algorithm depends on it. These types are the hardest thing in the
project to change later.

Delivered:

- `include/flashpoint/types.hpp` — `Side`, `Price`, `Quantity`, `OrderId`.
- `include/flashpoint/order.hpp` — `Order`, the immutable inbound request.
- `include/flashpoint/ostream.hpp` — stream inserters, kept out of the library
  so `<ostream>` never reaches the hot path.
- 30 tests across `types_test.cpp` and `order_test.cpp`, plus 26 `static_assert`s
  covering the properties a runtime test cannot express.

Decisions recorded as DD-009 through DD-013: prices as integer ticks,
hand-written strong types, `Order` as request-only, boundary validation, and
separated stream inserters.

**Verification performed:**

- 33/33 tests pass under ASan/UBSan; 32/32 in Release, where the death test
  correctly compiles out under `NDEBUG`.
- Negative controls, compiled against the real headers: transposing price and
  quantity, implicitly converting an `int` to a `Price`, building a `Quantity`
  from a `Price`, and adding two `Price`s are each **rejected by the compiler**.
  The type safety is demonstrated, not asserted.
- `sizeof(Order) == 32` and `std::is_trivially_copyable_v<Order>` are pinned by
  tests, so a later field addition fails loudly rather than quietly doubling the
  book's memory traffic.

## Milestone 4 — Limit order book ✔

**Objective:** a container holding resting orders in correct price-time
priority, with top-of-book and depth queries. No matching.

Delivered:

- `include/flashpoint/order_book.hpp`, `src/order_book.cpp` — `OrderBook` with
  `add`, `remove`, `best_bid`, `best_ask`, `quantity_at`, `order_count_at`,
  `front_at`, `contains`, `size`, `empty`.
- Price levels in a `std::map` per side (deferred, DD-015); orders within a level
  in an intrusive FIFO queue over a pooled vector with a free list (DD-016).
- `std::hash<OrderId>`, added now that the order index needs it.
- 23 tests, the last of which is a differential test running 4,000 mixed
  operations against a naive reference model, plus 10 static assertions pinning
  the public API surface (DD-018).

**Verification performed:**

- 56/56 tests pass under ASan/UBSan.
- **Mutation tested.** Four real bugs were injected into `order_book.cpp` and the
  suite caught every one: `best_bid` reading `begin()` instead of `rbegin()`
  (4 failures), the queue built LIFO instead of FIFO (8), empty levels never
  erased (3), and `unlink` skipping the `next->prev` fixup (3).

## Milestone 5 — Matching engine, limit orders ✔

**Objective:** cross an incoming limit order against the book at price-time
priority, emit trades, rest the remainder.

Delivered:

- `include/flashpoint/trade.hpp` — `Trade`, one execution.
- `include/flashpoint/matching_engine.hpp` — `MatchingEngine`, header-only
  because `submit` is templated on the trade sink (DD-023).
- `SubmitStatus` / `SubmitResult`, and the structural-validity check at the
  engine boundary that DD-012 promised at Milestone 3.
- `OrderBook::remaining_of()` and `OrderBook::reduce()` — the two primitives
  matching needs. `reduce` preserves queue position.
- 20 tests, the last asserting two invariants after every submission across
  3,000 randomised orders.

Decisions recorded as DD-019 through DD-023.

**Verification performed:**

- 76/76 tests pass under ASan/UBSan.
- **Mutation tested.** Five injected bugs, all caught: trades printing at the
  aggressor's limit instead of the maker's price (5 failures), equality no
  longer crossing (11), buys aggressing against bids instead of asks (14), maker
  and taker transposed (5), and a partial fill wiping the maker entirely (3).
- The randomised test checks conservation: `filled + resting` always equals the
  submitted quantity, recorded trades account for exactly `filled`, and
  the engine never leaves a crossed book.

## Milestone 6 — Market orders and time-in-force ✔

**Objective:** support market orders with venue protection, and the three
time-in-force rules.

Delivered:

- `OrderType` and `TimeInForce` in `types.hpp`, and both as fields on `Order`.
  They fit in the existing padding, so `sizeof(Order)` is still 32.
- `Order::limit()` and `Order::market()` factories.
- `EngineConfig::market_protection_ticks`, and the effective-limit resolution
  that gives market and limit orders one shared matching loop.
- `OrderBook::quantity_available()`, which fill-or-kill uses to decide before
  emitting anything.
- `SubmitResult::cancelled`, making `filled + resting + cancelled` equal the
  submitted quantity.
- 40 new tests, mostly in `tests/time_in_force_test.cpp`.

Decisions recorded as DD-024 through DD-027.

**Verification performed:**

- 116/116 tests pass under ASan/UBSan.
- **Mutation tested.** Five injected bugs, all caught: the protection band
  subtracted instead of added for buys (5 failures), IOC and FOK limit orders
  resting instead of cancelling (3), the fill-or-kill feasibility comparison off
  by one (4), availability skipping the level exactly at the limit (4), and the
  saturation guard disabled so protection overflows (1, caught by UBSan in the
  price-range test).
- The randomised test mixes limit and market orders across all three
  time-in-force values and checks after every submission that the quantities add
  up, that nothing rests which should not have, that fill-or-kill is all or
  nothing, and that the book is never left crossed.

## Milestone 7 — Cancel orders ✔

**Objective:** the engine-level cancel protocol. The book's removal path already
existed from Milestone 4, so this milestone is the validation, rejection
reasons, and reporting around it.

Delivered:

- `MatchingEngine::cancel(OrderId)` returning `CancelResult{status, cancelled}`.
- `CancelStatus` with `Cancelled`, `UnknownOrder`, and `RejectedInvalidId`.
- 14 tests in `tests/cancel_test.cpp`, including a randomised submit-and-cancel
  run.

Decisions recorded as DD-028 and DD-029.

**Verification performed:**

- 130/130 tests pass under ASan/UBSan.
- **Mutation tested.** Three injected bugs, all caught: cancel reporting zero
  instead of the remaining quantity (3 failures), cancel reporting success
  without removing the order (10), and cancel skipping the invalid-id check (1).
- The randomised test runs 3,000 mixed submits and cancels, checking that every
  successful cancel reported exactly what the book held immediately beforehand,
  that a cancel for an order which filled in the meantime reports
  `UnknownOrder`, and that the book is never left crossed.

## Milestone 8 — Modify ✔

**Objective:** change a resting order's price or quantity, applying the venue
rule for when that costs the order its place in line.

Delivered:

- `MatchingEngine::modify(id, price, quantity, sink)` returning
  `ModifyResult{status, filled, resting, priority}`.
- `QueuePriority` and `ModifyStatus`.
- `OrderBook::resting_order(OrderId)`, returning a `RestingOrder` snapshot by
  value.
- 19 tests in `tests/modify_test.cpp`, including a randomised run that checks the
  priority rule on every applied modify.

Decisions recorded as DD-030 through DD-033.

**Verification performed:**

- 148/148 tests pass under ASan/UBSan.
- **Mutation tested.** Four injected bugs, all caught: an unchanged quantity
  wrongly losing priority (2 failures), a price change not being detected (7),
  the retained path reporting Lost (5), and `reduce` called with the wrong
  amount (1).
- Every priority test is followed by an order that trades against the level.
  Aggregate depth is identical whether priority was kept or lost, so checking
  who fills next is the only way to see the difference.
- The randomised run mixes submits, modifies and cancels, and independently
  recomputes the expected priority for every applied modify. It also asserts both
  branches of the rule were exercised at least fifty times each.

## Milestone 9 — Event stream and market data ✔

**Objective:** publish an ordered, numbered stream of everything the engine does,
and add the two aggregated views a market data feed needs.

Delivered:

- `include/flashpoint/event.hpp` — `Event`, `EventType`, `RejectReason`.
- `include/flashpoint/market_data.hpp` — `TopOfBook`, `LevelSnapshot`.
- `SequenceNumber` in `types.hpp`, the numbering DD-020 parked at Milestone 6.
- `OrderBook::top_of_book()` and `OrderBook::snapshot(Side, span)`.
- The trade sink replaced by an event sink across `submit`, `modify` and
  `cancel`. The matching core split into a private `apply()` so a modify
  publishes `Modified` rather than a second `Accepted`.
- `to_trade(Event)` for consumers that only want the tape.
- `tests/test_support.hpp` with a shared `EventRecorder`.
- 38 new tests across `event_test.cpp` and `market_data_test.cpp`.

Decisions recorded as DD-034 through DD-038.

**Verification performed:**

- 186/186 tests pass under ASan/UBSan.
- **Mutation tested.** Five injected bugs, all caught: sequence numbers never
  advancing (3 failures), a modify publishing `Accepted` instead of `Modified`
  (2), a trade event omitting the counterparty (9), the snapshot listing bids
  lowest first (2), and the snapshot writing one row past the caller's buffer
  (1, caught by ASan).
- The event stream and the returned summaries are cross-checked: trade events
  must account for exactly the reported `filled`, and cancelled events for
  exactly `cancelled`.

## Milestone 10 — Benchmarks ✔

**Objective:** produce the first real numbers, following the methodology
`docs/PERFORMANCE.md` has carried since Milestone 1.

Delivered:

- `benchmarks/support.hpp` — workload construction, latency recorder,
  nearest-rank percentiles, instrumentation-cost measurement.
- `benchmarks/latency_bench.cpp` — eight scenarios at two book shapes, reporting
  p50 / p90 / p99 / p99.9 / max.
- `benchmarks/throughput_bench.cpp` — Google Benchmark v1.9.5, pinned.
- `FLASHPOINT_BUILD_BENCHMARKS`, off by default and on in the release presets.
- Measured results and their caveats written into `docs/PERFORMANCE.md`.

Decisions recorded as DD-039 and DD-040.

**Headline findings:**

- Every operation that touches a price level costs **1.5–1.75× more** on a book
  with 100× the levels, holding the same number of orders. The ratio is
  consistent across unrelated operations, which is what a single shared
  bottleneck looks like.
- The multi-level sweep is the most expensive operation and scales worst in
  absolute terms (417 → 667 ns), which is DD-022 appearing exactly where it was
  predicted.
- Reading top of book is 4.23 ns and already O(1), yet still 1.73× slower on the
  deep book. That is cache behaviour, not complexity.

**Verification performed:**

- Both harnesses build clean under `-Werror` and run to completion.
- **Two bugs in the benchmarks themselves were found and fixed**, both recorded
  in `docs/PERFORMANCE.md`: `modify, priority retained` was measuring the
  priority-*lost* path and then a no-op, and an add-only throughput benchmark was
  measuring node-pool growth rather than the add path. Neither would have failed
  anything. Both were caught by noticing a figure that did not move when the book
  shape changed, which it should have.

## Milestone 11 — Measured tuning pass ✔

**Objective:** close the 1.5–1.75× deep-book penalty Milestone 10 measured, using
the measurements rather than intuition to choose what to change.

**What happened, in order:**

1. **Added the measurement Milestone 10 lacked.** No existing scenario created a
   price level; every add reused an existing price. Two scenarios were added.
   Level creation costs 83 ns and does not scale with depth.
2. **That eliminated the sorted vector**, which had been the front-runner. It
   would have had to shift every element past an insertion, which is an estimated
   200–500 ns at a thousand levels. This turns a 1.7× read problem into a 3–6×
   insertion problem.
3. **Built a pooled allocator for the level maps. Measured no improvement at all.
   Reverted** (DD-041). Each book is built in one burst, so the system allocator
   already returned contiguous nodes; there was no scattering to fix.
4. **That negative result forced the search onward**, and found the real cause:
   `best_bid()` used `rbegin()`, which walks the tree because `std::map` caches
   only its leftmost node. It was O(log L) while documented as O(1) (DD-042).
5. **Fixed by ordering bids descending**, so both sides put the best price at
   `begin()`.

**Measured result:**

| Benchmark | deep before | deep after | change |
|---|---:|---:|---:|
| `best_bid` | 7.32 ns | 1.55 ns | **4.7× faster** |
| `top_of_book` | 7.33 ns | 3.11 ns | **2.36× faster** |
| `snapshot`, ten levels | 73.8 ns | 30.0 ns | **2.46× faster** |
| `add_then_cancel` | 195 ns | 204 ns | unchanged |
| `cross_one_level` | 35.4 ns | 35.8 ns | unchanged |

All three read paths are now flat across book depth; they were 1.7× worse deep.
The mutation paths are unchanged, since they never read the touch.

**Verification performed:**

- 186/186 tests pass under ASan/UBSan, 185/185 in Release, with no test changes.
  The book's behaviour is identical; only the internal ordering moved.
- Both sides now order best-first, so `snapshot` and `quantity_available` lost
  their reverse-iteration special cases and got shorter.
- The remaining deep-book penalty is `map::find` tree depth, diagnosed and parked
  with the evidence in `docs/PERFORMANCE.md`.

## Milestone 12 — Demo application ✔

**Objective:** something a reviewer can clone and run in ten seconds that makes
the engine visible.

Delivered:

- `demo/` — a command interpreter reading any `std::istream`, so script replay
  and the interactive prompt are one piece of code (DD-043).
- `demo/scenarios/tour.txt` — a ten-part narrated scenario that builds a book,
  sweeps it, cancels, modifies both ways, and exercises IOC, FOK and a market
  order. Embedded in the binary at configure time so it runs from any directory.
- An ASCII depth ladder and an event log, printed from the engine's real event
  stream rather than a summary invented by the demo.
- `--generate N` — synthetic flow at volume, reporting throughput per chunk.
- `OrderBook::level_count(Side)`, the L that everything non-constant scales with.

Decisions recorded as DD-043 through DD-045.

**Verification performed:**

- 190/190 tests pass under ASan/UBSan.
- The tour runs end to end and its output demonstrates each behaviour: order
  #101 fills before #104 at the same price, a buy limited at 10260 trades at
  10253, a shrinking modify keeps queue position while a growing one loses it, a
  fill-or-kill that cannot fill produces no trade at all.
- **A hang was found and fixed on the first run.** The demo guessed at reading
  standard input from `isatty()`, which blocks forever when standard input is an
  open pipe with nothing on it: CI, `make`, most process runners. Reading stdin
  now requires an explicit `-` (DD-044).
- **The generator settled DD-041.** Two million orders with levels churning
  throughout show flat throughput (176 ns/op at both 100k and 2M), so the
  reverted pooled allocator would not have helped a long-running book either.

## Parked decisions

Recorded here so they are not silently defaulted:

- ~~**Price representation**~~: Resolved at Milestone 3 — signed integer ticks
  (DD-009).
- ~~**Single-symbol vs multi-symbol engine**~~: Resolved at Milestone 4 — the book
  is symbol-unaware and V1 trades one instrument (DD-014).
- ~~**Order storage / lifetime model**~~: Resolved at Milestone 4 — intrusive FIFO
  queue over a pooled vector with a free list (DD-016).
- ~~**Resting-order representation**~~: Resolved at Milestone 4 — a 24-byte node
  holding only id, remaining quantity and links; side and price live in the
  order index (DD-017).
- ~~**`std::hash<OrderId>`**~~: Added at Milestone 4, now that the order index
  needs it.
- **Price-level container**: Deferred to later. `std::map` as of now; a ladder or hybrid is
  the likely replacement. The reasoning, including the case against deferring, is DD-015.
  The deferral is safe only because DD-018 keeps the public API handle-based, so
  no caller can depend on the container, and a test enforces that.
- **Original quantity on a resting order**: To be decided at Milestone 9. Events
  may need it; if so it is one extra field on `Node` (DD-017).
- **Level cursor for sweeps**: Still open after Milestone 11. Worth roughly
  250 ns on a ten-order deep-book sweep (DD-022). Only worth its API cost if a
  flatter price-level container does not already absorb it.
- **Flatter price-level container**: Still open after Milestone 11, now with a
  diagnosis. The remaining deep-book penalty is `map::find` tree depth on the
  mutation paths, which no allocator or comparator change touches. A
  direct-indexed ladder over a configured tick window would fix it, at the cost
  of the largest piece of new code in the project and reopening DD-009.
- **Self-trade prevention**: Out of scope for V1. Real venues match on a
  participant id and suppress self-crosses; adding it means putting a client id
  on `Order`, which the demo would not exercise.
- **Owner checks on cancel**: Out of scope for V1, and blocked on the same
  missing participant id. Any caller can currently cancel any order.
- **Mass cancel**: Out of scope for V1. Without a participant id there is nobody
  to scope it to, so it would mean cancelling every resting order in the book,
  which is a venue-halt operation rather than a client one.
- **Richer cancel rejection reasons**: Would need an order-history subsystem
  tracking terminated ids (DD-029). Belongs in front of the engine, not in it.
- **FIX-style modify semantics**: Two known divergences, both deliberate.
  `new_quantity` is the new remaining rather than a new total (DD-031), and a
  modify keeps the order's id rather than minting a replacement (DD-033).
  Closing the first means adding original quantity to `Node`; the second means
  the same order-history mapping as above.
- ~~**Trade sequence numbers**~~: Resolved at Milestone 9 — the engine numbers
  every event from 1 (DD-036).

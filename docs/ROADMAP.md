# Roadmap

Milestones are completed in order, each ending in a
single logical commit. This file is updated as part of the milestone it closes.

| #   | Milestone                                                           | Status                         |
| --- | ------------------------------------------------------------------- | ------------------------------ |
| 1   | Project setup & build system                                        | ✔ Complete                     |
| 2   | Unit test harness & CI                                              | ✔ Complete (delivered with M1) |
| 3   | Core domain types (`Price`, `Quantity`, `OrderId`, `Side`, `Order`) | ✔ Complete                     |
| 4   | Limit order book (price levels, FIFO priority)                      | ✔ Complete                     |
| 5   | Matching engine — limit orders                                      | ⬜ Not started                 |
| 6   | Market orders & time-in-force (IOC / FOK)                           | ⬜ Not started                 |
| 7   | Cancel orders                                                       | ⬜ Not started                 |
| 8   | Modify / cancel-replace                                             | ⬜ Not started                 |
| 9   | Event stream & market data (trades, L2 snapshot, top-of-book)       | ⬜ Not started                 |
| 10  | Benchmarks (throughput & latency percentiles)                       | ⬜ Not started                 |
| 11  | Measured performance tuning pass                                    | ⬜ Not started                 |
| 12  | Demo application (order feed replay)                                | ⬜ Not started                 |
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

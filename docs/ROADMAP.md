# Roadmap

Milestones are completed in order, each ending in a
single logical commit. This file is updated as part of the milestone it closes.

| #   | Milestone                                                           | Status                         |
| --- | ------------------------------------------------------------------- | ------------------------------ |
| 1   | Project setup & build system                                        | ✔ Complete                     |
| 2   | Unit test harness & CI                                              | ✔ Complete (delivered with M1) |
| 3   | Core domain types (`Price`, `Quantity`, `OrderId`, `Side`, `Order`) | ✔ Complete                     |
| 4   | Limit order book (price levels, FIFO priority)                      | ⬜ Not started                 |
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

## Parked decisions

Recorded here so they are not silently defaulted:

- **Single-symbol vs multi-symbol engine**: To be decided at Milestone 4. Determines
  whether `MatchingEngine` owns one book or a map of books, and whether
  threading ever enters the design.
- ~~**Price representation**~~: Resolved at Milestone 3 — signed integer ticks,
  see DD-009.
- **Order storage / lifetime model**: To be decided at Milestone 4. Intrusive lists
  over a slab allocator vs `std::deque` per price level.
- **Resting-order representation**: To be decided at Milestone 4. `Order` is the
  inbound request only (DD-011); what the book stores is a separate type shaped
  by the book's own needs.
- **`std::hash<OrderId>`**: To be added at Milestone 4, alongside the book's
  order-lookup map. Deliberately omitted now rather than pulling `<functional>`
  into every translation unit before anything needs it.

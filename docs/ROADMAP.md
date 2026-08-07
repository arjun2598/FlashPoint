# Roadmap

Milestones are completed in order, each ending in a
single logical commit. This file is updated as part of the milestone it closes.

| #   | Milestone                                                           | Status                         |
| --- | ------------------------------------------------------------------- | ------------------------------ |
| 1   | Project setup & build system                                        | ✔ Complete                     |
| 2   | Unit test harness & CI                                              | ✔ Complete (delivered with M1) |
| 3   | Core domain types (`Price`, `Quantity`, `OrderId`, `Side`, `Order`) | ⬜ Not started                 |
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

## Parked decisions

Recorded here so they are not silently defaulted:

- **Single-symbol vs multi-symbol engine**: To be decided at Milestone 4. Determines
  whether `MatchingEngine` owns one book or a map of books, and whether
  threading ever enters the design.
- **Price representation**: To be decided at Milestone 3. Fixed-point integer ticks
  are near-certain, but the tick-size and scaling policy is a real choice.
- **Order storage / lifetime model**: To be decided at Milestone 4. Intrusive lists
  over a slab allocator vs `std::deque` per price level.

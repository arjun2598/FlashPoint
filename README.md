# FlashPoint

[![CI](https://github.com/arjun2598/FlashPoint/actions/workflows/ci.yml/badge.svg)](https://github.com/arjun2598/FlashPoint/actions/workflows/ci.yml)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

A low-latency limit order book and matching engine in modern C++, built to the
standards of production exchange infrastructure: price-time priority matching,
a measured performance story, and a test suite that runs under sanitizers on
every commit.

> **Status: in development.** Milestone 11 of 13 complete — the build system, CI,
> the core domain types, the order book, matching for limit and market orders,
> cancel, modify, the event stream, benchmarks and a measured tuning pass are
> in place. A replay demo is next.
> See [`docs/ROADMAP.md`](docs/ROADMAP.md). This README describes what exists right
> now, not what is planned.

## What exists currently

- **Benchmarks with real numbers, and a tuning pass driven by them.** Throughput
  via Google Benchmark, latency percentiles via a purpose-built harness, at two
  book shapes. 15.7 M add-and-cancel pairs per second; 3.1 ns to read top of
  book, flat across book depth. The tuning pass found `best_bid()` was O(log L)
  while documented as O(1), and fixing it made the read paths 2.4–4.7× faster on
  a deep book. A pooled allocator was also tried, measured no improvement, and
  reverted. Both results are written up in
  [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md).
- **Event stream and market data.** Every operation publishes an ordered,
  sequence-numbered stream of what it did: acknowledgements, executions,
  rejections, cancellations and amendments. Events are one flat trivially
  copyable record, so a stream can be written and read back with no encoding
  step. Plus top-of-book and an L2 depth snapshot that fills a caller-supplied
  buffer without allocating.
- **Cancel and modify.** Cancel reports the quantity that was still resting,
  which is what the client actually pulled. Modify follows the venue rule for
  queue priority: shrinking an order keeps its place in line, growing it or
  repricing it sends it to the back, and a reprice across the spread trades.
- **Market orders and time-in-force.** Market orders trade up to a
  venue-configured protection price derived from the opposite touch, so one
  cannot sweep a thin book to an arbitrary price. GoodTillCancel rests,
  ImmediateOrCancel cancels its remainder, and FillOrKill checks it can fill
  before emitting any trade.
- **Matching engine** for limit orders at price-time priority. Sweeps multiple
  price levels, executes at the resting order's price so improvement accrues to
  the aggressor, preserves queue position across partial fills, and rests the
  remainder. Trades are handed to a caller-supplied sink, so neither a
  non-marketable order nor a sweep allocates.
- **Limit order book** with price-time priority. Price levels in an ordered map
  per side (O(1) top-of-book); orders within a level in an intrusive FIFO queue
  over a pooled vector, so resting an order allocates nothing once warmed and
  cancelling one is O(1) from any queue position.
- **Core domain types** — `Side`, `Price`, `Quantity`, `OrderId` and `Order`.
  Strong types, not typedefs: passing a quantity where a price belongs is a
  compile error, and `Order` is pinned at 32 bytes so two share a cache line.
- CMake build producing a `flashpoint` static library and a GoogleTest suite,
  186 tests running under ASan/UBSan, including differential and randomised
  invariant tests for both the book and the engine.
- Three build presets: Debug with ASan/UBSan, Release, and RelWithDebInfo.
- `-Werror` with an aggressive warning set including `-Wconversion`.
- CI across Linux/GCC, Linux/Clang and macOS/AppleClang, in both sanitized
  Debug and Release, plus a version-pinned `clang-format` gate.

## Planned

A replay demo, then documentation polish.

## Quick start

Requires CMake 3.25+ and a C++20 compiler. Nothing else — GoogleTest is fetched
automatically on the first configure.

```bash
git clone https://github.com/arjun2598/FlashPoint.git
```

```bash
cd FlashPoint && cmake --preset dev && cmake --build --preset dev --parallel && ctest --preset dev
```

That is the sanitized Debug build. For an optimised build, substitute the
`release` preset. Full details in [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md).

## Documentation

| Document | Contents |
|----------|----------|
| [Roadmap](docs/ROADMAP.md) | Milestones, status, and parked decisions |
| [Architecture](docs/ARCHITECTURE.md) | Layering, component structure, dependency direction |
| [Design Decisions](docs/DESIGN_DECISIONS.md) | Contested choices, alternatives, and why they lost |
| [Performance](docs/PERFORMANCE.md) | Measurement methodology and results |
| [Development](docs/DEVELOPMENT.md) | Setup, build options, formatting, gotchas |

## Engineering approach

A few choices that shape the whole project, each recorded in full in
[`docs/DESIGN_DECISIONS.md`](docs/DESIGN_DECISIONS.md):

- **All logic in a library, executables are thin.** An engine that only exists
  inside `main()` cannot be tested or benchmarked.
- **C++20 over C++23.** Ensure standard library support for others if using this project.
- **Sanitizers in CI, not as an afterthought.** An order book is a graph of
  linked nodes with manual lifetimes, hence use-after-free on a cancelled order is the
  single most likely defect here, and it is silent without ASan.
- **No performance claim without a measurement.** `docs/PERFORMANCE.md` states
  its methodology before any number exists.

## License

MIT — see [LICENSE](LICENSE).

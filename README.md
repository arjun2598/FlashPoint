# FlashPoint

[![CI](https://github.com/arjun2598/FlashPoint/actions/workflows/ci.yml/badge.svg)](https://github.com/arjun2598/FlashPoint/actions/workflows/ci.yml)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

A low-latency limit order book and matching engine in modern C++, built to the
standards of production exchange infrastructure: price-time priority matching,
a measured performance story, and a test suite that runs under sanitizers on
every commit.

> **Status: in development.** Milestone 6 of 13 complete — the build system, CI,
> the core domain types, the order book, and matching for limit and market
> orders are in place. Cancel is next. See [`docs/ROADMAP.md`](docs/ROADMAP.md).
> This README describes what exists right now, not what is planned.

## What exists currently

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
  116 tests running under ASan/UBSan, including differential and randomised
  invariant tests for both the book and the engine.
- Three build presets: Debug with ASan/UBSan, Release, and RelWithDebInfo.
- `-Werror` with an aggressive warning set including `-Wconversion`.
- CI across Linux/GCC, Linux/Clang and macOS/AppleClang, in both sanitized
  Debug and Release, plus a version-pinned `clang-format` gate.

## Planned

Cancel and cancel-replace, an event stream with L2 market data, a benchmark
suite reporting p50/p99/p99.9 latency, and a replay demo.

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

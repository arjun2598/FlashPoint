# Architecture

> **Status: skeleton.** Only the build-level structure exists as of Milestone 1.
> Component sections are filled in by the milestone that introduces them, not
> written speculatively in advance. Anything below marked *(planned)* is
> intent, not implemented code.

## Repository layout

```
FlashPoint/
├── CMakeLists.txt              Root build; library target, options, summary
├── CMakePresets.json           dev / release / relwithdebinfo
├── cmake/
│   ├── CompilerWarnings.cmake  Warning policy as an INTERFACE target
│   └── Sanitizers.cmake        ASan + UBSan as an INTERFACE target
├── include/flashpoint/         Public headers (the API surface)
├── src/                        Implementation translation units
├── tests/                      GoogleTest suite, one executable
├── docs/                       This directory
└── .github/workflows/ci.yml    Build matrix + format gate
```

## Build-level structure

```
                    ┌─────────────────────────┐
                    │   flashpoint (STATIC)   │
                    │   all engine logic      │
                    └───────────┬─────────────┘
                                │ linked by
          ┌─────────────────────┼─────────────────────┐
          │                     │                     │
  ┌───────▼────────┐   ┌────────▼────────┐   ┌────────▼────────┐
  │ flashpoint_    │   │ benchmarks      │   │ demo            │
  │ tests          │   │  (planned, M10) │   │  (planned, M12) │
  └────────────────┘   └─────────────────┘   └─────────────────┘
```

Two INTERFACE targets carry build policy rather than global flags:

- `flashpoint_warnings` — linked `PRIVATE`, so consumers do not inherit it, and
  so `FetchContent` dependencies are never compiled under `-Werror`.
- `flashpoint_sanitizers` — linked `PUBLIC`, because sanitizer flags must appear
  on both the compile and link lines of every target in the graph.

## Layering (planned)

The intended dependency direction, innermost first. Each layer may depend only
on layers above it.

| Layer | Contents | Milestone |
|-------|----------|-----------|
| Domain types | `Price`, `Quantity`, `OrderId`, `Side`, `Order` — value types, no allocation, no I/O | 3 |
| Book | `OrderBook` — price levels, FIFO priority within a level, order lookup | 4 |
| Engine | `MatchingEngine` — applies commands to the book, produces events | 5–8 |
| Events | Trade / acknowledgement / rejection stream, L2 market data | 9 |
| Applications | Tests, benchmarks, demo | 1, 10, 12 |

The domain layer must not know the book exists; the book must not know the
engine exists. This is what keeps the book independently testable, and it is the
boundary that makes it possible to swap the book's internal data structure at
Milestone 11 without touching engine logic.

## Component detail

*Written as each component lands.*

- **Domain types** — pending Milestone 3.
- **Order book** — pending Milestone 4.
- **Matching engine** — pending Milestone 5.
- **Event stream** — pending Milestone 9.

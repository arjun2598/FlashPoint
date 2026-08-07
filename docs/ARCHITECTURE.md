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
│   ├── types.hpp               Side, Price, Quantity, OrderId
│   ├── order.hpp               Order — the inbound request
│   ├── ostream.hpp             Stream inserters; NOT included by the library
│   └── version.hpp.in          Generated into the build tree by CMake
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

### Domain types (Milestone 3)

Header-only, allocation-free, I/O-free. Every one is trivially copyable and
usable in constant expressions.

| Type | Representation | Size | Validity |
|------|----------------|------|----------|
| `Side` | `enum class : std::uint8_t` | 1 B | `is_valid(Side)` — an `enum class` does not constrain its range, and casting malformed wire data is how a third value appears |
| `Price` | signed `std::int64_t` ticks | 8 B | none — every value is a legitimate price, including negative (DD-009) |
| `Quantity` | unsigned `std::uint64_t` | 8 B | `is_valid()` — zero is the only malformed state; negatives are unrepresentable |
| `OrderId` | unsigned `std::uint64_t` | 8 B | `is_valid()` — zero is reserved as "no order" |
| `Order` | the four above | 32 B | `is_valid()` — structural only |

Three properties are worth stating explicitly, because later milestones depend
on them and the tests enforce all three:

- **The types are not interconvertible.** Each constructor is `explicit`, so a
  raw integer cannot become a `Price`, and a `Quantity` cannot be passed where a
  `Price` belongs. `Order`'s constructor takes price and quantity adjacently;
  transposing them fails to compile rather than mispricing an order (DD-010).
- **`Order` is trivially copyable and exactly 32 bytes**, so two orders share a
  64-byte cache line. Both are asserted, the size in `tests/order_test.cpp` as a
  deliberate performance regression test.
- **Each type exposes its representation as a nested `Rep` alias.** Narrowing
  `Price::Rep` to 32 bits is a possible future extension, and this makes it a
  one-line change here rather than an edit at every use site. This is a concrete
  payoff of the wrapper over a bare typedef.

`Order` models the *inbound request*: immutable, no remaining quantity, no
timestamp. The representation the book stores for a resting order is a Milestone
4 design, once the book exists to constrain it (DD-011).

- **Order book**: pending Milestone 4.
- **Matching engine**: pending Milestone 5.
- **Event stream**: pending Milestone 9.

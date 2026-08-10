# Design Decisions

A running log of decisions that were genuinely contested, where a reasonable
engineer could have chosen otherwise. Each entry states the alternatives and why
they lost. Decisions with no real alternative are not recorded here.

The order is chronological, and that is deliberate: read straight through and the
reasoning develops, including the two places where a measurement overturned an
earlier choice. The index below is for finding one entry; the body is for
following the argument.

## Index

### Build and tooling

| | Decision | |
|---|---|---|
| [DD-001](#dd-001) | Library + thin executable split |  |
| [DD-002](#dd-002) | `include/` + `src/` split rather than header-only |  |
| [DD-003](#dd-003) | C++20, not C++23 |  |
| [DD-004](#dd-004) | GoogleTest |  |
| [DD-005](#dd-005) | Dependencies via CMake `FetchContent`, pinned to tags |  |
| [DD-006](#dd-006) | Warning and sanitizer policy as INTERFACE targets |  |
| [DD-007](#dd-007) | `-Wconversion` and `-Wsign-conversion` enabled |  |
| [DD-008](#dd-008) | Version header generated from `project()` |  |
| [DD-039](#dd-039) | Two benchmark harnesses, because they answer two questions |  |
| [DD-040](#dd-040) | Benchmarks are built in CI but never run there |  |

### Domain types

| | Decision | |
|---|---|---|
| [DD-009](#dd-009) | `Price` is an integer value of ticks |  |
| [DD-010](#dd-010) | Hand-written strong types, not aliases or a generic template |  |
| [DD-011](#dd-011) | `Order` is the inbound request only |  |
| [DD-012](#dd-012) | Validation happens at the boundary, not in constructors |  |
| [DD-013](#dd-013) | Stream inserters live in a separate header |  |
| [DD-024](#dd-024) | Order type and time-in-force are fields on `Order` |  |

### The order book

| | Decision | |
|---|---|---|
| [DD-014](#dd-014) | The book is single-instrument and symbol-unaware |  |
| [DD-015](#dd-015) | Price levels stay in a `std::map`, for now | revisited at Milestone 11 |
| [DD-016](#dd-016) | Orders within a level are an intrusive list over a pooled vector |  |
| [DD-017](#dd-017) | A node stores only what the matching path reads |  |
| [DD-018](#dd-018) | The book's public interface is handle-based |  |

### The matching engine

| | Decision | |
|---|---|---|
| [DD-019](#dd-019) | Trades are delivered to a caller-supplied sink |  |
| [DD-020](#dd-020) | A trade records the aggressor's side |  |
| [DD-021](#dd-021) | `Trade` is an aggregate, built with designated initialisers |  |
| [DD-022](#dd-022) | The matching loop lives in the engine, driven by small book primitives | still open |
| [DD-023](#dd-023) | `MatchingEngine` is header-only |  |
| [DD-025](#dd-025) | Market orders get a venue-configured protection price |  |
| [DD-026](#dd-026) | Fill-or-kill checks feasibility before matching |  |
| [DD-027](#dd-027) | `SubmitResult` gains a `cancelled` quantity |  |

### Cancel and modify

| | Decision | |
|---|---|---|
| [DD-028](#dd-028) | Cancel has its own result type |  |
| [DD-029](#dd-029) | A cancel that misses reports one status |  |
| [DD-030](#dd-030) | Modify follows the standard queue priority rule |  |
| [DD-031](#dd-031) | The new quantity is the new remaining, not a new total | diverges from FIX |
| [DD-032](#dd-032) | A repriced order goes back through matching |  |
| [DD-033](#dd-033) | A modify keeps the order's id | diverges from FIX |

### Events and market data

| | Decision | |
|---|---|---|
| [DD-034](#dd-034) | An event is one flat record with a type tag |  |
| [DD-035](#dd-035) | The event sink replaced the trade sink |  |
| [DD-036](#dd-036) | The engine assigns sequence numbers |  |
| [DD-037](#dd-037) | A modify publishes `Modified`, never a second `Accepted` |  |
| [DD-038](#dd-038) | The depth snapshot writes into a caller-supplied buffer |  |

### Performance

| | Decision | |
|---|---|---|
| [DD-041](#dd-041) | A pooled allocator for the level maps: tried, measured, reverted | **reverted** — measured no improvement |
| [DD-042](#dd-042) | Bids are stored descending, so `best_bid()` is `begin()` | **fixed a real bug** the reverted attempt exposed |

### The demo

| | Decision | |
|---|---|---|
| [DD-043](#dd-043) | Script replay and the interactive prompt are one parser |  |
| [DD-044](#dd-044) | No arguments runs the tour, and standard input must be asked for | fixed a hang |
| [DD-045](#dd-045) | The synthetic feed also settles DD-041 | settled DD-041 |

---


**Two entries are worth reading even out of context.** [DD-041](#dd-041) is an
optimisation that was built, measured, found to do nothing, and reverted.
[DD-042](#dd-042) is the bug that negative result led to: `best_bid()` had been
O(log L) while documented as O(1), and no amount of reasoning had found it.

---

<a id="dd-001"></a>

## DD-001 — Library + thin executable split

**Decision:** all logic lives in a `flashpoint` static library. Tests,
benchmarks and the demo are thin executables that link against it.

**Alternative:** a single executable with the engine inside `main.cpp`.

**Why:** an engine that only exists inside `main()` cannot be unit tested or
benchmarked. This split is what makes every later milestone possible. It also
forces an explicit public API surface, which is the boundary the tests exercise.

---

<a id="dd-002"></a>

## DD-002 — `include/` + `src/` split rather than header-only

**Decision:** public headers in `include/flashpoint/`, definitions in `src/`.

**Alternative:** header-only, letting the compiler inline across the entire
engine.

**Why:** header-only wrecks incremental build times and erases the API boundary.
The compromise: small, hot value types (`Price`, `Order`) will be defined
entirely in headers so they still inline at call sites, while orchestration code
lives in translation units. Revisit with measurements at Milestone 11: with LTO
this may be moot.

**Common mistake this avoids:** going header-only "for performance" before
measuring, then discovering the bottleneck was a cache miss in the book, not a
missed inline.

---

<a id="dd-003"></a>

## DD-003 — C++20, not C++23

**Decision:** target C++20.

**Alternative:** C++23, which offers `std::expected` (order acceptance/rejection
without exceptions) and `std::flat_map` (a sorted-vector map that is close to
the price-level container we would otherwise hand-roll).

**Why:** C++23 library support is uneven. Apple libc++ has `std::expected` but
`std::flat_map` availability varies, and CI runners differ across platforms. For
a portfolio project the worst possible outcome is "does not compile on the
reviewer's machine". Where `std::expected` is wanted, a small internal
`Result<T>` is a few dozen lines.

**Cost accepted:** we write a little more code by hand.

---

<a id="dd-004"></a>

## DD-004 — GoogleTest

**Decision:** GoogleTest v1.17.0.

**Alternatives:** Catch2 v3 (nicer `SECTION`s for order-book state fixtures, no
mocking); doctest (fastest compiles, weakest tooling).

**Why:** GoogleTest is widely used in production C++ codebases, and GMock will earn its
keep at Milestone 9 when we test the engine's event/callback boundary. That is
an interaction test, and interaction tests want a mock.

**Cost accepted:** slower test compiles than doctest.

---

<a id="dd-005"></a>

## DD-005 — Dependencies via CMake `FetchContent`, pinned to tags

**Decision:** `FetchContent` with exact release tags and `GIT_SHALLOW`.

**Alternatives:** vcpkg/Conan (proper binary caching, faster reconfigures);
git submodules (no configure-time network).

**Why:** `git clone && cmake --preset dev && cmake --build --preset dev` must
work with nothing else installed. Every step a reviewer has to perform before
seeing a green build is a step where they stop. Package managers optimise
reconfigure time, which is a maintainer's problem, not a reviewer's.

**Cost accepted:** ~30s on the first configure while GoogleTest is fetched and
built.

**Detail:** GoogleTest is declared `SYSTEM`, so its headers are excluded from our
`-Werror` policy. Without this, third-party warnings would break our build.

---

<a id="dd-006"></a>

## DD-006 — Warning and sanitizer policy as INTERFACE targets

**Decision:** `flashpoint_warnings` and `flashpoint_sanitizers` are INTERFACE
libraries defined in `cmake/`.

**Alternative:** global `add_compile_options()`.

**Why:** global flags leak into `FetchContent` dependencies, so `-Werror` would
fail on GoogleTest's own code. Targets opt in. Warnings are linked `PRIVATE`
(consumers should not inherit our policy); sanitizers are linked `PUBLIC`
because those flags must reach both the compile and the link line of every
target in the graph.

---

<a id="dd-007"></a>

## DD-007 — `-Wconversion` and `-Wsign-conversion` enabled

**Decision:** enable them, despite the noise.

**Why:** this codebase is arithmetic over fixed-width integers — prices in
ticks, quantities, order counts. A silent narrowing in fill quantity arithmetic
is not a style issue, it is a money bug. Paying for explicit casts is the point.

**Cost accepted:** more verbose code at integer-type boundaries. This pressure is
intentional as it should push us toward strong typedefs at Milestone 3 rather than
raw integers everywhere.

---

<a id="dd-008"></a>

## DD-008 — Version header generated from `project()`

**Decision:** `version.hpp` is generated by `configure_file` from
`version.hpp.in`; the version exists in exactly one place.

**Alternative:** hand-maintained constants in a checked-in header.

**Why:** two copies of a version number diverge. `version()` is deliberately
out-of-line rather than `constexpr` so a caller can detect a mismatch between
the headers they compiled against and the library they linked.

---

<a id="dd-009"></a>

## DD-009 — `Price` is an integer value of ticks

**Decision:** `Price` wraps a signed `std::int64_t` count of ticks. Tick size and
the mapping to a displayed currency amount are dependent on instrument configuration.

**Alternatives:** fixed-point with a compile-time scale (e.g. integer × 10⁻⁴);
runtime-scaled decimal carrying its scale per instance.

**Why:** Prices are stored as integer tick counts, so comparisons are exact and never rely on floating-point arithmetic. The type stays eight bytes. A tick count also maps naturally to an array index, which keeps open the possibility of implementing a direct-indexed price ladder in Milestone 11.

A compile-time scale bakes one scale into a type that should work across asset
classes, and mixes display formatting with the core matching logic. Storing the scale alongside every price would make each Price object larger and require extra work every time prices are compared.

**Deliberately signed.** Negative prices can exist: oil futures settled below zero
in 2020 and calendar-spread instruments quote negative routinely. Whether negative prices are allowed is a business rule enforced by the engine, not by the `Price` type itself.
Consequently `Price` has no `is_valid()`, as unlike `Quantity` and `OrderId`,
there is no value of the representation that is inherently malformed.

**Cost accepted:** A value like `Price{10250}` is not human-readable without knowing the tick
size. Formatting is the demo's job at Milestone 12.

---

<a id="dd-010"></a>

## DD-010 — Hand-written strong types, not aliases or a generic template

**Decision:** `Price`, `Quantity` and `OrderId` are each written out as a
distinct class with an `explicit` constructor and only the operations that make
sense for it.

**Alternatives:** plain aliases (`using Price = std::int64_t`); a generic
`StrongType<T, Tag, Skills...>` with capability mixins.

**Why not type aliases?:** `Order{id, side, quantity, price}` would compile with price
and quantity transposed. Both are 64-bit integers, so the `-Wconversion`
warning we turned on precisely to catch integer mistakes (DD-007) is blind to
it. Accidentally swapping price and quantity is a simple but very costly bug, yet it is the kind of bug the compiler can prevent entirely.

**Why the generic template loses:** it is a metaprogramming framework in service
of three types. It costs readability and compiler diagnostics to save
repetition that has not yet become an issue.

Each type gets exactly its own algebra rather than a uniform one. `Quantity` has
`+` and `-` because adding quantities is meaningful. `Price` has neither,
because adding two prices has no meaning. When a consumer eventually needs price
arithmetic, the right shape is the one `std::chrono` uses:
`Price - Price` yields a tick delta, `Price + delta` yields a `Price`, and
`Price + Price` stays ill-formed.

**Verified, not assumed:** `tests/order_test.cpp` asserts
`!std::is_constructible_v<Order, OrderId, Side, Quantity, Price>`.
Transposed calls are not merely discouraged, they do not compile.

**Cost accepted:** roughly forty lines per type, with some repetition between them.

---

<a id="dd-011"></a>

## DD-011 — `Order` is the inbound request only

**Decision:** `Order` is immutable and models what a client asked for. The representation the book stores for a resting order
is designed at Milestone 4.

**Alternatives:** one `Order` carrying both original and remaining quantity with
the book mutating it, or defining `Order` and `RestingOrder` together now.

**Why:** a single type conflates a request with a book entry. An incoming order has no remaining quantity until the matching engine accepts it. But defining `RestingOrder` right now means designing it for a consumer that does not exist. Once the order book exists, it will naturally determine what additional information a resting order needs.

**Consequence:** Milestone 4 necessarily introduces a second order type. That is
the intended outcome, not an oversight.

**Related:** `Order` carries **no timestamp**. Time priority already comes from FIFO ordering within a price level, which the book provides.
Storing arrival time per order would be eight redundant bytes on the hottest object in the system.

---

<a id="dd-012"></a>

## DD-012 — Validation happens at the boundary, not in constructors

**Decision:** Constructors do not validate their inputs.`Quantity` is unsigned, so negatives are unrepresentable rather than rejected. `is_valid()` is offered on
the types that have a malformed state, and the engine calls it once at its
entry point (Milestone 5).

**Alternatives:** throwing constructors, a private constructor plus a static
factory returning `Result<Order>`.

**Why:** what you validate at the boundary you may trust internally. Constructors that validate every object would repeat the same checks throughout the system, adding unnecessary work on the hot path. This seems more suitable for low-latency systems and is a deliberate contrast to the validate-everywhere style appropriate to less performance-sensitive code.

A `Result<T>` factory would be a type-safe alternative to exceptions. We deliberately postponed implementing `Result<T>` under DD-003, and introducing it now would also make every construction site more verbose.

**Cost accepted:** nothing prevents constructing a zero-quantity `Order` deep
inside the code. The mitigation is that the one place orders enter the system is
the place that checks.

**The exception that proves the rule:** `Quantity::operator-=` carries an
`assert` against unsigned wraparound. Wraparound is well-defined behaviour, so
neither UBSan nor any warning catches it, meaning an over-fill would silently produce a
resting quantity of 1.8 × 10¹⁹. `assert` costs nothing under `NDEBUG`, so the
check exists in every development and CI build and vanishes in release. Debug
assertions are free; runtime validation is not.

---

<a id="dd-013"></a>

## DD-013 — Stream inserters live in a separate header

**Decision:** `operator<<` for the domain types is in `flashpoint/ostream.hpp`,
which the library never includes. Tests, the demo and diagnostics include it
explicitly.

**Alternative:** put the inserters in `types.hpp` and `order.hpp` alongside the
types.

**Why:** `<ostream>` is an expensive header to include. Pulling it into the core domain types would increase compile times for every translation unit, even though the matching engine itself never formats output.

Without stream operators, GoogleTest prints failed comparisons as raw bytes instead of readable values.

---

<a id="dd-014"></a>

## DD-014 — The book is single-instrument and symbol-unaware

**Decision:** `OrderBook` holds one instrument's orders and has no concept of a
symbol. Version 1 trades a single instrument end to end.

**Alternative:** the engine holding a `map<SymbolId, OrderBook>`, with a symbol
threaded through every command and event.

**Why:** a book is better as a per-instrument structure.
Real venues tend to shard by symbol onto independent, single-threaded matching
partitions, so multi-symbol support is a layer above the book, not a feature
of it. Because the book is symbol-unaware, that layer is a wrapper around an
unchanged `OrderBook` whenever it is wanted.

**Cost accepted:** V1's demo trades one instrument.

---

<a id="dd-015"></a>

## DD-015 — Price levels stay in a `std::map`, for now

**Decision:** both sides are `std::map<Price, Level>` in ascending order, so the
best ask is `begin()` and the best bid is `rbegin()`, making both O(1). Potentially replacing this
is scheduled for Milestone 11, driven by the Milestone 10 profile.

**Alternatives:** a sorted vector; a direct-indexed ladder over the tick range; a
hybrid ladder-plus-map.

This one was argued at length rather than assumed. The case _against_
deferring is real: The best solution should be designed first before implementing it, rather than
rushing into a less than optimal one.

**What decided it:**

1. **The simple book is a test oracle.** When the structure is replaced, both
   implementations run the same order flow and must produce identical output.
   Differential testing is a valuable technique available for a
   data-structure rewrite where a subtle priority bug is invisible to ordinary
   unit tests, and it requires the simple version to exist. This milestone
   already uses the technique against a naive reference model.
2. **A benchmark without a baseline is not a measurement.** "p99 = 400ns" alone
   says nothing. "1200ns → 180ns, and here is the profile" is a result, and it
   requires having built the slow one.
3. **Domain knowledge names what is _usually_ hot, not what is hot here.** The
   literature says level lookup matters. It does not say it dominates _this_
   code, where the order index or allocation may well be larger. This serves to
   verify the claims for this use case through comparison.

**The condition that makes it safe:** the deferral is only cheap if no caller can
depend on the container, which is why DD-018 exists and is enforced by tests.
Without that, this would be a trap rather than a decision.

**Cost accepted:** every level lookup is O(log L) in the number of distinct
price levels, with a pointer chase per node. `add` and `remove` both carry that
term right now.

---

<a id="dd-016"></a>

## DD-016 — Orders within a level are an intrusive list over a pooled vector

**Decision:** each price level is a doubly-linked FIFO queue whose nodes live in
one `std::vector<Node>` owned by the book. Freed nodes form a free list threaded
through the nodes themselves.

**Alternatives:** `std::deque<Node>` per level; `std::list<Node>` per level plus
an id→iterator index.

**Why not `std::deque`:** cancel-from-middle is O(n). That is a _design_ error
rather than a tuning problem, since real order flow tends to be dominated by cancels, and
fixing it later would mean rewriting the algorithm and its API.

**Why not `std::list`:** it has the same O(1) complexity as the intrusive
version, so this is honestly a constant-factor decision, not a complexity one.
DD-015's own principle argues for deferring it. What tips it is that
`std::list` allocates per order, and unbounded allocation on the hot path produces an unbounded latency _tail_,
which for a matching engine is the characteristic that matters most. That, plus the fact that the answer
here is genuinely well established rather than empirical, is why this one was
built now while DD-015 was deferred.

**Why indices rather than pointers:** a `std::uint32_t` index is half the size of
a pointer, and it survives the pool reallocating as it grows, which is what
lets the pool start empty instead of demanding a fixed capacity up front. It also
leaves `OrderBook` copyable, since it holds no self-referential pointers.

**Cost accepted:** manual lifetime management, and pointer surgery with four
distinct unlink cases. The mitigation is that every CI job runs under ASan and
UBSan (Milestone 1), and that all four cases plus a differential test cover it.

---

<a id="dd-017"></a>

## DD-017 — A node stores only what the matching path reads

**Decision:** `Node` is `{OrderId, Quantity remaining, prev, next}` = 24 bytes.
Side and price are implied by the level containing it. The order index maps an
id to a `Locator` carrying the side and price needed to find that level.

**Alternative:** a self-describing node holding side, price and original
quantity.

**Why:** side and price are identical for every order at a level, so storing them
per order is pure duplication on the hottest object in the book. Splitting the
cold bookkeeping into the index minimises queue walk.

**Cost accepted:** a `Node` cannot be interpreted without knowing its level, and
original quantity is absent. If Milestone 9's events need the original, adding
one field to a 24-byte struct is cheap.

---

<a id="dd-018"></a>

## DD-018 — The book's public interface is handle-based

**Decision:** every accessor returns a value or an `OrderId`. Nothing returns an
iterator, a reference into the book, or any type derived from the internal
containers. `tests/order_book_test.cpp` pins this with static assertions on the
exact return types, plus detection-idiom checks that `OrderBook` exposes no
`iterator` type and is not iterable.

**Alternative:** exposing level iteration directly, which would be convenient for
market-data snapshots at Milestone 9.

**Why:** this is the precondition that makes DD-015 a deferral rather than a
trap. If the engine can hold an iterator into the price-level container, then
replacing that container stops being a private change and the Milestone 11 work
becomes a rewrite. Asserting the property in a test rather than a comment means
it fails at the moment it is violated, when undoing it is still cheap.

`front_at()` exists for a related reason: time priority that cannot be observed
cannot be tested, and the FIFO guarantee needs to be verified.

**Cost accepted:** market-data snapshots at Milestone 9 will need a purpose-built
accessor rather than raw iteration.

---

<a id="dd-019"></a>

## DD-019 — Trades are delivered to a caller-supplied sink

**Decision:** `submit(order, on_trade)` is templated on the sink and invokes it
once per execution, as each execution happens.

**Alternatives:** returning a `std::vector<Trade>`; filling an engine-owned
buffer the caller reads through a `std::span`.

**Why:** many orders in real flow are non-marketable and produce zero trades, so
the design should make that case free and the sweeping case cheap. Returning a
vector allocates precisely when the engine is busiest. An engine-owned buffer
avoids the allocation but hands the caller a view into engine memory that
silently changes on the next `submit`, which contradicts the handle-based
discipline of DD-018.

A sink does neither. It is also likely the better shape for Milestone 9's event stream,
so the seam is built once rather than migrated to later.

**Cost accepted:** `MatchingEngine::submit` is a template, so the engine is
header-only. See DD-023.

---

<a id="dd-020"></a>

## DD-020 — A trade records the aggressor's side

**Decision:** `Trade` carries maker id, taker id, price, quantity, **and** which
side aggressed.

**Alternatives:** the four fields without the side; or those plus a trade
sequence number.

**Why:** the engine knows the aggressor for free, and a consumer reading the
tape cannot reconstruct it from the trade alone. It is a real signal: a run
of buyer-aggressed prints is buying pressure. Eight bytes after padding is a
cheap price for information that is otherwise unrecoverable.

Sequence numbers were left out: numbering is the job of whatever assembles the
event stream at Milestone 9, and adding it here would mean deciding now who owns
the counter.

---

<a id="dd-021"></a>

## DD-021 — `Trade` is an aggregate, built with designated initialisers

**Decision:** `Trade` is a plain struct with public members, constructed as
`Trade{.maker_id = ..., .taker_id = ..., ...}`.

**Alternative:** a class with a positional constructor and accessors, matching
`Order`.

**Why:** `maker_id` and `taker_id` are both `OrderId`. This is the one case in
the codebase where strong types cannot help: the two arguments genuinely _are_
the same type, so a positional constructor would let them be transposed silently,
producing trades that attribute every execution to the wrong party while every
quantity still balanced.

C++20 designated initialisers put the field name at the call site, which makes
the mistake visible exactly where it would be made. A `static_assert` keeps
`Trade` an aggregate so this cannot be quietly undone.

**Consistency cost accepted:** `Trade` and `Order` are shaped differently. That
is justified by the different risk: `Order`'s fields are four distinct types, so
positional construction is already safe there (DD-010).

---

<a id="dd-022"></a>

## DD-022 — The matching loop lives in the engine, driven by small book primitives

**Decision:** the engine reads the touch, reads the front order, and tells the
book to `reduce` or `remove` it. One loop in the engine, with the book gaining
only `remaining_of()` and `reduce()`.

**Alternatives:** moving part of the loop into the book; or having the book hand
out a cursor into a price level that the engine walks.

**Why:** a cursor is a more efficient answer as it would turn a sweep from
one price lookup _per fill_ into one per _level_. It is also exactly the handle
into the book's internals that DD-018 forbids, and giving it away would make the
Milestone 11 container swap a rewrite rather than a private change. Spending that
flexibility before any profile says level lookups matter is the trade we currently
decided not to make (DD-015).

Moving the loop into the book was rejected for a simpler reason: it does not
reduce the number of lookups, it only relocates the code, and it blurs the line
between the container and the algorithm.

**Cost accepted:** a sweep of N resting orders performs N level lookups instead
of one per level. If Milestone 10 shows that dominating, a cursor will become a
measured change and can be introduced as an opaque handle rather than a leaked
iterator.

**A related invariant, recorded because it is invisible:** `OrderBook::reduce`
does not touch the order's queue links. A partially filled order keeps its place
in line. Sending it to the back would leave aggregate depth identical and betray
itself only in the order of later executions, which is why it has a dedicated
test.

---

<a id="dd-023"></a>

## DD-023 — `MatchingEngine` is header-only

**Decision:** the whole engine lives in `matching_engine.hpp`, a deliberate
exception to DD-002's rule that orchestration belongs in a translation unit.

**Why:** `submit` is templated on the sink (DD-019), so it cannot live in a `.cpp`
without either explicit instantiation for every sink type or type erasure.
Type erasure would reintroduce the indirect call the sink design exists to avoid.
Being header-only also lets the compiler inline the sink call and the book
primitives into the matching loop, which is the hottest code in the project.

**Cost accepted:** every translation unit that includes the engine recompiles
when the matching logic changes. With one engine and a handful of consumers, that
is not yet a real cost; if it becomes one, a non-template `submit` taking an
erased sink can be added _alongside_ the template for cold callers.

---

<a id="dd-024"></a>

## DD-024 — Order type and time-in-force are fields on `Order`

**Decision:** `Order` gains `OrderType` (Limit, Market) and `TimeInForce`
(GoodTillCancel, ImmediateOrCancel, FillOrKill). Both are `enum class` on
`std::uint8_t`.

**Why this costs nothing:** `Order` was 25 bytes of payload inside a 32-byte
object. Two more bytes makes 27, which still rounds to 32. `sizeof(Order)` is
unchanged and the layout test in `tests/order_test.cpp` did not need editing.

**Validation:** a market order with GoodTillCancel is rejected as malformed. A
market order has no price, so it cannot rest, so the combination has no meaning.
This lives in `Order::is_valid()` because it is a property of the message, not
venue policy.

**Construction:** `Order::limit()` and `Order::market()` factories were added
alongside the general constructor. `Order::market()` takes no price, which stops
callers from supplying one that would be ignored.

---

<a id="dd-025"></a>

## DD-025 — Market orders get a venue-configured protection price

**Decision:** `EngineConfig::market_protection_ticks` sets how far past the
opposite touch a market order may trade. A market buy with a best ask of 100 and
a band of 5 gets an effective limit of 105. Anything it cannot fill by then is
cancelled.

**Alternatives:** let the client supply a protection price in `Order::price`; or
have no protection at all.

**Why:** without protection, a market order into a thin book can sweep to an
arbitrary price. Real venues guard against this; CME calls it
market-with-protection.

A client-supplied protection price was rejected because it makes a market order
functionally identical to an ImmediateOrCancel limit order, leaving
`OrderType::Market` as a label rather than a behaviour. A venue band keeps the
distinction real: the client does not need to know the current price to send one.

**How it shapes the code:** the effective limit is resolved once, before the
matching loop:

```
effective_limit = (type == Market) ? protection_price(side) : order.price()
```

The loop is then unchanged for both order types. This was the better half of two
options considered at the start of Milestone 6: a branch inside the loop, or
storing market orders as a synthetic extreme limit price. Protection gives the
single code path of the second without the fake price, because the protection
price is a real number derived from a real touch.

**Details worth knowing:**

- Protection is measured from the touch when the order arrives, not
  re-evaluated per level as the sweep progresses.
- If the opposite side is empty there is no touch, and nothing to trade against
  either. The order is cancelled in full.
- `protection_price` saturates rather than wrapping. A touch near the end of the
  price range would otherwise overflow a signed integer, which is undefined
  behaviour.

**Cost accepted:** the engine now carries venue configuration. It is one integer.

---

<a id="dd-026"></a>

## DD-026 — Fill-or-kill checks feasibility before matching

**Decision:** a FillOrKill order first asks the book how much it could trade at
its limit or better, via `OrderBook::quantity_available()`. If that is less than
the order quantity, nothing happens at all.

**Alternatives:** match optimistically and roll back if it fell short; or expose
level iteration so the engine can walk the book itself.

**Why not rollback:** trades are handed to the sink as they occur, and a trade
already delivered cannot be withdrawn. Undoing a partial sweep correctly is
harder than checking first, and any bug in it produces phantom executions.

**Why not level iteration:** that is the cursor turned down in DD-022, for the
same reason. `quantity_available` answers one question and exposes nothing.

**Cost accepted:** a FillOrKill order makes one extra pass over the levels within
its limit. Only FillOrKill pays it.

The engine asserts after matching that a FillOrKill order which passed the check
did in fact fill completely. The two paths have to agree, and an assertion is
cheaper than discovering they do not in production.

---

<a id="dd-027"></a>

## DD-027 — `SubmitResult` gains a `cancelled` quantity

**Decision:** `SubmitResult` becomes `{status, filled, resting, cancelled}`, with
`filled + resting + cancelled` equal to the submitted quantity for any accepted
order.

**Alternative:** new status codes such as `Cancelled` or
`PartiallyFilledThenCancelled`.

**Why:** `status` answers whether the order was accepted. The quantities answer
what became of it. Keeping those separate stops the status list from needing a
case for every combination as Milestones 7 and 8 add cancel and modify.

It also strengthens a test that already existed. The randomised engine test
checked `filled + resting == submitted`; it now checks the three-way identity
across market orders and all three time-in-force values.

---

<a id="dd-028"></a>

## DD-028 — Cancel has its own result type

**Decision:** `MatchingEngine::cancel()` returns
`CancelResult{status, cancelled}` rather than reusing `SubmitResult` or
returning a bool.

**Why:** a cancel is not a submission. Reusing `SubmitResult` would leave
`filled` and `resting` permanently zero, which reads like a bug rather than a
deliberate shape. A bare bool would lose the quantity.

The quantity matters. An order that filled 30 of 50 before the cancel arrived
only had 20 left to pull, and that is the number the client needs. `cancel`
therefore reads `remaining_of()` before removing, not the order's original size.

---

<a id="dd-029"></a>

## DD-029 — A cancel that misses reports one status

**Decision:** `CancelStatus::UnknownOrder` covers three cases the engine cannot
tell apart: the id never existed, the order already filled completely, or it was
already cancelled.

**Alternative:** keep a record of terminated orders so the engine can distinguish
them.

**Why:** the book only knows whether an id is currently resting. Telling the
three apart means retaining every id ever seen, forever, or designing an
eviction policy. That is an order-history subsystem, and putting it in the
matching engine would give the hot path unbounded memory growth to serve a
diagnostic.

**Cost accepted:** a client cancelling an order that just filled gets a less
helpful reason.

**Separate from this:** a cancel naming `OrderId::kNone` gets its own
`RejectedInvalidId` status. That is a malformed message rather than a miss, and
it mirrors the structural check `submit` performs.

---

<a id="dd-030"></a>

## DD-030 — Modify follows the standard queue priority rule

**Decision:**

| Change | Priority |
|---|---|
| quantity reduced, same price | retained |
| quantity unchanged, same price | retained |
| quantity increased | lost |
| price changed | lost |

`ModifyResult` reports which happened, as `QueuePriority::Retained` or `Lost`.

**Alternatives:** every modify loses priority, which is simpler and is what some
smaller venues do; or make the rule configurable.

**Why:** shrinking an order takes nothing from the orders behind it, so there is
no reason to move it back. Growing it does, because the added size would sit
ahead of orders that were already waiting. Penalising shrinking would just push clients
to cancel and resubmit instead.

Configurability was rejected because it doubles the behaviours to test for a rule
nobody wants to flip.

**Why the result reports it:** whether an amend cost the order its place is the
most consequential thing about a modify, and the client cannot see the book to
work it out. Reporting it also makes the rule directly testable rather than
inferred from `front_at()`.

**Worth knowing about the tests:** aggregate depth looks identical whether
priority was kept or lost. A bug here is invisible unless the test checks who
fills next, so every priority case is followed by an order that trades against
the level.

---

<a id="dd-031"></a>

## DD-031 — The new quantity is the new remaining, not a new total

**Decision:** `modify(id, price, quantity)` sets the resting quantity directly.
An order with 20 left, modified to 25, rests 25 regardless of what it filled
earlier.

**Alternative:** treat it as a new total order size including fills, the way FIX
defines `OrderQty`, and reject any value at or below the filled quantity.

**Why:** the book stores only remaining quantity per resting order (DD-017). The
FIX reading needs the original quantity too, which is a field on `Node`, the
hottest object in the book.

The priority comparison follows from this: "increased" means larger than what is
currently resting, not larger than what was originally sent.

**Cost accepted:** a divergence from FIX semantics. Recorded here so it is a
known difference rather than an oversight. Adding the original quantity to `Node`
is the change that would close it.

---

<a id="dd-032"></a>

## DD-032 — A repriced order goes back through matching

**Decision:** when a modify loses priority, the order is removed and re-submitted
through `submit()`. If the new price crosses the spread, it trades.

**Alternative:** reject a modify that would cross.

**Why:** an order repriced across the spread is a new order at that price and
should behave like one. Rejecting this would force clients to cancel and resubmit,
which doesn't make sense for something expected and acceptable.

Routing through `submit()` rather than putting the order straight back in the
book is what makes this work, and it means the matching rules live in exactly one
place. It also keeps the guarantee that the engine never leaves a crossed book,
for modify as well as submit.

**Safety note:** the order is removed before the re-submit, so a rejected
re-submit would lose it. Every rejection is ruled out by construction: the id was
just removed so it cannot collide, and a Limit/GoodTillCancel order built from a
checked id, a book-supplied side and a non-zero quantity is always structurally
valid. An assertion records the reasoning.

---

<a id="dd-033"></a>

## DD-033 — A modify keeps the order's id

**Decision:** `modify` amends in place. The order keeps the id it had, even when
it loses priority and is internally removed and re-added.

**Alternative:** FIX-style cancel-replace, where the replacement carries a new
client order id and the old one is retired.

**Why:** one id keeps the priority rules the visible feature of this milestone
rather than id bookkeeping. It also means a client tracking an order does not
have to follow an id chain across amends.

**Cost accepted:** a divergence from FIX, which models a replace as a new order.
A venue reporting both ids would need the mapping kept somewhere, which is the
same order-history subsystem DD-029 pushed outside the engine.

---

<a id="dd-034"></a>

## DD-034 — An event is one flat record with a type tag

**Decision:** `Event` is a single struct carrying every field any event type
needs, plus an `EventType` saying which of them mean anything. The header
documents the contract per type.

**Alternatives:** a `std::variant` of five distinct event structs; or a sink with
a separate callback per event type.

**Why:** the deciding factor is Milestone 12. A replay demo wants to write the
stream to a file and read it back, and a fixed-size trivially copyable record
does that with no encoding step. A variant fights it, and per-type callbacks give
up having a stream at all. This is also how real market data protocols are laid
out.

**Cost accepted:** fields that mean nothing for some types. A `Cancelled` event
has a `counterparty_id` nobody should read. The mitigation is a table in
`event.hpp` stating exactly which fields each type populates, and a
`static_assert` keeping `Event` an aggregate so designated initialisers name each
field at the point it is set.

---

<a id="dd-035"></a>

## DD-035 — The event sink replaced the trade sink

**Decision:** `submit`, `modify` and `cancel` each take an event sink.
Executions arrive as `Trade` events alongside acknowledgements, rejections and
cancellations. The result types stay, as summaries.

**Alternative:** keep the trade sink and add an event sink beside it.

**Why:** trades delivered through a second path are not part of the stream, and
the ordering between a trade and the acknowledgement that preceded it would be
unobservable. Keeping both would leave the design permanently worse to avoid one
mechanical edit.

**Cost accepted:** every existing call site changed, in the library and across
five test files.

**The result types are not redundant.** `SubmitResult` and friends are a summary
of what the stream already said, for callers that want the outcome without
reading events. The tests assert the two agree: trade events must account for
exactly the reported `filled`, and cancelled events for exactly `cancelled`.

---

<a id="dd-036"></a>

## DD-036 — The engine assigns sequence numbers

**Decision:** every published event carries a `SequenceNumber`, starting at 1 and
increasing by one. Scope is one engine, which is one instrument. Owed since
DD-020 parked it.

**Why:** without numbering, a consumer that drops or reorders events cannot tell.
Numbering is what makes the stream replayable and gap-detectable, which is the
reason to have a stream rather than a set of callbacks.

Real venues number per matching partition for the same reason: a single writer is
what makes the numbering meaningful.

**Detail:** rejections are numbered too. They are events, and a consumer that
skipped them would see gaps it could not explain.

`SequenceNumber` is a strong type rather than a raw integer, consistent with
DD-010.

---

<a id="dd-037"></a>

## DD-037 — A modify publishes `Modified`, never a second `Accepted`

**Decision:** the matching core was split out of `submit` into a private
`apply()`. `submit` publishes `Accepted` then calls `apply`. `modify` publishes
`Modified` then calls `apply`.

**Why:** a modify that loses priority removes the order and puts it back, which
is a submit internally. If it reused `submit`, the stream would carry a second
`Accepted` for an order the client never resubmitted. That is not a cosmetic
problem: a consumer reconstructing order state from the stream would count the
order twice.

Splitting `apply` out also keeps the matching rules in exactly one place, which
is what DD-032 relied on when it routed repriced orders back through matching.

---

<a id="dd-038"></a>

## DD-038 — The depth snapshot writes into a caller-supplied buffer

**Decision:**
`std::size_t OrderBook::snapshot(Side, std::span<LevelSnapshot> out) const`
fills the caller's buffer, best price first, and returns how many rows it wrote.

**Alternatives:** return a `std::vector<LevelSnapshot>`; or invoke a callback per
level.

**Why:** market data is snapshotted constantly, potentially on every book change.
Returning a vector allocates every time, which is both a cost and a source of
unpredictable latency. A callback avoids the allocation but leaves the caller to
accumulate the rows.

The span approach lets a publisher reuse one array across thousands of snapshots
with no allocation, and sizing the span is how the caller picks the depth.

**Rows are copies**, so nothing returned points into the book. That keeps the
price-level container replaceable (DD-018), which matters here because a depth
snapshot is exactly the kind of API that would otherwise leak an iterator.

**Cost accepted:** a slightly clunkier call site than returning a container.

---

<a id="dd-039"></a>

## DD-039 — Two benchmark harnesses, because they answer two questions

**Decision:** Google Benchmark measures throughput; a small hand-written harness
measures the per-operation latency distribution.

**Alternative:** Google Benchmark alone.

**Why:** rule 2 in `docs/PERFORMANCE.md` says report p50 / p99 / p99.9, because
for a matching engine the tail is the product. Google Benchmark reports mean,
median and standard deviation, and has no native percentiles. Using it alone
would have meant amending the rule to match the tool, which is the wrong way
round.

The measurement turned out to justify the split for a second reason not
predicted. `steady_clock` on this machine ticks at about 41.67 ns, and several
operations are faster than that, so the latency harness cannot resolve them. The
throughput harness amortises across millions of iterations and reads 4.23 ns for
top of book. Neither harness alone would have produced both the distribution and
the sub-tick means.

**The latency harness is deliberately small.** Record one timing per operation
into a vector reserved up front, sort, index the percentiles. Percentiles are
nearest-rank with no interpolation, so every figure published is a measurement
that actually happened.

**Instrumentation cost is measured and reported**, not hidden. An empty timed
region costs about 42 ns on this machine, which is stated next to the results so
a reader can separate the engine from the instrument.

---

<a id="dd-040"></a>

## DD-040 — Benchmarks are built in CI but never run there

**Decision:** the `release` preset builds the benchmark targets, so CI compiles
them on every push. CI never executes them and never asserts on timings.

**Alternatives:** run them and fail on threshold regressions; or leave them out
of CI entirely.

**Why:** the real risk is bit-rot. The benchmarks link against the engine's API,
and a milestone that changes a signature should break the build immediately
rather than two milestones later. Compiling them catches that for the cost of
one extra target.

Running them would not. Shared CI runners are virtualised and noisy, so any
threshold would either be so loose it catches nothing or so tight it flaps.
Automated regression detection needs dedicated hardware to mean anything, and
inventing a number from a shared runner would be worse than having none.

**Consequence:** benchmarks are off in the `dev` preset, so the everyday
configure stays fast and does not fetch Google Benchmark. They are only
meaningful in a Release build (rule 1), so tying them to the release
presets is also the semantically correct place.

---

<a id="dd-041"></a>

## DD-041 — A pooled allocator for the level maps: tried, measured, reverted

**Decision:** not adopted. Recorded because the negative result is the useful
part.

**The reasoning that motivated it.** Milestone 10 measured every book operation
costing 1.5–1.75× more on a book with 100× the price levels but the same number
of orders. Reading top of book was 1.73× slower despite already being O(1),
which no algorithm can explain. That pointed at cache locality: a tree walk
chasing pointers into nodes scattered across the heap. Giving the maps their
nodes from a contiguous arena is the cheapest change that attacks that, with no
API change and no conflict with `Price` being unbounded.

**What it measured.** Nothing. Every benchmark landed within noise of the
baseline: `add_then_cancel` 195 → 198 ns, `top_of_book` 7.33 → 7.37 ns,
`snapshot` 73.8 → 73.9 ns.

**Why it did nothing.** The benchmark builds each book in one burst, inserting
every level in sequence. A general-purpose allocator servicing a run of
same-sized requests already returns near-contiguous memory, so the nodes were
never scattered and there was nothing for a pool to fix. The scattering a pool
prevents happens in a book that churns levels for hours, which no benchmark here
simulates.

**Why it was reverted rather than kept.** Rule 5 in `docs/PERFORMANCE.md` says
no optimisation is committed without a before/after measurement. Keeping it would
have meant shipping a change justified only by a plausible story about a workload
we do not measure. It also cost real API surface: the maps' allocators point at
an arena the book owns, so `OrderBook` had to become move-only.

**What it did buy.** Ruling out node scattering is what forced the search to
continue, and the next thing it found was DD-042.

---

<a id="dd-042"></a>

## DD-042 — Bids are stored descending, so `best_bid()` is `begin()`

**Decision:** the bid side uses `std::greater<Price>`. Both sides now put the
best price at `begin()`.

**What was wrong.** Milestone 4 used one comparator for both sides so the two
maps would be the same type and every helper could be written once. That made
`best_ask()` `begin()` and `best_bid()` `rbegin()`, documented as O(1) each.

`begin()` is O(1) because `std::map` caches its leftmost node. There is no
corresponding cache for the rightmost, so `rbegin()` walks the tree to find the
maximum. **`best_bid()` was O(log L)**, and the complexity table in
`order_book.hpp` said otherwise.

**How it was found.** After DD-041 ruled out node scattering, the remaining
suspect was the tree itself. Splitting `top_of_book` into its two halves settled
it immediately:

| | 10 levels | 1,000 levels |
|---|---:|---:|
| `best_ask()` — `begin()` | 1.54 ns | 1.55 ns |
| `best_bid()` — `rbegin()` | 4.14 ns | 7.32 ns |

One scales, the other does not. That is not a cache effect.

**Measured result:**

| Benchmark | before (deep) | after (deep) | change |
|---|---:|---:|---:|
| `best_bid` | 7.32 ns | 1.55 ns | **4.7× faster** |
| `top_of_book` | 7.33 ns | 3.11 ns | **2.36× faster** |
| `snapshot`, ten levels | 73.8 ns | 30.0 ns | **2.46× faster** |
| `add_then_cancel` | 195 ns | 204 ns | unchanged |
| `cross_one_level` | 35.4 ns | 35.8 ns | unchanged |

The three read paths are now **flat across book depth**; they were 1.7× worse on
the deep book before. The mutation paths are untouched, which is exactly right:
they do not read the touch, and their cost is `map::find`.

**Cost accepted.** The two sides are different types now, so `levels_for()` is
gone and a few helpers are written per side or as a template. That was the
simplification Milestone 4 bought with the shared comparator, and it turned out
to be paid for with a hidden O(log L).

**Unexpected benefit.** Both sides now order best-first, so every walk over
levels runs forward from `begin()`. `snapshot` and `quantity_available` lost
their reverse-iteration special cases and got shorter, not longer.

**What this leaves.** The remaining deep-book penalty on mutations is `map::find`
tree depth: roughly four node visits against ten. No allocator or comparator
change touches that; only a flatter structure does. Parked with the diagnosis
recorded, so a future attempt starts from evidence rather than a hunch.

---

<a id="dd-043"></a>

## DD-043 — Script replay and the interactive prompt are one parser

**Decision:** the demo's command parser reads a `std::istream`. Given a file it
replays a scenario; given a terminal it is an interactive prompt. There is no
separate REPL implementation.

**Why:** The parsing does not care where the lines come from, so interactivity costs a prompt
and not exiting on a bad line.

What is *not* free is a REPL that feels like a proper tool: history, arrow keys,
tab completion. That needs `readline` or similar, and DD-005 committed to
building with nothing else installed.

**Why the script is still the primary artifact.** A scripted run produces
deterministic output that pastes into a README; a terminal session does not.
Most people who look at a repository read rather than run it. And a script can
narrate itself: lines beginning `##` print as headings, so
`demo/scenarios/tour.txt` is simultaneously the demo, a worked example, and
documentation of the engine's behaviour that cannot drift from the code.

---

<a id="dd-044"></a>

## DD-044 — No arguments runs the tour, and standard input must be asked for

**Decision:** bare `flashpoint_demo` runs the built-in tour. Reading standard
input requires an explicit `-`.

**What the first version did, and why it was wrong.** It checked `isatty()` and
read standard input whenever it was not a terminal. That reasoning sounds right
but hangs forever the moment standard input is an open pipe with nothing on it,
which is what happens under CI, `make`, and most process runners. The first run
of the demo hung.

Guessing what the user meant from the shape of a file descriptor is not worth a
program that can hang. An explicit `-` costs only six characters.

**The tour is embedded in the binary**, generated from `scenarios/tour.txt` at
configure time, so the demo works from any working directory. The file on disk
stays the single source of truth.

---

<a id="dd-045"></a>

## DD-045 — The synthetic feed also settles DD-041

**Decision:** the demo includes `--generate N`, which runs random flow around a
random-walking mid and reports throughput per chunk.

**Why per chunk rather than one total.** Fragmentation shows up as a trend, not a
level. A single number for the whole run would hide a slowdown.

**What it settled.** DD-041 reverted a pooled allocator because no benchmark
could detect any benefit, and the honest reason was that every benchmark builds
its book in one burst. The system allocator was already returning contiguous
nodes, so there was nothing to fix. That left an open question: would a
long-running book that churns levels for millions of orders fragment?

Two million orders, with the touch drifting and roughly 30–160 levels per side
being created and destroyed throughout:

```
        orders        ns/op      orders/sec   resting   levels(bid/ask)
        100000        176.3         5671506       517   46/27
        500000        168.5         5936289       426   77/35
       1000000        173.1         5776214       390   61/48
       2000000        176.4         5667917       269   54/32
```

Flat. No degradation across two million orders. **DD-041's revert now has
long-running evidence behind it, not just the absence of a benchmark.**

That is the second time a measurement has changed a decision in this project,
and both times the useful result was a negative one.

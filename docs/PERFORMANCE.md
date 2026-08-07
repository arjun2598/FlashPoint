# Performance Notes

> **Status: methodology only.** No benchmarks exist yet as the benchmark harness
> lands at Milestone 10.

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

- **Price level lookup.** A tree (`std::map`) is O(log n) with poor locality and
  a pointer chase per node; a sorted vector or a direct-indexed price ladder is
  cache-friendly but costs on insert or memory. Real books are dense near the
  touch and sparse in the tail, which favours a hybrid.
- **Order storage within a level.** FIFO priority means we only ever pop from
  the front and push to the back. An intrusive list over a pre-allocated slab
  gives O(1) cancel-by-id and no per-order allocation, at the cost of manual
  lifetime management. `std::deque` is far simpler but makes cancel-from-middle
  awkward.
- **Allocation on the hot path.** The target is zero allocation during steady-
  state matching. Anything else introduces an unbounded tail.
- **The common case is not a trade.** In real markets the overwhelming majority
  of messages are non-marketable adds and cancels. Optimising the crossing path
  while the add path chases pointers is the classic mistake.
- **Top-of-book access.** Best bid/ask is read on essentially every operation.
  It should be O(1) and ideally in a hot cache line.
- **`Order` is 32 bytes, and could plausibly be 24.** `Price::Rep` and
  `Quantity::Rep` are 64-bit. Tick counts and share quantities may fit
  comfortably in 32 bits for any realistic instrument, which would take the
  payload from 25 bytes to 17 and the object from 32 to 24, improving cache density and allowing
  more orders to fit in higher cache levels. However, the choice of 64-bit is intentional 
  here to allow a greater range, and latency should be measured under realistic order-book workloads 
  before reducing the representation size, since smaller objects improve cache locality but may not 
  materially affect end-to-end matching performance.

  Recorded here rather than acted on. The point worth noting is that because
  each type names its representation as a nested `Rep` alias, testing the
  hypothesis is a two-line change plus updating the `sizeof` assertion in
  `tests/order_test.cpp` — not a repo-wide edit. That cheapness is a deliberate
  payoff of the strong-type wrapper (DD-010) and is what makes it reasonable to
  defer the question instead of guessing now.

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

## Environment used for benchmarking

*To be recorded at Milestone 10.*

## Results

*Coming soon!*

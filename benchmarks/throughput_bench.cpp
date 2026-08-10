// Sustained throughput, measured with Google Benchmark.
//
// This answers "how many operations per second under saturation". Per-operation
// latency percentiles are a different question and are measured separately in
// latency_bench.cpp, because Google Benchmark reports mean and median rather
// than the tail (rule 2 and rule 3 in docs/PERFORMANCE.md).
//
// Each fixture is built once outside the timed loop. Scenarios that would drain
// the book are sized so they cannot, which avoids pausing the timer inside the
// loop.

#include "support.hpp"

#include "flashpoint/market_data.hpp"
#include "flashpoint/matching_engine.hpp"
#include "flashpoint/order.hpp"
#include "flashpoint/types.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <random>
#include <vector>

namespace flashpoint::bench {
namespace {

const BookShape& shape_for(int index) {
    return index == 0 ? kShallow : kDeep;
}

/// Add then immediately cancel, which is the steady-state shape of real quoting
/// flow and keeps the book from growing without bound.
///
/// There is deliberately no add-only throughput benchmark. Google Benchmark
/// chooses its own iteration count, and an add-only loop ran six million orders
/// into a book that started with five thousand, so it measured pool growth and
/// hash rehashing rather than the add path. Add in isolation is measured by the
/// latency harness instead, where the sample count is fixed and the pool is
/// reserved up front.
void bm_add_then_cancel(benchmark::State& state) {
    const BookShape& shape = shape_for(static_cast<int>(state.range(0)));

    MatchingEngine engine{shape.total_orders() * 2};
    OrderId::Rep next_id = 1;
    fill_side(engine, Side::Buy, shape, 100'000, next_id);

    std::mt19937 rng{11};
    std::uniform_int_distribution<Price::Rep> level{0, shape.levels - 1};

    for (auto _ : state) {
        const OrderId id{next_id++};
        auto added = engine.submit(
            Order::limit(id, Side::Buy, Price{100'000 - level(rng)}, Quantity{100}), discard);
        benchmark::DoNotOptimize(added);
        auto cancelled = engine.cancel(id, discard);
        benchmark::DoNotOptimize(cancelled);
    }
    state.SetItemsProcessed(state.iterations() * 2);
}

/// An aggressive order meeting a resting order that is always large enough to
/// absorb it, so the book is reduced rather than drained.
void bm_cross_one_level(benchmark::State& state) {
    const BookShape& shape = shape_for(static_cast<int>(state.range(0)));

    MatchingEngine engine{shape.total_orders() * 2};
    OrderId::Rep next_id = 1;

    // One resting sell per level, sized far beyond what the run can consume, so
    // every iteration is a partial fill against the same maker.
    for (Price::Rep level = 0; level < shape.levels; ++level) {
        auto result = engine.submit(Order::limit(OrderId{next_id++}, Side::Sell,
                                                 Price{100'000 + level}, Quantity{1'000'000'000}),
                                    discard);
        benchmark::DoNotOptimize(result);
    }

    for (auto _ : state) {
        auto result = engine.submit(Order::limit(OrderId{next_id++}, Side::Buy, Price{100'000},
                                                 Quantity{1}, TimeInForce::ImmediateOrCancel),
                                    discard);
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
}

void bm_read_top_of_book(benchmark::State& state) {
    const BookShape& shape = shape_for(static_cast<int>(state.range(0)));

    MatchingEngine engine{shape.total_orders() * 2};
    OrderId::Rep next_id = 1;
    fill_side(engine, Side::Buy, shape, 100'000, next_id);
    fill_side(engine, Side::Sell, shape, 100'001, next_id);

    for (auto _ : state) {
        TopOfBook top = engine.book().top_of_book();
        benchmark::DoNotOptimize(top);
    }
    state.SetItemsProcessed(state.iterations());
}

/// Best bid alone. `bids_` is ascending, so this is `rbegin()`.
void bm_read_best_bid(benchmark::State& state) {
    const BookShape& shape = shape_for(static_cast<int>(state.range(0)));
    MatchingEngine engine{shape.total_orders() * 2};
    OrderId::Rep next_id = 1;
    fill_side(engine, Side::Buy, shape, 100'000, next_id);

    for (auto _ : state) {
        auto best = engine.book().best_bid();
        benchmark::DoNotOptimize(best);
    }
    state.SetItemsProcessed(state.iterations());
}

/// Best ask alone. `asks_` is ascending, so this is `begin()`.
void bm_read_best_ask(benchmark::State& state) {
    const BookShape& shape = shape_for(static_cast<int>(state.range(0)));
    MatchingEngine engine{shape.total_orders() * 2};
    OrderId::Rep next_id = 1;
    fill_side(engine, Side::Sell, shape, 100'001, next_id);

    for (auto _ : state) {
        auto best = engine.book().best_ask();
        benchmark::DoNotOptimize(best);
    }
    state.SetItemsProcessed(state.iterations());
}

void bm_snapshot_ten_levels(benchmark::State& state) {
    const BookShape& shape = shape_for(static_cast<int>(state.range(0)));

    MatchingEngine engine{shape.total_orders() * 2};
    OrderId::Rep next_id = 1;
    fill_side(engine, Side::Buy, shape, 100'000, next_id);
    fill_side(engine, Side::Sell, shape, 100'001, next_id);

    std::array<LevelSnapshot, 10> rows{};

    for (auto _ : state) {
        std::size_t written = engine.book().snapshot(Side::Buy, rows);
        benchmark::DoNotOptimize(written);
        benchmark::DoNotOptimize(rows);
    }
    state.SetItemsProcessed(state.iterations());
}

// Argument 0 is the shallow book, 1 the deep one. Both hold the same number of
// orders, so the difference between them is the level count alone.
BENCHMARK(bm_add_then_cancel)->Arg(0)->Arg(1);
BENCHMARK(bm_cross_one_level)->Arg(0)->Arg(1);
BENCHMARK(bm_read_top_of_book)->Arg(0)->Arg(1);
BENCHMARK(bm_read_best_bid)->Arg(0)->Arg(1);
BENCHMARK(bm_read_best_ask)->Arg(0)->Arg(1);
BENCHMARK(bm_snapshot_ten_levels)->Arg(0)->Arg(1);

}  // namespace
}  // namespace flashpoint::bench

BENCHMARK_MAIN();

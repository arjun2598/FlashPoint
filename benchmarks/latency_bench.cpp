// Per-operation latency, reported as percentiles.
//
// Google Benchmark measures throughput well but reports mean and median. For a
// matching engine the tail is what matters, so the distribution is measured
// here instead, following rule 2 in docs/PERFORMANCE.md.
//
// Every scenario runs at two book shapes holding the same number of orders but
// differing 100x in price levels. Everything in the book that is not O(1) scales
// with the level count, so the gap between the two columns is the cost the
// Milestone 11 container change is meant to remove.

#include "support.hpp"

#include "flashpoint/market_data.hpp"
#include "flashpoint/matching_engine.hpp"
#include "flashpoint/order.hpp"
#include "flashpoint/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace flashpoint::bench {
namespace {

/// Wraps one timed operation. Kept in one place so every scenario measures the
/// same two clock reads around the same shape of call.
template <typename Operation>
void time_one(LatencyRecorder& recorder, Operation&& operation) {
    const auto start = Clock::now();
    operation();
    const auto end = Clock::now();
    recorder.record(static_cast<std::uint64_t>(std::chrono::nanoseconds{end - start}.count()));
}

// ---------------------------------------------------------------------------
// Scenarios
// ---------------------------------------------------------------------------

/// Adding an order that cannot trade. The most common message in real flow.
///
/// Only bids are populated, so nothing can cross and the measurement is the add
/// path alone. New orders reuse existing prices, so the level count stays fixed
/// while the queues grow.
LatencyStats add_non_marketable(const BookShape& shape, std::size_t samples) {
    MatchingEngine engine{shape.total_orders() + samples};
    OrderId::Rep next_id = 1;
    fill_side(engine, Side::Buy, shape, 100'000, next_id);

    LatencyRecorder recorder{samples};
    std::mt19937 rng{7};
    std::uniform_int_distribution<Price::Rep> level{0, shape.levels - 1};

    for (std::size_t i = 0; i < samples; ++i) {
        const Order order =
            Order::limit(OrderId{next_id++}, Side::Buy, Price{100'000 - level(rng)}, Quantity{100});
        time_one(recorder, [&] {
            const auto result = engine.submit(order, discard);
            do_not_optimize(result);
        });
    }
    return recorder.summarise();
}

/// Cancelling a resting order chosen at random.
///
/// Runs in blocks: fill the book, cancel a fraction of it, rebuild. Cancelling
/// everything would drain levels and shrink the level count part way through,
/// which would quietly change what is being measured.
LatencyStats cancel_random(const BookShape& shape, std::size_t samples) {
    LatencyRecorder recorder{samples};
    std::mt19937 rng{11};

    while (recorder.size() < samples) {
        MatchingEngine engine{shape.total_orders()};
        OrderId::Rep next_id = 1;
        std::vector<OrderId> resting = fill_side(engine, Side::Buy, shape, 100'000, next_id);
        std::shuffle(resting.begin(), resting.end(), rng);

        // Half the book, so every level keeps orders and the level count holds.
        const std::size_t budget = std::min(resting.size() / 2, samples - recorder.size());
        for (std::size_t i = 0; i < budget; ++i) {
            const OrderId id = resting[i];
            time_one(recorder, [&] {
                const auto result = engine.cancel(id, discard);
                do_not_optimize(result);
            });
        }
    }
    return recorder.summarise();
}

/// Adding an order at a price that does not yet exist, creating a new level.
///
/// Milestone 10 measured no scenario that created a level: every add reused a
/// price already in the book. This is the operation the candidate replacements
/// for the price-level container differ on most: a sorted vector has to shift
/// every element past the insertion point.
///
/// The new level is a new best bid: real flow moves the touch constantly, and it
/// is the worst position for any structure that keeps levels contiguous.
///
/// The previous new level is cancelled untimed before each measurement, so the
/// level count stays at the book shape rather than growing.
LatencyStats add_creating_level(const BookShape& shape, std::size_t samples) {
    MatchingEngine engine{shape.total_orders() + 16};
    OrderId::Rep next_id = 1;
    fill_side(engine, Side::Buy, shape, 100'000, next_id);

    LatencyRecorder recorder{samples};
    OrderId previous{};

    for (std::size_t i = 0; i < samples; ++i) {
        if (previous.is_valid()) {
            const auto removed = engine.cancel(previous, discard);
            do_not_optimize(removed);
        }

        // Above every existing bid, so this is always a new best level. The
        // price varies so no slot is reused from one iteration to the next.
        const auto offset = static_cast<Price::Rep>(i % 64);
        const OrderId id{next_id++};
        const Order order = Order::limit(id, Side::Buy, Price{100'001 + offset}, Quantity{100});

        time_one(recorder, [&] {
            const auto result = engine.submit(order, discard);
            do_not_optimize(result);
        });
        previous = id;
    }
    return recorder.summarise();
}

/// Cancelling the only order at a price, destroying the level.
///
/// The existing cancel scenario always leaves other orders behind, so it never
/// exercises the erase path. This one does.
LatencyStats cancel_destroying_level(const BookShape& shape, std::size_t samples) {
    MatchingEngine engine{shape.total_orders() + 16};
    OrderId::Rep next_id = 1;
    fill_side(engine, Side::Buy, shape, 100'000, next_id);

    LatencyRecorder recorder{samples};

    for (std::size_t i = 0; i < samples; ++i) {
        const auto offset = static_cast<Price::Rep>(i % 64);
        const OrderId id{next_id++};

        // Untimed: create a level holding exactly this one order.
        const auto added = engine.submit(
            Order::limit(id, Side::Buy, Price{100'001 + offset}, Quantity{100}), discard);
        do_not_optimize(added);

        time_one(recorder, [&] {
            const auto result = engine.cancel(id, discard);
            do_not_optimize(result);
        });
    }
    return recorder.summarise();
}

/// An aggressive order that consumes exactly one resting order.
LatencyStats cross_one_level(const BookShape& shape, std::size_t samples) {
    LatencyRecorder recorder{samples};

    while (recorder.size() < samples) {
        MatchingEngine engine{shape.total_orders()};
        OrderId::Rep next_id = 1;
        fill_side(engine, Side::Sell, shape, 100'000, next_id);

        const std::size_t budget = std::min(shape.total_orders() / 2, samples - recorder.size());
        for (std::size_t i = 0; i < budget; ++i) {
            // Priced at the far end of the book so it always crosses, sized to
            // take one resting order and no more.
            const Order order =
                Order::limit(OrderId{next_id++}, Side::Buy, Price{100'000 + shape.levels},
                             Quantity{100}, TimeInForce::ImmediateOrCancel);
            time_one(recorder, [&] {
                const auto result = engine.submit(order, discard);
                do_not_optimize(result);
            });
        }
    }
    return recorder.summarise();
}

/// An aggressive order large enough to consume several resting orders.
///
/// This is where the engine re-enters the book once per fill (DD-022), so it is
/// the scenario most sensitive to the cost of a level lookup.
LatencyStats sweep_many_orders(const BookShape& shape, std::size_t samples) {
    // int, so converting to either std::size_t or Quantity::Rep is a real cast
    // on every platform. Declaring it as one of those two would make the
    // conversion to the other useless on Linux, where they are the same type.
    constexpr int kOrdersConsumed = 10;
    LatencyRecorder recorder{samples};

    while (recorder.size() < samples) {
        MatchingEngine engine{shape.total_orders()};
        OrderId::Rep next_id = 1;
        fill_side(engine, Side::Sell, shape, 100'000, next_id);

        const std::size_t budget =
            std::min(shape.total_orders() / (2 * static_cast<std::size_t>(kOrdersConsumed)),
                     samples - recorder.size());
        for (std::size_t i = 0; i < budget; ++i) {
            const Order order =
                Order::limit(OrderId{next_id++}, Side::Buy, Price{100'000 + shape.levels},
                             Quantity{100 * static_cast<Quantity::Rep>(kOrdersConsumed)},
                             TimeInForce::ImmediateOrCancel);
            time_one(recorder, [&] {
                const auto result = engine.submit(order, discard);
                do_not_optimize(result);
            });
        }
    }
    return recorder.summarise();
}

/// Shrinking a resting order at the same price. The cheap path: no relinking and
/// no level change, but still one level lookup to update the cached aggregate.
///
/// The quantity must genuinely fall. Passing the same quantity back would skip
/// the reduce entirely and measure a lookup and an event, which is not what this
/// row claims to be.
///
/// The order's own price is used. A fixed price would make most iterations a
/// reprice, which is the priority-lost path, and would migrate the whole book
/// onto one level part way through the run.
LatencyStats modify_retained(const BookShape& shape, std::size_t samples) {
    LatencyRecorder recorder{samples};
    std::mt19937 rng{13};

    while (recorder.size() < samples) {
        MatchingEngine engine{shape.total_orders()};
        OrderId::Rep next_id = 1;
        const std::vector<OrderId> resting = fill_side(engine, Side::Buy, shape, 100'000, next_id);
        std::uniform_int_distribution<std::size_t> pick{0, resting.size() - 1};

        // Orders rest with 100 units, so the book supports this many reductions
        // before any of them would reach zero.
        const std::size_t budget = std::min(resting.size() * 50, samples - recorder.size());

        for (std::size_t i = 0; i < budget; ++i) {
            const OrderId id = resting[pick(rng)];
            const auto before = engine.book().resting_order(id);
            if (!before.has_value() || before->remaining <= Quantity{1}) {
                continue;
            }
            const Price price = before->price;
            const Quantity smaller = before->remaining - Quantity{1};
            time_one(recorder, [&] {
                const auto result = engine.modify(id, price, smaller, discard);
                do_not_optimize(result);
            });
        }
    }
    return recorder.summarise();
}

/// Growing a resting order. The expensive path: remove, then re-add at the back.
LatencyStats modify_lost(const BookShape& shape, std::size_t samples) {
    LatencyRecorder recorder{samples};
    std::mt19937 rng{17};

    while (recorder.size() < samples) {
        MatchingEngine engine{shape.total_orders()};
        OrderId::Rep next_id = 1;
        std::vector<OrderId> resting = fill_side(engine, Side::Buy, shape, 100'000, next_id);
        std::shuffle(resting.begin(), resting.end(), rng);

        const std::size_t budget = std::min(resting.size(), samples - recorder.size());
        for (std::size_t i = 0; i < budget; ++i) {
            const OrderId id = resting[i];
            const auto before = engine.book().resting_order(id);
            if (!before.has_value()) {
                continue;
            }
            const Price price = before->price;
            const Quantity larger = before->remaining + Quantity{1};
            time_one(recorder, [&] {
                const auto result = engine.modify(id, price, larger, discard);
                do_not_optimize(result);
            });
        }
    }
    return recorder.summarise();
}

/// Reading best bid and ask. Done on nearly every decision a trading system
/// makes, so it needs to be close to free.
LatencyStats read_top_of_book(const BookShape& shape, std::size_t samples) {
    MatchingEngine engine{shape.total_orders() * 2};
    OrderId::Rep next_id = 1;
    fill_side(engine, Side::Buy, shape, 100'000, next_id);
    fill_side(engine, Side::Sell, shape, 100'001, next_id);

    LatencyRecorder recorder{samples};
    for (std::size_t i = 0; i < samples; ++i) {
        time_one(recorder, [&] {
            const TopOfBook top = engine.book().top_of_book();
            do_not_optimize(top);
        });
    }
    return recorder.summarise();
}

/// Publishing ten levels of depth on one side.
LatencyStats snapshot_ten_levels(const BookShape& shape, std::size_t samples) {
    MatchingEngine engine{shape.total_orders() * 2};
    OrderId::Rep next_id = 1;
    fill_side(engine, Side::Buy, shape, 100'000, next_id);
    fill_side(engine, Side::Sell, shape, 100'001, next_id);

    std::array<LevelSnapshot, 10> rows{};
    LatencyRecorder recorder{samples};
    for (std::size_t i = 0; i < samples; ++i) {
        time_one(recorder, [&] {
            const std::size_t written = engine.book().snapshot(Side::Buy, rows);
            do_not_optimize(written);
            do_not_optimize(rows);
        });
    }
    return recorder.summarise();
}

}  // namespace
}  // namespace flashpoint::bench

int main() {
    using namespace flashpoint::bench;

    constexpr std::size_t kSamples = 200'000;
    constexpr std::size_t kSweepSamples = 50'000;

    std::printf("FlashPoint latency benchmarks\n");
    std::printf("All figures in nanoseconds. Percentiles are nearest-rank, so every\n");
    std::printf("number shown is a measurement that actually occurred.\n");

    const LatencyStats overhead = measure_clock_overhead(kSamples);
    print_header("Instrumentation cost (an empty timed region)");
    print_row("two steady_clock reads, nothing between", overhead);
    std::printf("\nSubtract roughly the p50 above from every figure below to separate\n");
    std::printf("the engine from the instrument measuring it.\n");

    for (const BookShape& shape : {kShallow, kDeep}) {
        print_header(std::string{"Book: "} + std::string{shape.name} + "  (" +
                     std::to_string(shape.total_orders()) + " resting orders)");

        print_row("add, non-marketable", add_non_marketable(shape, kSamples));
        print_row("add, creating a new best level", add_creating_level(shape, kSamples));
        print_row("cancel, random resting order", cancel_random(shape, kSamples));
        print_row("cancel, destroying a level", cancel_destroying_level(shape, kSamples));
        print_row("submit, crosses one resting order", cross_one_level(shape, kSamples));
        print_row("submit, sweeps ten resting orders", sweep_many_orders(shape, kSweepSamples));
        print_row("modify, priority retained", modify_retained(shape, kSamples));
        print_row("modify, priority lost", modify_lost(shape, kSamples));
        print_row("read top of book", read_top_of_book(shape, kSamples));
        print_row("snapshot, ten levels", snapshot_ten_levels(shape, kSamples));
    }

    std::printf("\n");
    return 0;
}

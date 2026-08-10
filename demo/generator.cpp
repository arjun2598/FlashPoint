#include "generator.hpp"

#include "render.hpp"

#include "flashpoint/event.hpp"
#include "flashpoint/matching_engine.hpp"
#include "flashpoint/order.hpp"
#include "flashpoint/types.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <ostream>
#include <random>
#include <span>
#include <vector>

namespace flashpoint::demo {
namespace {

/// Counts events, and optionally keeps the most recent executions.
class Collector {
public:
    explicit Collector(std::size_t keep_trades) : keep_(keep_trades) {}

    void operator()(const Event& event) {
        switch (event.type) {
            case EventType::Accepted:
                ++tally_.accepted;
                break;
            case EventType::Trade:
                ++tally_.trades;
                tally_.volume += event.quantity;
                if (keep_ > 0) {
                    recent_.push_back(to_trade(event));
                    if (recent_.size() > keep_) {
                        recent_.pop_front();
                    }
                }
                break;
            case EventType::Cancelled:
                ++tally_.cancelled;
                break;
            case EventType::Modified:
                ++tally_.modified;
                break;
            case EventType::Rejected:
                ++tally_.rejected;
                break;
        }
    }

    [[nodiscard]] const Tally& tally() const noexcept {
        return tally_;
    }

    [[nodiscard]] std::vector<Trade> recent() const {
        return {recent_.begin(), recent_.end()};
    }

private:
    std::size_t keep_;
    Tally tally_;
    std::deque<Trade> recent_;
};

/// Copies out the top levels of one side.
[[nodiscard]] std::vector<LevelSnapshot> take_levels(const OrderBook& book, Side side,
                                                     std::size_t depth) {
    std::vector<LevelSnapshot> rows(depth);
    const std::size_t written = book.snapshot(side, std::span<LevelSnapshot>{rows});
    rows.resize(written);
    return rows;
}

}  // namespace

GeneratorResult run_generator(const GeneratorConfig& config) {
    // A mid that random-walks, with orders placed around it. This is what churns
    // levels: as the mid drifts, levels near the old touch drain and new ones
    // appear ahead of it.
    Price::Rep mid = 100'000;

    std::mt19937 rng{config.seed};
    std::uniform_int_distribution<int> action{0, 99};
    std::uniform_int_distribution<Price::Rep> offset{0, 20};
    std::uniform_int_distribution<Price::Rep> drift{-1, 1};
    std::uniform_int_distribution<Quantity::Rep> size{1, 500};

    MatchingEngine engine{EngineConfig{config.protection_ticks}, config.orders / 8 + 1024};
    Collector collector{config.keep_trades};

    std::vector<OrderId> resting;
    resting.reserve(config.orders / 4 + 1);
    OrderId::Rep next_id = 1;

    GeneratorResult result;

    while (result.submitted < config.orders) {
        const std::size_t chunk = std::min(config.chunk, config.orders - result.submitted);
        const auto start = std::chrono::steady_clock::now();

        for (std::size_t i = 0; i < chunk; ++i) {
            mid += drift(rng);
            const int roll = action(rng);

            // Cancels are the most common message in real flow after passive
            // adds, so they get a large share here too.
            if (roll < 35 && !resting.empty()) {
                std::uniform_int_distribution<std::size_t> pick{0, resting.size() - 1};
                const std::size_t slot = pick(rng);
                const OrderId id = resting[slot];
                resting[slot] = resting.back();
                resting.pop_back();
                const auto cancelled = engine.cancel(id, collector);
                static_cast<void>(cancelled);
            } else if (roll < 45 && !resting.empty()) {
                std::uniform_int_distribution<std::size_t> pick{0, resting.size() - 1};
                const std::size_t slot = pick(rng);
                const OrderId id = resting[slot];
                const auto current = engine.book().resting_order(id);
                if (current.has_value()) {
                    const auto modified =
                        engine.modify(id, current->price, Quantity{size(rng)}, collector);
                    static_cast<void>(modified);
                } else {
                    // Traded away since it was submitted. Drop it, or the list
                    // fills with dead ids and a growing share of the run is
                    // cancels and modifies that can only be rejected.
                    resting[slot] = resting.back();
                    resting.pop_back();
                }
            } else {
                const bool buying = (roll % 2) == 0;
                const Side side = buying ? Side::Buy : Side::Sell;

                // Most orders sit away from the touch and rest; a minority
                // reach across it and trade.
                const bool aggressive = roll >= 90;
                const Price::Rep away = aggressive ? -offset(rng) : offset(rng);
                const Price price{buying ? mid - away : mid + away};

                const OrderId id{next_id++};
                const auto submitted =
                    engine.submit(Order::limit(id, side, price, Quantity{size(rng)}), collector);
                if (submitted.resting > Quantity{}) {
                    resting.push_back(id);
                }
            }
        }

        const auto end = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration<double>{end - start}.count();
        result.submitted += chunk;

        // A chunk can complete faster than the clock's resolution on a very
        // small run, so guard the division rather than reporting infinity.
        const double safe_elapsed = elapsed > 0 ? elapsed : 1e-9;

        result.chunks.push_back(ChunkStats{
            result.submitted, safe_elapsed * 1e9 / static_cast<double>(chunk),
            static_cast<double>(chunk) / safe_elapsed, engine.book().size(),
            engine.book().level_count(Side::Buy), engine.book().level_count(Side::Sell)});
    }

    result.tally = collector.tally();
    result.tally.sequences = engine.last_sequence();
    result.recent_trades = collector.recent();
    result.bids = take_levels(engine.book(), Side::Buy, 10);
    result.asks = take_levels(engine.book(), Side::Sell, 10);
    result.top = engine.book().top_of_book();

    return result;
}

void print_generator_result(std::ostream& out, const GeneratorConfig& config,
                            const GeneratorResult& result) {
    out << "Generating " << config.orders << " orders, seed " << config.seed << ".\n";
    out << "Throughput is reported per chunk so a trend across the run is visible.\n\n";
    out << "        orders        ns/op      orders/sec   resting   levels(bid/ask)\n";
    out << "  ------------------------------------------------------------------------\n";

    for (const ChunkStats& chunk : result.chunks) {
        out << "  " << std::setw(12) << chunk.orders_so_far << "  " << std::setw(11) << std::fixed
            << std::setprecision(1) << chunk.nanoseconds_per_order << "  " << std::setw(14)
            << std::setprecision(0) << chunk.orders_per_second << "  " << std::setw(8)
            << chunk.resting << "   " << chunk.bid_levels << '/' << chunk.ask_levels << '\n';
    }

    out << "\nEvents published\n";
    out << "  accepted   " << result.tally.accepted << '\n';
    out << "  trades     " << result.tally.trades << "   (" << result.tally.volume.value()
        << " units)\n";
    out << "  cancelled  " << result.tally.cancelled << '\n';
    out << "  modified   " << result.tally.modified << '\n';
    out << "  rejected   " << result.tally.rejected << '\n';
    out << "  total      " << result.tally.sequences.value() << " sequence numbers issued\n";

    print_levels(out, result.bids, result.asks, result.top);
}

}  // namespace flashpoint::demo

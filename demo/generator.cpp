#include "generator.hpp"

#include "render.hpp"

#include "flashpoint/event.hpp"
#include "flashpoint/matching_engine.hpp"
#include "flashpoint/order.hpp"
#include "flashpoint/types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <random>
#include <vector>

namespace flashpoint::demo {
namespace {

/// Counts what the engine did, without storing anything.
struct Tally {
    std::uint64_t accepted = 0;
    std::uint64_t trades = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t modified = 0;
    std::uint64_t rejected = 0;
    Quantity volume{};

    void operator()(const Event& event) {
        switch (event.type) {
            case EventType::Accepted:
                ++accepted;
                break;
            case EventType::Trade:
                ++trades;
                volume += event.quantity;
                break;
            case EventType::Cancelled:
                ++cancelled;
                break;
            case EventType::Modified:
                ++modified;
                break;
            case EventType::Rejected:
                ++rejected;
                break;
        }
    }
};

}  // namespace

std::size_t run_generator(std::ostream& out, const GeneratorConfig& config) {
    // A mid that random-walks, with orders placed around it. This is what churns
    // levels: as the mid drifts, levels near the old touch drain and new ones
    // appear ahead of it.
    Price::Rep mid = 100'000;

    std::mt19937 rng{config.seed};
    std::uniform_int_distribution<int> action{0, 99};
    std::uniform_int_distribution<Price::Rep> offset{0, 20};
    std::uniform_int_distribution<Price::Rep> drift{-1, 1};
    std::uniform_int_distribution<Quantity::Rep> size{1, 500};

    MatchingEngine engine{config.orders / 8 + 1024};
    Tally tally;

    std::vector<OrderId> resting;
    resting.reserve(config.orders / 4);
    OrderId::Rep next_id = 1;

    out << "Generating " << config.orders << " orders, seed " << config.seed << ".\n";
    out << "Throughput is reported per chunk so a trend across the run is visible.\n\n";
    out << "        orders        ns/op      orders/sec   resting   levels(bid/ask)\n";
    out << "  ------------------------------------------------------------------------\n";

    std::size_t submitted = 0;
    while (submitted < config.orders) {
        const std::size_t chunk = std::min(config.chunk, config.orders - submitted);
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
                const auto result = engine.cancel(id, tally);
                static_cast<void>(result);
            } else if (roll < 45 && !resting.empty()) {
                std::uniform_int_distribution<std::size_t> pick{0, resting.size() - 1};
                const std::size_t slot = pick(rng);
                const OrderId id = resting[slot];
                const auto current = engine.book().resting_order(id);
                if (current.has_value()) {
                    const auto result =
                        engine.modify(id, current->price, Quantity{size(rng)}, tally);
                    static_cast<void>(result);
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
                const auto result =
                    engine.submit(Order::limit(id, side, price, Quantity{size(rng)}), tally);
                if (result.resting > Quantity{}) {
                    resting.push_back(id);
                }
            }
        }

        const auto end = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration<double>{end - start}.count();
        submitted += chunk;

        const double per_op = elapsed * 1e9 / static_cast<double>(chunk);
        const double rate = static_cast<double>(chunk) / elapsed;

        out << "  ";
        out.width(12);
        out << submitted << "  ";
        out.width(11);
        out.precision(1);
        out << std::fixed << per_op << "  ";
        out.width(14);
        out.precision(0);
        out << rate << "  ";
        out.width(8);
        out << engine.book().size() << "   ";
        out << engine.book().level_count(Side::Buy) << '/' << engine.book().level_count(Side::Sell)
            << '\n';
    }

    out << "\nEvents published\n";
    out << "  accepted   " << tally.accepted << '\n';
    out << "  trades     " << tally.trades << "   (" << tally.volume.value() << " units)\n";
    out << "  cancelled  " << tally.cancelled << '\n';
    out << "  modified   " << tally.modified << '\n';
    out << "  rejected   " << tally.rejected << '\n';
    out << "  total      " << engine.last_sequence().value() << " sequence numbers issued\n";

    print_book(out, engine.book(), 8);

    return submitted;
}

}  // namespace flashpoint::demo

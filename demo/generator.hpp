// A synthetic order feed, for running the engine at volume.
//
// Two purposes. It shows the demo doing real work rather than a scripted dozen
// orders, and it is the long-running level-churning workload the benchmarks do
// not provide.
//
// That second purpose matters: DD-041 reverted a pooled allocator because the
// benchmarks could not detect any benefit, and the reason was that each
// benchmark builds its book in one burst. This runs for millions of orders with
// the touch moving constantly, so levels are created and destroyed throughout.
// Reporting throughput per chunk makes any fragmentation visible as a slowdown
// over the run.
//
// The simulation and the reporting are separate so the terminal demo and the
// browser build can share the first and differ in the second.

#pragma once

#include "flashpoint/market_data.hpp"
#include "flashpoint/trade.hpp"
#include "flashpoint/types.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <vector>

namespace flashpoint::demo {

struct GeneratorConfig {
    std::size_t orders = 1'000'000;
    std::uint32_t seed = 1;

    /// How many orders per timing chunk. Throughput is reported per chunk so a
    /// trend across the run is visible.
    std::size_t chunk = 100'000;

    /// How far past the opposite touch a market order may trade, in ticks.
    Price::Rep protection_ticks = 10;

    /// How many of the most recent executions to keep for display. Zero keeps
    /// none, which is what a throughput run wants.
    std::size_t keep_trades = 0;
};

/// Throughput over one block of orders.
struct ChunkStats {
    std::size_t orders_so_far = 0;
    double nanoseconds_per_order = 0;
    double orders_per_second = 0;
    std::size_t resting = 0;
    std::size_t bid_levels = 0;
    std::size_t ask_levels = 0;
};

/// What the engine did, counted rather than stored.
struct Tally {
    std::uint64_t accepted = 0;
    std::uint64_t trades = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t modified = 0;
    std::uint64_t rejected = 0;
    Quantity volume{};
    SequenceNumber sequences{};
};

struct GeneratorResult {
    std::size_t submitted = 0;
    std::vector<ChunkStats> chunks;
    Tally tally;

    /// The most recent executions, oldest first. Empty unless `keep_trades` was
    /// set, because a two-million-order run would otherwise retain millions.
    std::vector<Trade> recent_trades;

    /// The book as it stood at the end.
    std::vector<LevelSnapshot> bids;
    std::vector<LevelSnapshot> asks;
    TopOfBook top;
};

/// Runs the feed. Prints nothing.
[[nodiscard]] GeneratorResult run_generator(const GeneratorConfig& config);

/// Renders a result to a terminal.
void print_generator_result(std::ostream& out, const GeneratorConfig& config,
                            const GeneratorResult& result);

}  // namespace flashpoint::demo

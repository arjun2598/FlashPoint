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

#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>

namespace flashpoint::demo {

struct GeneratorConfig {
    std::size_t orders = 1'000'000;
    std::uint32_t seed = 1;

    /// How many orders per timing chunk. Throughput is reported per chunk so a
    /// trend across the run is visible.
    std::size_t chunk = 100'000;
};

/// Runs the feed and prints a report. Returns the number of orders submitted.
std::size_t run_generator(std::ostream& out, const GeneratorConfig& config);

}  // namespace flashpoint::demo

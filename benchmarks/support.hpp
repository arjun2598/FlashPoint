// Shared helpers for the benchmarks: workload construction, latency recording,
// and percentile reporting.

#pragma once

#include "flashpoint/event.hpp"
#include "flashpoint/matching_engine.hpp"
#include "flashpoint/order.hpp"
#include "flashpoint/types.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace flashpoint::bench {

/// Stops the compiler from deleting work whose result is never used.
///
/// Without this, a benchmark that computes something and throws it away can be
/// optimised down to nothing, and the timing measures an empty loop.
template <typename T>
inline void do_not_optimize(const T& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}

inline void discard(const Event&) {}

// ---------------------------------------------------------------------------
// Workloads
// ---------------------------------------------------------------------------

/// How a book is shaped for a run.
///
/// The two shapes used below hold the same number of orders but differ in the
/// number of distinct price levels by a factor of 100. Everything in the book
/// that is not O(1) scales with the level count, so comparing the two isolates
/// exactly the cost Milestone 11 is meant to remove.
struct BookShape {
    std::string_view name;
    Price::Rep levels;
    Quantity::Rep orders_per_level;

    [[nodiscard]] std::size_t total_orders() const {
        return static_cast<std::size_t>(levels) * static_cast<std::size_t>(orders_per_level);
    }
};

inline constexpr BookShape kShallow{"shallow (10 levels)", 10, 500};
inline constexpr BookShape kDeep{"deep (1000 levels)", 1000, 5};

/// Fills one side of the book, returning the ids that are now resting.
///
/// Bids run downwards from `base`, asks upwards, so the two never cross. Only
/// one side is populated in most scenarios, which keeps a measurement of the add
/// path free of any matching work.
inline std::vector<OrderId> fill_side(MatchingEngine& engine, Side side, const BookShape& shape,
                                      Price::Rep base, OrderId::Rep& next_id) {
    std::vector<OrderId> ids;
    ids.reserve(shape.total_orders());

    for (Price::Rep level = 0; level < shape.levels; ++level) {
        const Price price{side == Side::Buy ? base - level : base + level};
        for (Quantity::Rep i = 0; i < shape.orders_per_level; ++i) {
            const OrderId id{next_id++};
            const auto result =
                engine.submit(Order::limit(id, side, price, Quantity{100}), discard);
            if (result.resting > Quantity{}) {
                ids.push_back(id);
            }
        }
    }
    return ids;
}

// ---------------------------------------------------------------------------
// Latency recording
// ---------------------------------------------------------------------------

using Clock = std::chrono::steady_clock;

struct LatencyStats {
    std::size_t samples = 0;
    double min = 0;
    double p50 = 0;
    double p90 = 0;
    double p99 = 0;
    double p999 = 0;
    double max = 0;
    double mean = 0;
};

/// Collects one timing per operation, then reports the distribution.
///
/// The vector is reserved up front so recording a sample never allocates. An
/// allocation inside the measured loop would show up as a tail-latency spike
/// that belongs to the harness rather than the engine.
class LatencyRecorder {
public:
    explicit LatencyRecorder(std::size_t expected_samples) {
        samples_.reserve(expected_samples);
    }

    void record(std::uint64_t nanoseconds) {
        samples_.push_back(nanoseconds);
    }

    [[nodiscard]] std::size_t size() const {
        return samples_.size();
    }

    /// Sorts and summarises. Destructive, so call once.
    [[nodiscard]] LatencyStats summarise() {
        LatencyStats stats;
        if (samples_.empty()) {
            return stats;
        }

        std::sort(samples_.begin(), samples_.end());

        const auto total = std::accumulate(samples_.begin(), samples_.end(), std::uint64_t{0});

        stats.samples = samples_.size();
        stats.min = static_cast<double>(samples_.front());
        stats.max = static_cast<double>(samples_.back());
        stats.mean = static_cast<double>(total) / static_cast<double>(samples_.size());
        stats.p50 = percentile(0.50);
        stats.p90 = percentile(0.90);
        stats.p99 = percentile(0.99);
        stats.p999 = percentile(0.999);
        return stats;
    }

private:
    /// Nearest-rank percentile: the smallest value at or below which the given
    /// fraction of samples fall. No interpolation, so every number reported is a
    /// measurement that actually happened.
    [[nodiscard]] double percentile(double fraction) const {
        const auto count = static_cast<double>(samples_.size());
        const auto rank = static_cast<std::size_t>(std::ceil(fraction * count));
        const std::size_t index = rank == 0 ? 0 : std::min(rank - 1, samples_.size() - 1);
        return static_cast<double>(samples_[index]);
    }

    std::vector<std::uint64_t> samples_;
};

/// Measures what one timed region costs when it contains nothing.
///
/// Every latency below includes two clock reads. Reporting this separately lets
/// a reader judge how much of a small number is the engine and how much is the
/// instrument measuring it.
[[nodiscard]] inline LatencyStats measure_clock_overhead(std::size_t samples) {
    LatencyRecorder recorder{samples};
    for (std::size_t i = 0; i < samples; ++i) {
        const auto start = Clock::now();
        const auto end = Clock::now();
        recorder.record(static_cast<std::uint64_t>(std::chrono::nanoseconds{end - start}.count()));
    }
    return recorder.summarise();
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

inline void print_header(std::string_view title) {
    std::printf("\n%s\n", std::string(title).c_str());
    std::printf("%-44s %9s %8s %8s %8s %8s %9s %9s\n", "scenario", "samples", "min", "p50", "p90",
                "p99", "p99.9", "max");
    std::printf("%s\n", std::string(112, '-').c_str());
}

inline void print_row(std::string_view scenario, const LatencyStats& stats) {
    std::printf("%-44s %9zu %8.0f %8.0f %8.0f %8.0f %9.0f %9.0f\n", std::string(scenario).c_str(),
                stats.samples, stats.min, stats.p50, stats.p90, stats.p99, stats.p999, stats.max);
}

}  // namespace flashpoint::bench

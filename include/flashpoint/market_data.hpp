// Aggregated views of the book, for publishing rather than matching.

#pragma once

#include "flashpoint/types.hpp"

#include <cstdint>
#include <optional>
#include <type_traits>

namespace flashpoint {

/// One row of a depth-of-market view: everything resting at one price.
///
/// Individual orders are not visible here. Three orders totalling 30 at price
/// 100 produce a single row of 30, with an order count of 3. That is what "level
/// 2" means, as opposed to level 3 where each order is shown separately.
struct LevelSnapshot {
    Price price{};
    Quantity quantity{};
    std::uint32_t order_count{};

    friend constexpr bool operator==(const LevelSnapshot&, const LevelSnapshot&) noexcept = default;
};

static_assert(std::is_trivially_copyable_v<LevelSnapshot>);

/// The best price on each side, with the quantity resting there.
///
/// Read on almost every decision a trading system makes, so it is one value
/// rather than four separate calls into the book.
struct TopOfBook {
    /// Highest bid, or nullopt when there are no bids.
    std::optional<Price> bid_price{};

    /// Quantity resting at `bid_price`. Zero when there is no bid.
    Quantity bid_quantity{};

    /// Lowest ask, or nullopt when there are no asks.
    std::optional<Price> ask_price{};

    /// Quantity resting at `ask_price`. Zero when there is no ask.
    Quantity ask_quantity{};

    [[nodiscard]] constexpr bool has_bid() const noexcept {
        return bid_price.has_value();
    }

    [[nodiscard]] constexpr bool has_ask() const noexcept {
        return ask_price.has_value();
    }

    /// Ask minus bid, or nullopt unless both sides are present.
    ///
    /// Returned in ticks. Converting to a currency amount needs the instrument's
    /// tick size, which lives at the presentation edge (DD-009).
    [[nodiscard]] constexpr std::optional<Price::Rep> spread() const noexcept {
        if (!has_bid() || !has_ask()) {
            return std::nullopt;
        }
        return ask_price->ticks() - bid_price->ticks();
    }

    friend constexpr bool operator==(const TopOfBook&, const TopOfBook&) noexcept = default;
};

}  // namespace flashpoint

// The matching engine.
//
// Header-only, deliberately. `submit` is templated on the trade sink so that
// delivering a trade is a direct call the compiler can inline, with no
// allocation and no indirect dispatch. That is a considered exception to DD-002
// (orchestration belongs in a translation unit): this is the hottest code in the
// project, and a template cannot live anywhere else.

#pragma once

#include "flashpoint/order.hpp"
#include "flashpoint/order_book.hpp"
#include "flashpoint/trade.hpp"
#include "flashpoint/types.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace flashpoint {

/// Outcome of submitting an order.
enum class SubmitStatus : std::uint8_t {
    /// The order was processed. It may have fully filled, partially filled and
    /// rested, or rested untouched.
    Accepted,

    /// The order was malformed: no id, zero quantity, or a side outside the
    /// enumerators.
    RejectedInvalid,

    /// An order with this id is already resting.
    RejectedDuplicateId,
};

[[nodiscard]] constexpr std::string_view to_string(SubmitStatus status) noexcept {
    switch (status) {
        case SubmitStatus::Accepted:
            return "Accepted";
        case SubmitStatus::RejectedInvalid:
            return "RejectedInvalid";
        case SubmitStatus::RejectedDuplicateId:
            return "RejectedDuplicateId";
    }
    return "Unknown";
}

/// What happened to a submitted order.
///
/// `filled + resting` always equals the submitted quantity for an accepted
/// order; both are zero for a rejected one. That identity is the cheapest
/// available check that no quantity was invented or lost, and the tests assert
/// it on every submission.
struct SubmitResult {
    SubmitStatus status{SubmitStatus::Accepted};

    /// Total quantity executed across every trade this order produced.
    Quantity filled{};

    /// Quantity left resting in the book. Zero if the order fully filled.
    Quantity resting{};

    [[nodiscard]] constexpr bool accepted() const noexcept {
        return status == SubmitStatus::Accepted;
    }

    friend constexpr bool operator==(const SubmitResult&, const SubmitResult&) noexcept = default;
};

/// Applies incoming orders to a single instrument's book at price-time priority.
///
/// The engine owns its book. It is the only component that may leave the book
/// in a crossed state, but it never does: a submitted order is matched against
/// the opposing side until it can no longer trade, and only then does the
/// remainder rest. That is why `OrderBook` itself contains no crossing check:
/// a container cannot resolve a cross, because resolving one means producing a
/// trade.
///
/// Handles plain limit orders only. Every unfilled remainder rests.
class MatchingEngine {
public:
    MatchingEngine() = default;

    /// Pre-sizes the underlying book for `expected_orders` resting orders.
    explicit MatchingEngine(std::size_t expected_orders) : book_(expected_orders) {}

    /// Submits a limit order.
    ///
    /// `on_trade` is invoked once per execution, in the order the executions
    /// occur, with a `const Trade&`. It is called before the corresponding
    /// resting order is retired or reduced, but callers should not depend on
    /// book state during the callback.
    ///
    /// A non-marketable order calls `on_trade` zero times, which costs nothing here.
    template <typename TradeSink>
    [[nodiscard]] SubmitResult submit(const Order& order, TradeSink&& on_trade) {
        // The boundary check DD-012 promised. Everything downstream, including
        // OrderBook::add, which asserts validity as a precondition, may assume
        // an order that reaches it is well formed.
        if (!order.is_valid()) {
            return SubmitResult{SubmitStatus::RejectedInvalid, Quantity{}, Quantity{}};
        }
        if (book_.contains(order.id())) {
            return SubmitResult{SubmitStatus::RejectedDuplicateId, Quantity{}, Quantity{}};
        }

        Quantity remaining = order.quantity();
        Quantity filled{};

        while (remaining > Quantity{}) {
            const std::optional<Price> touch = best_opposing(order.side());
            if (!touch.has_value() || !crosses(order.side(), order.price(), *touch)) {
                break;
            }

            const std::optional<OrderId> maker = book_.front_at(opposite(order.side()), *touch);
            assert(maker.has_value() && "a level exists at the touch but holds no orders");

            const std::optional<Quantity> maker_remaining = book_.remaining_of(*maker);
            assert(maker_remaining.has_value() && "front_at returned an id the book does not hold");

            // Both sides are strictly positive here as the book never retains a
            // zero-quantity order, so the fill is too and the loop always
            // makes progress.
            const Quantity fill = std::min(remaining, *maker_remaining);
            assert(fill > Quantity{} && "a zero-quantity fill would loop forever");

            on_trade(Trade{
                .maker_id = *maker,
                .taker_id = order.id(),
                .price = *touch,  // the maker's price: improvement goes to the taker
                .quantity = fill,
                .aggressor = order.side(),
            });

            if (fill == *maker_remaining) {
                [[maybe_unused]] const bool retired = book_.remove(*maker);
                assert(retired);
            } else {
                // Partial fill: the maker keeps its place in the queue.
                [[maybe_unused]] const bool reduced = book_.reduce(*maker, fill);
                assert(reduced);
            }

            remaining -= fill;
            filled += fill;
        }

        if (remaining > Quantity{}) {
            [[maybe_unused]] const bool rested =
                book_.add(Order{order.id(), order.side(), order.price(), remaining});
            assert(rested && "the id was checked as absent on entry");
        }

        return SubmitResult{SubmitStatus::Accepted, filled, remaining};
    }

    /// Read-only view of the book. Its own interface is handle-based (DD-018),
    /// so handing out a const reference exposes nothing a caller could depend on.
    [[nodiscard]] const OrderBook& book() const noexcept {
        return book_;
    }

private:
    /// The best price on the side an aggressor of `side` trades against: a buy
    /// lifts the lowest ask, a sell hits the highest bid.
    [[nodiscard]] std::optional<Price> best_opposing(Side side) const {
        return side == Side::Buy ? book_.best_ask() : book_.best_bid();
    }

    /// Whether an aggressor limited at `limit` can trade against `resting`.
    ///
    /// A buyer will pay up to its limit, so it crosses any ask at or below it. A
    /// seller will accept down to its limit, so it crosses any bid at or above
    /// it. Equality trades in both directions.
    [[nodiscard]] static constexpr bool crosses(Side aggressor, Price limit,
                                                Price resting) noexcept {
        return aggressor == Side::Buy ? resting <= limit : resting >= limit;
    }

    OrderBook book_;
};

}  // namespace flashpoint

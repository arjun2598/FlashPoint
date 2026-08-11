#pragma once

#include "flashpoint/event.hpp"
#include "flashpoint/types.hpp"

#include <cassert>
#include <type_traits>

namespace flashpoint {

/// A single execution: one resting order matched against one incoming order.
/// A sweep that fills against four resting orders produces four Trades, not one.
///
/// A plain aggregate rather than a class with a constructor. `maker_id` and
/// `taker_id` are both `OrderId`, so a positional constructor would let them be
/// transposed silently, which strong types cannot catch when both arguments are
/// the same type. Designated initialisers put the field name at the call site:
///
///     Trade{.maker_id = resting, .taker_id = incoming, ...}
///
/// (DD-021).
struct Trade {
    /// The order that was already resting and provided liquidity.
    OrderId maker_id{};

    /// The incoming order that crossed the spread and took liquidity.
    OrderId taker_id{};

    /// The execution price, which is always the maker's price. Price
    /// improvement goes to the aggressor: a buy limit at 105 lifting an ask
    /// resting at 100 trades at 100.
    Price price{};

    /// Quantity executed: the smaller of what each side had remaining.
    Quantity quantity{};

    /// Which side initiated. The engine knows this for free and a tape consumer
    /// cannot reconstruct it, so recording it costs 8 bytes (after padding) and
    /// saves the downstream from guessing. A run of buyer-aggressed prints is
    /// buying pressure; without this field that signal is simply absent.
    Side aggressor{};

    friend constexpr bool operator==(const Trade&, const Trade&) noexcept = default;
};

static_assert(std::is_trivially_copyable_v<Trade>,
              "Trade must stay trivially copyable: it is produced on the matching hot path and "
              "handed to a sink by value.");

static_assert(std::is_aggregate_v<Trade>,
              "Trade must remain an aggregate so that designated initialisers keep guarding the "
              "two adjacent OrderId fields (DD-021).");

/// Extracts a Trade from a Trade event.
///
/// The engine publishes executions as `Event`s so the whole stream is one record
/// type. This rebuilds the narrower value for anyone who only cares about the
/// tape, such as a market data consumer.
///
/// Precondition: `event.type == EventType::Trade`.
[[nodiscard]] constexpr Trade to_trade(const Event& event) noexcept {
    assert(event.type == EventType::Trade && "to_trade requires a Trade event");
    return Trade{.maker_id = event.counterparty_id,
                 .taker_id = event.order_id,
                 .price = event.price,
                 .quantity = event.quantity,
                 .aggressor = event.side};
}

}  // namespace flashpoint

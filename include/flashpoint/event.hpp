// The engine's output stream.
//
// Every accepted order, execution, rejection, cancellation and amendment is
// published as one Event, in the order it happened, numbered from 1.

#pragma once

#include "flashpoint/types.hpp"

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace flashpoint {

/// What an Event describes.
enum class EventType : std::uint8_t {
    /// An order passed validation and the engine took it. Emitted before any
    /// trades it goes on to produce, the way a FIX "New" acknowledgement is.
    Accepted,

    /// An order or request was refused. `reason` says why. Nothing changed.
    Rejected,

    /// One execution: a resting order matched against an incoming one.
    Trade,

    /// Quantity left the book without trading. Covers an explicit cancel, an
    /// ImmediateOrCancel remainder, a FillOrKill that could not fill, and a
    /// market order that ran out of protected liquidity.
    Cancelled,

    /// A resting order was amended. `priority` says whether it kept its place.
    /// Emitted before any trades a reprice goes on to produce.
    Modified,
};

[[nodiscard]] constexpr std::string_view to_string(EventType type) noexcept {
    switch (type) {
        case EventType::Accepted:
            return "Accepted";
        case EventType::Rejected:
            return "Rejected";
        case EventType::Trade:
            return "Trade";
        case EventType::Cancelled:
            return "Cancelled";
        case EventType::Modified:
            return "Modified";
    }
    return "Invalid";
}

/// Why a request was refused.
///
/// One list across submit, cancel and modify, so a consumer reading the stream
/// does not need to know which call produced the rejection.
enum class RejectReason : std::uint8_t {
    /// Not a rejection. What every non-Rejected event carries.
    None,

    /// The order was not well formed: no id, zero quantity, an out-of-range
    /// enum, or a market order marked GoodTillCancel.
    MalformedOrder,

    /// An order with this id is already resting.
    DuplicateOrderId,

    /// No order with this id is resting. The engine cannot tell "never existed"
    /// from "already filled" or "already cancelled" (DD-029).
    UnknownOrder,

    /// The id was `OrderId::kNone`, the reserved "no order" value.
    InvalidOrderId,

    /// A modify asked for zero quantity. Removing an order is cancel's job.
    InvalidQuantity,
};

[[nodiscard]] constexpr std::string_view to_string(RejectReason reason) noexcept {
    switch (reason) {
        case RejectReason::None:
            return "None";
        case RejectReason::MalformedOrder:
            return "MalformedOrder";
        case RejectReason::DuplicateOrderId:
            return "DuplicateOrderId";
        case RejectReason::UnknownOrder:
            return "UnknownOrder";
        case RejectReason::InvalidOrderId:
            return "InvalidOrderId";
        case RejectReason::InvalidQuantity:
            return "InvalidQuantity";
    }
    return "Invalid";
}

/// One entry in the engine's output stream.
///
/// A single flat record rather than a variant of five shapes, for replay: it is
/// trivially copyable and fixed size, so a stream can be written to a file or a
/// socket and read back with no encoding step, which is how market data
/// protocols are laid out.
///
/// The cost is that not every field means something for every type. The table
/// below is the contract; anything not listed for a type is left at its default
/// and must not be read.
///
/// | Type | Fields it populates |
/// |---|---|
/// | Accepted | order_id, price (its limit), quantity (accepted), side |
/// | Rejected | order_id, reason |
/// | Trade | order_id (taker), counterparty_id (maker), price, quantity, side |
/// | Cancelled | order_id, price, quantity (removed), side |
/// | Modified | order_id, price (new), quantity (new resting), side, priority |
///
/// Market orders have no meaningful price, so `price` is zero on their Accepted
/// and Cancelled events.
struct Event {
    /// Position in the stream, starting at 1. Gaps mean events were missed.
    SequenceNumber sequence{};

    /// The order this event is about. For a Trade this is the incoming order.
    OrderId order_id{};

    /// Trade only: the resting order that provided liquidity.
    OrderId counterparty_id{};

    Price price{};

    Quantity quantity{};

    Side side{};

    EventType type{};

    /// Rejected only.
    RejectReason reason{RejectReason::None};

    /// Modified only.
    QueuePriority priority{QueuePriority::Retained};

    friend constexpr bool operator==(const Event&, const Event&) noexcept = default;
};

static_assert(std::is_trivially_copyable_v<Event>,
              "Event must stay trivially copyable so a stream of them can be written and read "
              "back without an encoding step.");

static_assert(std::is_aggregate_v<Event>,
              "Event must remain an aggregate so designated initialisers name each field at the "
              "point it is set. Several fields share a type and would otherwise be transposable.");

}  // namespace flashpoint

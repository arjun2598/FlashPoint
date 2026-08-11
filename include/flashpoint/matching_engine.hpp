// The matching engine.
//
// Header-only. Every mutating call is templated on the event sink, so
// publishing an event is a direct call the compiler can inline, with no
// allocation and no indirect dispatch. That is a considered exception to DD-002,
// which puts orchestration in a translation unit: a template cannot live
// anywhere else, and this is the hottest code in the project.

#pragma once

#include "flashpoint/event.hpp"
#include "flashpoint/market_data.hpp"
#include "flashpoint/order.hpp"
#include "flashpoint/order_book.hpp"
#include "flashpoint/trade.hpp"
#include "flashpoint/types.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace flashpoint {

/// Outcome of submitting an order.
enum class SubmitStatus : std::uint8_t {
    /// The order was processed. `SubmitResult` says what happened to it.
    Accepted,

    /// The order was malformed. Nothing was touched.
    RejectedInvalid,

    /// An order with this id is already resting. Nothing was touched.
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
/// For an accepted order, `filled + resting + cancelled` equals the submitted
/// quantity. All three are zero for a rejected one.
///
/// This is a summary of what the event stream already said. It exists because
/// callers usually want the outcome without reading events, but the stream is
/// the authority.
struct SubmitResult {
    SubmitStatus status{SubmitStatus::Accepted};

    /// Quantity executed across every trade this order produced.
    Quantity filled{};

    /// Quantity left resting in the book.
    Quantity resting{};

    /// Quantity that neither traded nor rested. Non-zero only for orders that
    /// cannot rest: market orders, ImmediateOrCancel, and a FillOrKill that
    /// could not fill.
    Quantity cancelled{};

    [[nodiscard]] constexpr bool accepted() const noexcept {
        return status == SubmitStatus::Accepted;
    }

    friend constexpr bool operator==(const SubmitResult&, const SubmitResult&) noexcept = default;
};

/// Outcome of a cancel request.
enum class CancelStatus : std::uint8_t {
    /// The order was resting and has been removed.
    Cancelled,

    /// No order with that id is resting.
    ///
    /// Covers three cases the engine cannot tell apart: the id never existed,
    /// the order already filled, or it was already cancelled. Distinguishing
    /// them needs an order-history subsystem rather than a matching engine
    /// (DD-029).
    UnknownOrder,

    /// The id was `OrderId::kNone`, the reserved "no order" value.
    RejectedInvalidId,
};

[[nodiscard]] constexpr std::string_view to_string(CancelStatus status) noexcept {
    switch (status) {
        case CancelStatus::Cancelled:
            return "Cancelled";
        case CancelStatus::UnknownOrder:
            return "UnknownOrder";
        case CancelStatus::RejectedInvalidId:
            return "RejectedInvalidId";
    }
    return "Unknown";
}

/// What happened to a cancel request.
struct CancelResult {
    CancelStatus status{CancelStatus::UnknownOrder};

    /// Quantity that was still resting when the cancel took effect. This is the
    /// remaining quantity, not the original size: an order that filled 30 of 50
    /// before being cancelled reports 20.
    Quantity cancelled{};

    [[nodiscard]] constexpr bool succeeded() const noexcept {
        return status == CancelStatus::Cancelled;
    }

    friend constexpr bool operator==(const CancelResult&, const CancelResult&) noexcept = default;
};

/// Outcome of a modify request.
enum class ModifyStatus : std::uint8_t {
    /// The change was applied. `ModifyResult` says what became of the order.
    Modified,

    /// No order with that id is resting. Same limitation as cancel (DD-029).
    UnknownOrder,

    /// The id was `OrderId::kNone`.
    RejectedInvalidId,

    /// The requested quantity was zero. Removing an order is `cancel`.
    RejectedInvalidQuantity,
};

[[nodiscard]] constexpr std::string_view to_string(ModifyStatus status) noexcept {
    switch (status) {
        case ModifyStatus::Modified:
            return "Modified";
        case ModifyStatus::UnknownOrder:
            return "UnknownOrder";
        case ModifyStatus::RejectedInvalidId:
            return "RejectedInvalidId";
        case ModifyStatus::RejectedInvalidQuantity:
            return "RejectedInvalidQuantity";
    }
    return "Unknown";
}

/// What happened to a modify request.
///
/// For an applied change, `filled + resting` equals the requested quantity. A
/// modify never cancels anything: the order it produces is a resting limit
/// order, so whatever does not trade stays in the book.
struct ModifyResult {
    ModifyStatus status{ModifyStatus::UnknownOrder};

    /// Quantity traded as a direct result of this modify. Non-zero only when the
    /// new price crossed the spread.
    Quantity filled{};

    /// Quantity left resting after the change.
    Quantity resting{};

    /// Whether the order kept its place in line.
    QueuePriority priority{QueuePriority::Lost};

    [[nodiscard]] constexpr bool modified() const noexcept {
        return status == ModifyStatus::Modified;
    }

    friend constexpr bool operator==(const ModifyResult&, const ModifyResult&) noexcept = default;
};

/// Venue policy the engine applies to incoming orders.
struct EngineConfig {
    /// How far past the opposite touch a market order may trade, in ticks.
    ///
    /// A market buy when the best ask is 100 and the band is 5 gets an effective
    /// limit of 105. Anything it cannot fill by then is cancelled.
    ///
    /// Without it, a thin book would let a market order sweep to an arbitrary
    /// price. CME calls it market-with-protection.
    Price::Rep market_protection_ticks = 10;
};

/// Applies incoming orders to a single instrument's book at price-time priority.
///
/// The engine owns its book, and is the only thing that can leave it crossed. It
/// never does: an order matches against the opposing side until it can no longer
/// trade, and only then does the remainder rest. That is why `OrderBook` has no
/// crossing check of its own. A container cannot resolve a cross, because
/// resolving one means producing a trade.
///
/// Every mutating call takes an event sink and publishes what it did, in order,
/// with sequence numbers starting at 1. The returned result types are summaries
/// of the same information for callers that do not want to read the stream.
class MatchingEngine {
public:
    MatchingEngine() = default;

    explicit MatchingEngine(EngineConfig config) : config_(config) {
        assert(config_.market_protection_ticks >= 0 && "protection band cannot be negative");
    }

    /// Pre-sizes the underlying book for `expected_orders` resting orders.
    explicit MatchingEngine(std::size_t expected_orders) : book_(expected_orders) {}

    MatchingEngine(EngineConfig config, std::size_t expected_orders)
        : config_(config), book_(expected_orders) {
        assert(config_.market_protection_ticks >= 0 && "protection band cannot be negative");
    }

    /// Submits an order.
    ///
    /// Publishes, in this order: `Accepted` (or `Rejected`), then one `Trade`
    /// per execution, then `Cancelled` if any quantity could neither trade nor
    /// rest. An order that rests untouched produces one event.
    template <typename EventSink>
    [[nodiscard]] SubmitResult submit(const Order& order, EventSink&& on_event) {
        // The one place structural validity is checked. Everything downstream,
        // including OrderBook::add, may assume what reaches it is well formed.
        if (!order.is_valid()) {
            emit_reject(on_event, order.id(), RejectReason::MalformedOrder);
            return SubmitResult{SubmitStatus::RejectedInvalid, Quantity{}, Quantity{}, Quantity{}};
        }
        if (book_.contains(order.id())) {
            emit_reject(on_event, order.id(), RejectReason::DuplicateOrderId);
            return SubmitResult{SubmitStatus::RejectedDuplicateId, Quantity{}, Quantity{},
                                Quantity{}};
        }

        emit(on_event, Event{.order_id = order.id(),
                             .price = quoted_price(order),
                             .quantity = order.quantity(),
                             .side = order.side(),
                             .type = EventType::Accepted});

        return apply(order, on_event);
    }

    /// Changes the price and/or remaining quantity of a resting order.
    ///
    /// `new_quantity` is the new *remaining* quantity, not a new total order
    /// size (DD-031). Queue priority follows the usual venue rule:
    ///
    /// | Change | Priority |
    /// |---|---|
    /// | quantity reduced, same price | retained |
    /// | quantity unchanged, same price | retained |
    /// | quantity increased | lost |
    /// | price changed | lost |
    ///
    /// Shrinking an order takes nothing from the orders behind it, so there is
    /// no reason to move it back. Growing it does, so it goes to the back.
    ///
    /// A modify that reprices an order across the spread trades, exactly as a
    /// fresh order at that price would. The order keeps its id throughout.
    ///
    /// Publishes `Modified` (or `Rejected`), then one `Trade` per execution.
    template <typename EventSink>
    [[nodiscard]] ModifyResult modify(OrderId id, Price new_price, Quantity new_quantity,
                                      EventSink&& on_event) {
        if (!id.is_valid()) {
            emit_reject(on_event, id, RejectReason::InvalidOrderId);
            return ModifyResult{ModifyStatus::RejectedInvalidId, Quantity{}, Quantity{},
                                QueuePriority::Lost};
        }
        if (!new_quantity.is_valid()) {
            // Modifying to zero would mean deleting the order, which is cancel's
            // job. Rejecting keeps the two operations distinct.
            emit_reject(on_event, id, RejectReason::InvalidQuantity);
            return ModifyResult{ModifyStatus::RejectedInvalidQuantity, Quantity{}, Quantity{},
                                QueuePriority::Lost};
        }

        const std::optional<RestingOrder> existing = book_.resting_order(id);
        if (!existing.has_value()) {
            emit_reject(on_event, id, RejectReason::UnknownOrder);
            return ModifyResult{ModifyStatus::UnknownOrder, Quantity{}, Quantity{},
                                QueuePriority::Lost};
        }

        const bool price_changed = new_price != existing->price;
        const bool quantity_increased = new_quantity > existing->remaining;

        if (!price_changed && !quantity_increased) {
            // Same price, and not larger. The order stays exactly where it is.
            if (new_quantity < existing->remaining) {
                [[maybe_unused]] const bool reduced =
                    book_.reduce(id, existing->remaining - new_quantity);
                assert(reduced);
            }
            emit(on_event, Event{.order_id = id,
                                 .price = existing->price,
                                 .quantity = new_quantity,
                                 .side = existing->side,
                                 .type = EventType::Modified,
                                 .priority = QueuePriority::Retained});
            return ModifyResult{ModifyStatus::Modified, Quantity{}, new_quantity,
                                QueuePriority::Retained};
        }

        // Priority is lost, so the order leaves and comes back as a new arrival.
        // Routing it through apply() rather than straight back into the book is
        // what makes a repriced order trade when it now crosses. apply() is used
        // instead of submit() so the stream carries Modified rather than a
        // second Accepted for an order the client never resubmitted.
        [[maybe_unused]] const bool removed = book_.remove(id);
        assert(removed && "resting_order found it but remove did not");

        emit(on_event, Event{.order_id = id,
                             .price = new_price,
                             .quantity = new_quantity,
                             .side = existing->side,
                             .type = EventType::Modified,
                             .priority = QueuePriority::Lost});

        const SubmitResult result =
            apply(Order::limit(id, existing->side, new_price, new_quantity), on_event);
        assert(result.cancelled == Quantity{} &&
               "a GoodTillCancel limit order cannot be cancelled");

        return ModifyResult{ModifyStatus::Modified, result.filled, result.resting,
                            QueuePriority::Lost};
    }

    /// Cancels a resting order.
    ///
    /// Returns the quantity that was still resting, which is what the client
    /// actually pulled. An order that partially filled first reports only the
    /// part that was left.
    ///
    /// Publishes `Cancelled`, or `Rejected` if the cancel missed.
    ///
    /// There is no owner check. Anyone may cancel any order, because `Order`
    /// carries no participant id. That is the same gap as self-trade prevention
    /// and is parked for the same reason.
    template <typename EventSink>
    [[nodiscard]] CancelResult cancel(OrderId id, EventSink&& on_event) {
        if (!id.is_valid()) {
            emit_reject(on_event, id, RejectReason::InvalidOrderId);
            return CancelResult{CancelStatus::RejectedInvalidId, Quantity{}};
        }

        // Read the order before removing it, since it is gone afterwards and the
        // event needs its price and side.
        const std::optional<RestingOrder> existing = book_.resting_order(id);
        if (!existing.has_value()) {
            emit_reject(on_event, id, RejectReason::UnknownOrder);
            return CancelResult{CancelStatus::UnknownOrder, Quantity{}};
        }

        [[maybe_unused]] const bool removed = book_.remove(id);
        assert(removed && "resting_order found the order but remove did not");

        emit(on_event, Event{.order_id = id,
                             .price = existing->price,
                             .quantity = existing->remaining,
                             .side = existing->side,
                             .type = EventType::Cancelled});

        return CancelResult{CancelStatus::Cancelled, existing->remaining};
    }

    /// Read-only view of the book. Its own interface is handle-based (DD-018),
    /// so a const reference exposes nothing a caller could depend on.
    [[nodiscard]] const OrderBook& book() const noexcept {
        return book_;
    }

    [[nodiscard]] const EngineConfig& config() const noexcept {
        return config_;
    }

    /// The sequence number of the last event published. Zero before any.
    [[nodiscard]] SequenceNumber last_sequence() const noexcept {
        return last_sequence_;
    }

private:
    /// Stamps the next sequence number on an event and publishes it.
    template <typename EventSink>
    void emit(EventSink& on_event, Event event) {
        last_sequence_ = last_sequence_.next();
        event.sequence = last_sequence_;
        on_event(static_cast<const Event&>(event));
    }

    template <typename EventSink>
    void emit_reject(EventSink& on_event, OrderId id, RejectReason reason) {
        emit(on_event, Event{.order_id = id, .type = EventType::Rejected, .reason = reason});
    }

    /// A market order has no price of its own, so its events carry zero rather
    /// than a value the client never specified.
    [[nodiscard]] static constexpr Price quoted_price(const Order& order) noexcept {
        return order.type() == OrderType::Limit ? order.price() : Price{};
    }

    /// Matches an already-validated order and disposes of the remainder.
    ///
    /// Publishes trades, and a `Cancelled` event for any quantity that could
    /// neither trade nor rest. Does not publish an acknowledgement: the caller
    /// has already said whether this was an `Accepted` order or a `Modified`
    /// one.
    template <typename EventSink>
    SubmitResult apply(const Order& order, EventSink& on_event) {
        // A limit order uses its own price. A market order uses a protection
        // price derived from the opposite touch. Resolving that here means the
        // matching loop below has one code path instead of two.
        const std::optional<Price> limit = effective_limit(order);
        if (!limit.has_value()) {
            // Market order with nothing resting opposite. No reference price and
            // nothing to trade against, so the whole order is cancelled.
            emit_cancelled(on_event, order, order.quantity());
            return SubmitResult{SubmitStatus::Accepted, Quantity{}, Quantity{}, order.quantity()};
        }

        // Fill-or-kill has to know the answer before emitting anything, because
        // a trade already handed to the sink cannot be withdrawn.
        if (order.time_in_force() == TimeInForce::FillOrKill &&
            book_.quantity_available(order.side(), *limit) < order.quantity()) {
            emit_cancelled(on_event, order, order.quantity());
            return SubmitResult{SubmitStatus::Accepted, Quantity{}, Quantity{}, order.quantity()};
        }

        Quantity remaining = order.quantity();
        Quantity filled{};

        while (remaining > Quantity{}) {
            const std::optional<Price> touch = best_opposing(order.side());
            if (!touch.has_value() || !crosses(order.side(), *limit, *touch)) {
                break;
            }

            const std::optional<OrderId> maker = book_.front_at(opposite(order.side()), *touch);
            assert(maker.has_value() && "a level exists at the touch but holds no orders");

            const std::optional<Quantity> maker_remaining = book_.remaining_of(*maker);
            assert(maker_remaining.has_value() && "front_at returned an id the book does not hold");

            // Both sides are strictly positive, because the book never keeps a
            // zero-quantity order. So the fill is positive and the loop always
            // makes progress.
            const Quantity fill = std::min(remaining, *maker_remaining);
            assert(fill > Quantity{} && "a zero-quantity fill would loop forever");

            emit(on_event, Event{.order_id = order.id(),
                                 .counterparty_id = *maker,
                                 .price = *touch,  // the maker's price
                                 .quantity = fill,
                                 .side = order.side(),
                                 .type = EventType::Trade});

            if (fill == *maker_remaining) {
                [[maybe_unused]] const bool retired = book_.remove(*maker);
                assert(retired);
            } else {
                // Partial fill. The maker keeps its place in the queue.
                [[maybe_unused]] const bool reduced = book_.reduce(*maker, fill);
                assert(reduced);
            }

            remaining -= fill;
            filled += fill;
        }

        assert((order.time_in_force() != TimeInForce::FillOrKill || remaining == Quantity{}) &&
               "fill-or-kill passed its feasibility check but did not fill");

        if (remaining == Quantity{}) {
            return SubmitResult{SubmitStatus::Accepted, filled, Quantity{}, Quantity{}};
        }

        // Only GoodTillCancel rests. Market orders cannot be GoodTillCancel,
        // so anything reaching this branch is a limit order.
        if (order.time_in_force() == TimeInForce::GoodTillCancel) {
            assert(order.type() == OrderType::Limit);
            [[maybe_unused]] const bool rested =
                book_.add(Order{order.id(), order.side(), order.price(), remaining, order.type(),
                                order.time_in_force()});
            assert(rested && "the id was checked as absent on entry");
            return SubmitResult{SubmitStatus::Accepted, filled, remaining, Quantity{}};
        }

        emit_cancelled(on_event, order, remaining);
        return SubmitResult{SubmitStatus::Accepted, filled, Quantity{}, remaining};
    }

    template <typename EventSink>
    void emit_cancelled(EventSink& on_event, const Order& order, Quantity quantity) {
        emit(on_event, Event{.order_id = order.id(),
                             .price = quoted_price(order),
                             .quantity = quantity,
                             .side = order.side(),
                             .type = EventType::Cancelled});
    }

    /// The worst price this order is willing to trade at.
    ///
    /// Limit orders carry their own. Market orders get one derived from the
    /// opposite touch, or nullopt when that side is empty.
    [[nodiscard]] std::optional<Price> effective_limit(const Order& order) const {
        if (order.type() == OrderType::Limit) {
            return order.price();
        }

        const std::optional<Price> touch = best_opposing(order.side());
        if (!touch.has_value()) {
            return std::nullopt;
        }
        return protection_price(order.side(), *touch);
    }

    /// Touch plus the protection band for a buy, minus it for a sell.
    ///
    /// Saturating, not wrapping. A touch near the end of the price range would
    /// otherwise overflow, which is undefined behaviour for a signed type.
    [[nodiscard]] Price protection_price(Side side, Price touch) const noexcept {
        constexpr Price::Rep kLowest = std::numeric_limits<Price::Rep>::min();
        constexpr Price::Rep kHighest = std::numeric_limits<Price::Rep>::max();

        const Price::Rep band = config_.market_protection_ticks;
        const Price::Rep at = touch.ticks();

        if (side == Side::Buy) {
            return Price{at > kHighest - band ? kHighest : at + band};
        }
        return Price{at < kLowest + band ? kLowest : at - band};
    }

    /// The best price on the side an aggressor trades against. A buy lifts the
    /// lowest ask, a sell hits the highest bid.
    [[nodiscard]] std::optional<Price> best_opposing(Side side) const {
        return side == Side::Buy ? book_.best_ask() : book_.best_bid();
    }

    /// Whether an aggressor limited at `limit` can trade against `resting`.
    ///
    /// A buyer pays up to its limit, so it crosses any ask at or below it. A
    /// seller accepts down to its limit, so it crosses any bid at or above it.
    /// Equality trades in both directions.
    [[nodiscard]] static constexpr bool crosses(Side aggressor, Price limit,
                                                Price resting) noexcept {
        return aggressor == Side::Buy ? resting <= limit : resting >= limit;
    }

    EngineConfig config_{};
    OrderBook book_;
    SequenceNumber last_sequence_{};
};

}  // namespace flashpoint

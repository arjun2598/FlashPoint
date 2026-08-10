// The matching engine.
//
// Header-only. `submit` is templated on the trade sink so delivering a trade is
// a direct call the compiler can inline, with no allocation and no indirect
// dispatch. That is a considered exception to DD-002, which puts orchestration
// in a translation unit: a template cannot live anywhere else, and this is the
// hottest code in the project.

#pragma once

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
/// quantity. All three are zero for a rejected one. The tests check that
/// identity on every submission, which catches quantity being invented or lost.
///
/// `status` says whether the order was accepted. The three quantities say what
/// became of it. Keeping those separate stops the status list from growing a
/// case for every combination as later milestones add order handling.
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
    /// This covers three cases the engine cannot tell apart: the id never
    /// existed, the order already filled completely, or it was already
    /// cancelled. Distinguishing them means keeping a record of every id ever
    /// seen, which is an order-history subsystem rather than a matching engine
    /// concern (DD-029).
    UnknownOrder,

    /// The id itself is malformed. `OrderId::kNone` is the reserved "no order"
    /// value, so a cancel naming it can never refer to anything.
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

    /// Quantity that was still resting when the cancel took effect, and is now
    /// gone. This is the *remaining* quantity, not the order's original size: an
    /// order that filled 30 of 50 before being cancelled reports 20.
    ///
    /// Zero unless `status` is Cancelled.
    Quantity cancelled{};

    [[nodiscard]] constexpr bool succeeded() const noexcept {
        return status == CancelStatus::Cancelled;
    }

    friend constexpr bool operator==(const CancelResult&, const CancelResult&) noexcept = default;
};

/// Whether a modified order kept its place in the queue at its price level.
enum class QueuePriority : std::uint8_t {
    /// The order stayed where it was.
    Retained,

    /// The order went to the back of the queue at its price.
    Lost,
};

[[nodiscard]] constexpr std::string_view to_string(QueuePriority priority) noexcept {
    switch (priority) {
        case QueuePriority::Retained:
            return "Retained";
        case QueuePriority::Lost:
            return "Lost";
    }
    return "Unknown";
}

/// Outcome of a modify request.
enum class ModifyStatus : std::uint8_t {
    /// The change was applied. `ModifyResult` says what became of the order.
    Modified,

    /// No order with that id is resting. Same limitation as cancel: the engine
    /// cannot tell "never existed" from "already filled" (DD-029).
    UnknownOrder,

    /// The id was `OrderId::kNone`, the reserved "no order" value.
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
    /// This is what makes a market order safe to send: without it, a thin book
    /// would let one sweep to an arbitrary price. CME calls it market-with-protection.
    Price::Rep market_protection_ticks = 10;
};

/// Applies incoming orders to a single instrument's book at price-time priority.
///
/// The engine owns its book, and is the only thing that can leave it crossed. It
/// never does: an order matches against the opposing side until it can no longer
/// trade, and only then does the remainder rest. That is why `OrderBook` has no
/// crossing check of its own. A container cannot resolve a cross, because
/// resolving one means producing a trade.
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
    /// `on_trade` is called once per execution, in the order the executions
    /// happen, with a `const Trade&`. An order that does not trade never calls
    /// it. That tends to be the common case in real flow, and it costs nothing.
    template <typename TradeSink>
    [[nodiscard]] SubmitResult submit(const Order& order, TradeSink&& on_trade) {
        // The one place structural validity is checked. Everything downstream,
        // including OrderBook::add, may assume what reaches it is well formed.
        if (!order.is_valid()) {
            return rejected(SubmitStatus::RejectedInvalid);
        }
        if (book_.contains(order.id())) {
            return rejected(SubmitStatus::RejectedDuplicateId);
        }

        // A limit order uses its own price. A market order uses a protection
        // price derived from the opposite touch. Resolving that here means the
        // matching loop below has one code path instead of two.
        const std::optional<Price> limit = effective_limit(order);
        if (!limit.has_value()) {
            // Market order with nothing resting opposite. No reference price and
            // nothing to trade against, so the whole order is cancelled.
            return SubmitResult{SubmitStatus::Accepted, Quantity{}, Quantity{}, order.quantity()};
        }

        // Fill-or-kill has to know the answer before emitting anything, because
        // a trade already handed to the sink cannot be withdrawn.
        if (order.time_in_force() == TimeInForce::FillOrKill &&
            book_.quantity_available(order.side(), *limit) < order.quantity()) {
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

            on_trade(Trade{
                .maker_id = *maker,
                .taker_id = order.id(),
                .price = *touch,  // the maker's price; improvement goes to the taker
                .quantity = fill,
                .aggressor = order.side(),
            });

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

        return SubmitResult{SubmitStatus::Accepted, filled, Quantity{}, remaining};
    }

    /// Changes the price and/or remaining quantity of a resting order.
    ///
    /// `new_quantity` is the new *remaining* quantity, not a new total order
    /// size. An order with 20 left that is modified to 25 rests 25, whatever it
    /// filled beforehand. FIX models this the other way, as a new total, but
    /// that needs the original quantity stored per resting order and the book
    /// keeps only the remainder (DD-017, DD-031).
    ///
    /// Queue priority follows the usual venue rule:
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
    /// fresh order at that price would. `on_trade` is called for each execution.
    /// The order keeps its id throughout.
    template <typename TradeSink>
    [[nodiscard]] ModifyResult modify(OrderId id, Price new_price, Quantity new_quantity,
                                      TradeSink&& on_trade) {
        if (!id.is_valid()) {
            return ModifyResult{ModifyStatus::RejectedInvalidId, Quantity{}, Quantity{},
                                QueuePriority::Lost};
        }
        if (!new_quantity.is_valid()) {
            // Modifying to zero would mean deleting the order, which is cancel's
            // job. Rejecting keeps the two operations distinct.
            return ModifyResult{ModifyStatus::RejectedInvalidQuantity, Quantity{}, Quantity{},
                                QueuePriority::Lost};
        }

        const std::optional<RestingOrder> existing = book_.resting_order(id);
        if (!existing.has_value()) {
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
            return ModifyResult{ModifyStatus::Modified, Quantity{}, new_quantity,
                                QueuePriority::Retained};
        }

        // Priority is lost, so the order leaves and comes back as a new arrival.
        // Routing it through submit() rather than straight back into the book is
        // what makes a repriced order trade when it now crosses.
        [[maybe_unused]] const bool removed = book_.remove(id);
        assert(removed && "resting_order found it but remove did not");

        // Every rejection submit() can produce is already ruled out: the id was
        // just removed so it cannot collide, and a Limit/GoodTillCancel order
        // built from a valid id, a book-supplied side and a non-zero quantity is
        // structurally valid.
        const SubmitResult result =
            submit(Order::limit(id, existing->side, new_price, new_quantity),
                   std::forward<TradeSink>(on_trade));
        assert(result.accepted() && "the replacement order should always be acceptable");

        return ModifyResult{ModifyStatus::Modified, result.filled, result.resting,
                            QueuePriority::Lost};
    }

    /// Cancels a resting order.
    ///
    /// Returns the quantity that was still resting, which is what the client
    /// actually pulled. An order that partially filled first reports only the
    /// part that was left.
    ///
    /// Cancelling is idempotent from the caller's point of view: a second cancel
    /// of the same id reports UnknownOrder rather than failing loudly.
    ///
    /// There is no owner check. Anyone may cancel any order, because `Order`
    /// carries no participant id. That is the same gap as self-trade prevention
    /// and is parked for the same reason.
    [[nodiscard]] CancelResult cancel(OrderId id) {
        if (!id.is_valid()) {
            return CancelResult{CancelStatus::RejectedInvalidId, Quantity{}};
        }

        // Read the remaining quantity before removing, since the order is gone
        // afterwards and this is what the caller needs reported back.
        const std::optional<Quantity> remaining = book_.remaining_of(id);
        if (!remaining.has_value()) {
            return CancelResult{CancelStatus::UnknownOrder, Quantity{}};
        }

        [[maybe_unused]] const bool removed = book_.remove(id);
        assert(removed && "remaining_of found the order but remove did not");

        return CancelResult{CancelStatus::Cancelled, *remaining};
    }

    /// Read-only view of the book. Its own interface is handle-based (DD-018),
    /// so a const reference exposes nothing a caller could depend on.
    [[nodiscard]] const OrderBook& book() const noexcept {
        return book_;
    }

    [[nodiscard]] const EngineConfig& config() const noexcept {
        return config_;
    }

private:
    [[nodiscard]] static constexpr SubmitResult rejected(SubmitStatus status) noexcept {
        return SubmitResult{status, Quantity{}, Quantity{}, Quantity{}};
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
};

}  // namespace flashpoint

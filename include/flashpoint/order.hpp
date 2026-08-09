#pragma once

#include "flashpoint/types.hpp"

#include <type_traits>

namespace flashpoint {

/// An inbound order request: what a client asked for, before the engine has
/// decided anything about it.
///
/// Immutable by design, and not what the book stores.
///
/// There is no timestamp. Time priority comes from FIFO ordering within a price
/// level, which the book provides, so an arrival time per order would be eight
/// redundant bytes on the hottest object in the system.
class Order {
public:
    constexpr Order() noexcept = default;

    /// The general form. Prefer the `limit()` and `market()` factories below,
    /// which name what they build and set the right defaults.
    constexpr Order(OrderId id, Side side, Price price, Quantity quantity,
                    OrderType type = OrderType::Limit,
                    TimeInForce time_in_force = TimeInForce::GoodTillCancel) noexcept
        : id_(id),
          price_(price),
          quantity_(quantity),
          side_(side),
          type_(type),
          time_in_force_(time_in_force) {}

    /// A limit order. Rests by default.
    [[nodiscard]] static constexpr Order limit(
        OrderId id, Side side, Price price, Quantity quantity,
        TimeInForce time_in_force = TimeInForce::GoodTillCancel) noexcept {
        return Order{id, side, price, quantity, OrderType::Limit, time_in_force};
    }

    /// A market order. Has no price, so the price field is left at zero.
    ///
    /// Defaults to ImmediateOrCancel because a market order cannot rest: there
    /// is no price to rest at. GoodTillCancel is rejected as invalid.
    [[nodiscard]] static constexpr Order market(
        OrderId id, Side side, Quantity quantity,
        TimeInForce time_in_force = TimeInForce::ImmediateOrCancel) noexcept {
        return Order{id, side, Price{}, quantity, OrderType::Market, time_in_force};
    }

    [[nodiscard]] constexpr OrderId id() const noexcept {
        return id_;
    }

    [[nodiscard]] constexpr Side side() const noexcept {
        return side_;
    }

    /// The order's limit price. Unused when `type()` is Market; the engine
    /// derives a protection price from the book instead.
    [[nodiscard]] constexpr Price price() const noexcept {
        return price_;
    }

    [[nodiscard]] constexpr Quantity quantity() const noexcept {
        return quantity_;
    }

    [[nodiscard]] constexpr OrderType type() const noexcept {
        return type_;
    }

    [[nodiscard]] constexpr TimeInForce time_in_force() const noexcept {
        return time_in_force_;
    }

    /// Whether the message is well formed. Nothing about venue policy.
    ///
    /// Price bands, lot sizes, and entitlements are checked once at the engine
    /// boundary, not here and not on the hot path.
    ///
    /// A market order with GoodTillCancel is rejected. Market orders never rest,
    /// so the combination has no meaning.
    [[nodiscard]] constexpr bool is_valid() const noexcept {
        if (!id_.is_valid() || !quantity_.is_valid()) {
            return false;
        }
        if (!flashpoint::is_valid(side_) || !flashpoint::is_valid(type_) ||
            !flashpoint::is_valid(time_in_force_)) {
            return false;
        }
        return type_ != OrderType::Market || time_in_force_ != TimeInForce::GoodTillCancel;
    }

    friend constexpr bool operator==(const Order&, const Order&) noexcept = default;

private:
    // Largest fields first: 8 + 8 + 8 + 1 + 1 + 1 = 27 bytes of payload, which
    // 8-byte alignment rounds up to a 32-byte object. Two orders still share a
    // 64-byte cache line. tests/order_test.cpp pins the size so a later field
    // addition has to be a deliberate choice.
    OrderId id_{};
    Price price_{};
    Quantity quantity_{};
    Side side_{};
    OrderType type_{OrderType::Limit};
    TimeInForce time_in_force_{TimeInForce::GoodTillCancel};
};

static_assert(std::is_trivially_copyable_v<Order>,
              "Order must stay trivially copyable. The book moves orders around freely, and any "
              "member that broke this would put an allocation on the hot path.");

}  // namespace flashpoint

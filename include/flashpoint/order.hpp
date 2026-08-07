#pragma once

#include "flashpoint/types.hpp"

#include <type_traits>

namespace flashpoint {

/// An inbound order request: what a client asked for, before the engine has
/// decided anything about it.
///
/// Immutable by design, and not what the book stores.
///
/// There is deliberately no timestamp. Time priority is a structural property of
/// FIFO ordering within a price level, which the book provides. Storing an
/// arrival time per order would be eight redundant bytes on the hottest object
/// in the system.
class Order {
public:
    constexpr Order() noexcept = default;

    constexpr Order(OrderId id, Side side, Price price, Quantity quantity) noexcept
        : id_(id), price_(price), quantity_(quantity), side_(side) {}

    [[nodiscard]] constexpr OrderId id() const noexcept {
        return id_;
    }

    [[nodiscard]] constexpr Side side() const noexcept {
        return side_;
    }

    [[nodiscard]] constexpr Price price() const noexcept {
        return price_;
    }

    [[nodiscard]] constexpr Quantity quantity() const noexcept {
        return quantity_;
    }

    /// Structural validity only: is this well formed as a message?
    ///
    /// Venue policy -- price bands, lot sizes, whether this instrument permits
    /// negative prices, whether the client is entitled to trade it -- is checked
    /// once at the engine boundary (Milestone 5). Never here, and never on the
    /// hot path: what you validate at the boundary you may trust internally.
    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return id_.is_valid() && quantity_.is_valid() && flashpoint::is_valid(side_);
    }

    friend constexpr bool operator==(const Order&, const Order&) noexcept = default;

private:
    // Ordered largest-first: 8 + 8 + 8 + 1 bytes of payload, which the 8-byte
    // alignment rounds to a 32-byte object. Two orders share a 64-byte cache
    // line. tests/order_test.cpp pins this so a careless field addition is
    // caught rather than quietly doubling the book's memory traffic.
    OrderId id_{};
    Price price_{};
    Quantity quantity_{};
    Side side_{};
};

static_assert(std::is_trivially_copyable_v<Order>,
              "Order must stay trivially copyable. The book relies on moving orders around "
              "freely, and any member that breaks this would "
              "put an allocation on the hot path.");

}  // namespace flashpoint

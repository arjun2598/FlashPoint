// Tests for the Order value type.
//
// Order has no logic beyond construction and a structural validity check, so
// the tests concentrate on the two things that would actually hurt later: that
// the field types make an argument transposition impossible to compile, and
// that the memory layout does not drift.

#include "flashpoint/order.hpp"

#include "flashpoint/ostream.hpp"
#include "flashpoint/types.hpp"

#include <gtest/gtest.h>

#include <type_traits>

namespace flashpoint {
namespace {

constexpr Order make_order() {
    return Order{OrderId{7}, Side::Buy, Price{10250}, Quantity{500}};
}

// ---------------------------------------------------------------------------
// Compile-time guarantees
// ---------------------------------------------------------------------------

// The payoff for DD-010. Order takes price and quantity adjacently, ane in a
// codebase of raw int64s, these arguments could easily be swapped mistakenly.
// No warning might catch this, since both arguments would be the same type. Here the
// transposed call is simply not a viable constructor.
static_assert(std::is_constructible_v<Order, OrderId, Side, Price, Quantity>);
static_assert(!std::is_constructible_v<Order, OrderId, Side, Quantity, Price>);

// Nor can the identifier and the price trade places.
static_assert(!std::is_constructible_v<Order, Price, Side, OrderId, Quantity>);

// The book will move orders around freely. Any member that broke this would put an allocation on
// the hot path. order.hpp asserts this too, but repeating it here means the test suite
// reports it as a named failure rather than a build error.
static_assert(std::is_trivially_copyable_v<Order>);

// Constructible in a constant expression, so orders can be built into constexpr
// fixtures and compile-time scenario tables.
static_assert(make_order().id() == OrderId{7});

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

// Pins the memory layout. Not a correctness property but a performance
// regression test: at 32 bytes two orders share a 64-byte cache line, and the
// book's traffic is dominated by walking orders. A later field addition fails
// here rather than passing unnoticed.
//
// 8 (OrderId) + 8 (Price) + 8 (Quantity) + 1 (Side) + 1 (OrderType)
// + 1 (TimeInForce) = 27 bytes of payload, rounded to 32 by the 8-byte
// alignment.
TEST(Order, FitsInThirtyTwoBytesSoTwoShareACacheLine) {
    EXPECT_EQ(sizeof(Order), 32U);
    EXPECT_EQ(alignof(Order), 8U);
}

// ---------------------------------------------------------------------------
// Construction and access
// ---------------------------------------------------------------------------

TEST(Order, ReportsBackExactlyWhatItWasBuiltFrom) {
    const Order order{OrderId{7}, Side::Buy, Price{10250}, Quantity{500}};

    EXPECT_EQ(order.id(), OrderId{7});
    EXPECT_EQ(order.side(), Side::Buy);
    EXPECT_EQ(order.price(), Price{10250});
    EXPECT_EQ(order.quantity(), Quantity{500});
}

TEST(Order, DefaultConstructsToAnEmptyInvalidOrder) {
    const Order order{};

    EXPECT_EQ(order.id(), OrderId{});
    EXPECT_EQ(order.price(), Price{});
    EXPECT_EQ(order.quantity(), Quantity{});
    EXPECT_FALSE(order.is_valid());
}

TEST(Order, EqualityComparesEveryField) {
    const Order order{OrderId{7}, Side::Buy, Price{10250}, Quantity{500}};

    EXPECT_EQ(order, (Order{OrderId{7}, Side::Buy, Price{10250}, Quantity{500}}));

    EXPECT_NE(order, (Order{OrderId{8}, Side::Buy, Price{10250}, Quantity{500}}));
    EXPECT_NE(order, (Order{OrderId{7}, Side::Sell, Price{10250}, Quantity{500}}));
    EXPECT_NE(order, (Order{OrderId{7}, Side::Buy, Price{10251}, Quantity{500}}));
    EXPECT_NE(order, (Order{OrderId{7}, Side::Buy, Price{10250}, Quantity{501}}));
}

// ---------------------------------------------------------------------------
// Structural validity
// ---------------------------------------------------------------------------

TEST(Order, WellFormedOrderIsValid) {
    EXPECT_TRUE((Order{OrderId{7}, Side::Buy, Price{10250}, Quantity{500}}.is_valid()));
    EXPECT_TRUE((Order{OrderId{7}, Side::Sell, Price{10250}, Quantity{500}}.is_valid()));
}

TEST(Order, ReservedOrderIdMakesTheOrderInvalid) {
    EXPECT_FALSE(
        (Order{OrderId{OrderId::kNone}, Side::Buy, Price{10250}, Quantity{500}}.is_valid()));
}

TEST(Order, ZeroQuantityMakesTheOrderInvalid) {
    EXPECT_FALSE((Order{OrderId{7}, Side::Buy, Price{10250}, Quantity{0}}.is_valid()));
}

// The realistic path to this state is decoding a malformed inbound message,
// which is precisely what the boundary check at Milestone 5 must reject.
TEST(Order, SideOutsideTheEnumeratorsMakesTheOrderInvalid) {
    EXPECT_FALSE((Order{OrderId{7}, static_cast<Side>(9), Price{10250}, Quantity{500}}.is_valid()));
}

// Price carries no validity constraint of its own: zero and negative ticks are
// both legitimately representable, and whether a venue accepts them is policy
// checked at the engine boundary. An order is not malformed merely for being
// priced oddly.
TEST(Order, PriceNeverAffectsStructuralValidity) {
    EXPECT_TRUE((Order{OrderId{7}, Side::Buy, Price{0}, Quantity{500}}.is_valid()));
    EXPECT_TRUE((Order{OrderId{7}, Side::Buy, Price{-500}, Quantity{500}}.is_valid()));
}

// ---------------------------------------------------------------------------
// Order type and time-in-force
// ---------------------------------------------------------------------------

TEST(Order, DefaultsToAGoodTillCancelLimitOrder) {
    const Order order{OrderId{7}, Side::Buy, Price{100}, Quantity{10}};

    EXPECT_EQ(order.type(), OrderType::Limit);
    EXPECT_EQ(order.time_in_force(), TimeInForce::GoodTillCancel);
}

TEST(Order, LimitFactoryBuildsALimitOrder) {
    const Order order = Order::limit(OrderId{7}, Side::Sell, Price{100}, Quantity{10},
                                     TimeInForce::ImmediateOrCancel);

    EXPECT_EQ(order.type(), OrderType::Limit);
    EXPECT_EQ(order.price(), Price{100});
    EXPECT_EQ(order.time_in_force(), TimeInForce::ImmediateOrCancel);
    EXPECT_TRUE(order.is_valid());
}

// A market order has no price, so the factory leaves the field at zero rather
// than asking the caller for a value that would be ignored.
TEST(Order, MarketFactoryLeavesThePriceAtZeroAndDefaultsToIoc) {
    const Order order = Order::market(OrderId{7}, Side::Buy, Quantity{10});

    EXPECT_EQ(order.type(), OrderType::Market);
    EXPECT_EQ(order.price(), Price{});
    EXPECT_EQ(order.time_in_force(), TimeInForce::ImmediateOrCancel);
    EXPECT_TRUE(order.is_valid());
}

// A market order cannot rest, so GoodTillCancel has no meaning for one.
TEST(Order, MarketOrderWithGoodTillCancelIsInvalid) {
    EXPECT_FALSE(
        Order::market(OrderId{7}, Side::Buy, Quantity{10}, TimeInForce::GoodTillCancel).is_valid());
}

TEST(Order, MarketOrderWithImmediateOrCancelOrFillOrKillIsValid) {
    EXPECT_TRUE(Order::market(OrderId{7}, Side::Buy, Quantity{10}, TimeInForce::ImmediateOrCancel)
                    .is_valid());
    EXPECT_TRUE(
        Order::market(OrderId{7}, Side::Buy, Quantity{10}, TimeInForce::FillOrKill).is_valid());
}

TEST(Order, EveryTimeInForceIsValidOnALimitOrder) {
    for (const TimeInForce tif :
         {TimeInForce::GoodTillCancel, TimeInForce::ImmediateOrCancel, TimeInForce::FillOrKill}) {
        EXPECT_TRUE(Order::limit(OrderId{7}, Side::Buy, Price{100}, Quantity{10}, tif).is_valid())
            << to_string(tif);
    }
}

TEST(Order, OutOfRangeTypeOrTimeInForceMakesTheOrderInvalid) {
    EXPECT_FALSE((Order{OrderId{7}, Side::Buy, Price{100}, Quantity{10}, static_cast<OrderType>(7),
                        TimeInForce::GoodTillCancel}
                      .is_valid()));
    EXPECT_FALSE((Order{OrderId{7}, Side::Buy, Price{100}, Quantity{10}, OrderType::Limit,
                        static_cast<TimeInForce>(9)}
                      .is_valid()));
}

TEST(Order, EqualityComparesTypeAndTimeInForce) {
    const Order base = Order::limit(OrderId{7}, Side::Buy, Price{100}, Quantity{10});

    EXPECT_NE(base, Order::limit(OrderId{7}, Side::Buy, Price{100}, Quantity{10},
                                 TimeInForce::ImmediateOrCancel));
    EXPECT_NE(base, (Order{OrderId{7}, Side::Buy, Price{100}, Quantity{10}, OrderType::Market,
                           TimeInForce::ImmediateOrCancel}));
}

}  // namespace
}  // namespace flashpoint

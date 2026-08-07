// Tests for the core scalar value types.
//
// These types carry almost no behaviour, so most of what matters about them is
// enforced at compile time: that a raw integer cannot silently become a Price,
// that a Quantity cannot be passed where a Price belongs, and that they stay
// cheap enough to hand around by value. Those are static_asserts rather than
// runtime cases because a runtime test could not express them. The code under
// test would fail to compile if they were violated, which is exactly the point.

#include "flashpoint/types.hpp"

#include "flashpoint/ostream.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <type_traits>

namespace flashpoint {
namespace {

// ---------------------------------------------------------------------------
// Compile-time guarantees
// ---------------------------------------------------------------------------

// The entire justification for hand-written strong types. If any of these
// become convertible, `Order{id, side, quantity, price}` starts compiling with
// price and quantity transposed, and no warning can catch it because both wrap
// 64-bit integers. Explicit constructors are what prevent that.
static_assert(!std::is_convertible_v<Price::Rep, Price>);
static_assert(!std::is_convertible_v<Quantity::Rep, Quantity>);
static_assert(!std::is_convertible_v<OrderId::Rep, OrderId>);

// The types must not be interconvertible with each other either.
static_assert(!std::is_constructible_v<Price, Quantity>);
static_assert(!std::is_constructible_v<Quantity, Price>);
static_assert(!std::is_constructible_v<OrderId, Price>);
static_assert(!std::is_constructible_v<Price, OrderId>);

// Explicit construction from the representation must still work, or the types
// would be unusable.
static_assert(std::is_constructible_v<Price, Price::Rep>);
static_assert(std::is_constructible_v<Quantity, Quantity::Rep>);
static_assert(std::is_constructible_v<OrderId, OrderId::Rep>);

// These are passed and returned by value on every hot path, so the wrapper must
// cost exactly nothing over the raw integer.
static_assert(std::is_trivially_copyable_v<Price>);
static_assert(std::is_trivially_copyable_v<Quantity>);
static_assert(std::is_trivially_copyable_v<OrderId>);
static_assert(sizeof(Price) == sizeof(Price::Rep));
static_assert(sizeof(Quantity) == sizeof(Quantity::Rep));
static_assert(sizeof(OrderId) == sizeof(OrderId::Rep));

// Usable in constant expressions, so prices and quantities can appear in
// constexpr tables and compile-time assertions in later milestones.
static_assert(Price{100} < Price{101});
static_assert(Quantity{3} + Quantity{4} == Quantity{7});
static_assert(OrderId{7}.is_valid());
static_assert(opposite(Side::Buy) == Side::Sell);

// ---------------------------------------------------------------------------
// Side
// ---------------------------------------------------------------------------

TEST(Side, OppositeSwapsBuyAndSell) {
    EXPECT_EQ(opposite(Side::Buy), Side::Sell);
    EXPECT_EQ(opposite(Side::Sell), Side::Buy);
}

// Applying opposite() twice must return the original side. Matching code calls
// it on both the aggressing and resting sides, so an asymmetry here would
// silently route orders to the wrong half of the book.
TEST(Side, OppositeIsItsOwnInverse) {
    EXPECT_EQ(opposite(opposite(Side::Buy)), Side::Buy);
    EXPECT_EQ(opposite(opposite(Side::Sell)), Side::Sell);
}

TEST(Side, DefinedEnumeratorsAreValid) {
    EXPECT_TRUE(is_valid(Side::Buy));
    EXPECT_TRUE(is_valid(Side::Sell));
}

// An enum class constrains nothing about its value range. Decoding a malformed
// wire message is exactly how an out-of-range Side gets created, so this is a
// real failure case and not a hypothetical one.
TEST(Side, ValueOutsideTheEnumeratorsIsRejected) {
    EXPECT_FALSE(is_valid(static_cast<Side>(2)));
    EXPECT_FALSE(is_valid(static_cast<Side>(255)));
}

TEST(Side, ToStringNamesBothSidesAndFlagsGarbage) {
    EXPECT_EQ(to_string(Side::Buy), "Buy");
    EXPECT_EQ(to_string(Side::Sell), "Sell");
    EXPECT_EQ(to_string(static_cast<Side>(99)), "Invalid");
}

// One byte, so Side costs nothing inside Order.
TEST(Side, OccupiesASingleByte) {
    EXPECT_EQ(sizeof(Side), 1U);
}

// ---------------------------------------------------------------------------
// Price
// ---------------------------------------------------------------------------

TEST(Price, DefaultConstructsToZeroTicks) {
    EXPECT_EQ(Price{}.ticks(), 0);
}

TEST(Price, PreservesTheTickCountItWasGiven) {
    EXPECT_EQ(Price{10250}.ticks(), 10250);
}

TEST(Price, OrdersByTickCount) {
    EXPECT_LT(Price{100}, Price{101});
    EXPECT_GT(Price{101}, Price{100});
    EXPECT_LE(Price{100}, Price{100});
    EXPECT_GE(Price{100}, Price{100});
    EXPECT_EQ(Price{100}, Price{100});
    EXPECT_NE(Price{100}, Price{101});
}

// Negative prices are not a malformed state. Oil futures settled below zero in
// 2020 and spread instruments quote negative as a matter of course. Whether a
// venue accepts one is policy for the engine boundary, not for this type.
TEST(Price, RepresentsNegativeValuesAndOrdersThemCorrectly) {
    EXPECT_EQ(Price{-500}.ticks(), -500);
    EXPECT_LT(Price{-500}, Price{0});
    EXPECT_LT(Price{-500}, Price{-499});
}

// The representation is the full signed 64-bit range; nothing may clamp it.
TEST(Price, SurvivesTheExtremesOfItsRepresentation) {
    constexpr auto kMin = std::numeric_limits<Price::Rep>::min();
    constexpr auto kMax = std::numeric_limits<Price::Rep>::max();

    EXPECT_EQ(Price{kMin}.ticks(), kMin);
    EXPECT_EQ(Price{kMax}.ticks(), kMax);
    EXPECT_LT(Price{kMin}, Price{kMax});
}

// ---------------------------------------------------------------------------
// Quantity
// ---------------------------------------------------------------------------

TEST(Quantity, DefaultConstructsToZero) {
    EXPECT_EQ(Quantity{}.value(), 0U);
}

// A zero-quantity order is meaningless, and it is the only malformed state the
// type can reach: unsigned representation makes negatives impossible.
TEST(Quantity, ZeroIsInvalidAndAnythingPositiveIsValid) {
    EXPECT_FALSE(Quantity{}.is_valid());
    EXPECT_FALSE(Quantity{0}.is_valid());
    EXPECT_TRUE(Quantity{1}.is_valid());
    EXPECT_TRUE(Quantity{std::numeric_limits<Quantity::Rep>::max()}.is_valid());
}

TEST(Quantity, AddsAndSubtracts) {
    EXPECT_EQ(Quantity{300} + Quantity{200}, Quantity{500});
    EXPECT_EQ(Quantity{500} - Quantity{200}, Quantity{300});
}

TEST(Quantity, CompoundAssignmentMatchesTheBinaryOperators) {
    Quantity q{100};
    q += Quantity{50};
    EXPECT_EQ(q, Quantity{150});

    q -= Quantity{30};
    EXPECT_EQ(q, Quantity{120});
}

// The boundary case for a fill that exactly exhausts a resting order. It must
// land on zero cleanly, not wrap, and the result must report itself invalid so
// the book knows the order is done.
TEST(Quantity, SubtractingTheFullAmountYieldsAnInvalidZero) {
    const Quantity remaining = Quantity{250} - Quantity{250};

    EXPECT_EQ(remaining, Quantity{});
    EXPECT_FALSE(remaining.is_valid());
}

TEST(Quantity, OrdersByValue) {
    EXPECT_LT(Quantity{100}, Quantity{101});
    EXPECT_GT(Quantity{101}, Quantity{100});
    EXPECT_EQ(Quantity{100}, Quantity{100});
    EXPECT_NE(Quantity{100}, Quantity{101});
}

#ifndef NDEBUG
// Over-subtraction is the money bug this type exists to prevent, and nothing
// else catches it: unsigned wraparound is well-defined behaviour, so UBSan stays
// silent and -Wconversion has nothing to say. The assert in operator-= is the
// only guard.
//
// Compiled out under NDEBUG, where assert() is a no-op by design. The guard is
// a development check, not a release-time branch on the hot path.
TEST(QuantityDeathTest, SubtractingMoreThanAvailableAborts) {
    Quantity remaining{5};
    EXPECT_DEATH(remaining -= Quantity{6}, "wrap around");
}
#endif

// ---------------------------------------------------------------------------
// OrderId
// ---------------------------------------------------------------------------

// Zero is reserved so that a default-constructed id is detectably absent rather
// than colliding with a real order.
TEST(OrderId, DefaultConstructsToTheReservedAbsentValue) {
    EXPECT_EQ(OrderId{}.value(), OrderId::kNone);
    EXPECT_FALSE(OrderId{}.is_valid());
    EXPECT_FALSE(OrderId{OrderId::kNone}.is_valid());
}

TEST(OrderId, AnyNonZeroValueIsValid) {
    EXPECT_TRUE(OrderId{1}.is_valid());
    EXPECT_TRUE(OrderId{std::numeric_limits<OrderId::Rep>::max()}.is_valid());
}

TEST(OrderId, ComparesByValue) {
    EXPECT_EQ(OrderId{42}, OrderId{42});
    EXPECT_NE(OrderId{42}, OrderId{43});
    EXPECT_LT(OrderId{42}, OrderId{43});
}

}  // namespace
}  // namespace flashpoint

// Core scalar value types shared by every layer of the engine.
//
// These are deliberately small, trivially copyable, and carry no behaviour
// beyond their own algebra. Each wraps a single integer in a distinct type so
// that passing a quantity where a price belongs is a compile error rather than
// a silent, expensive mistake -- `-Wconversion` cannot help there, because both
// are 64-bit integers underneath.

#pragma once

#include <cassert>
#include <compare>
#include <cstdint>
#include <string_view>

namespace flashpoint {

// ---------------------------------------------------------------------------
// Side
// ---------------------------------------------------------------------------

/// Which side of the book an order rests on.
///
/// Fixed to std::uint8_t so Side costs one byte inside Order rather than the
/// four an unsized enum would default to.
enum class Side : std::uint8_t {
    Buy = 0,
    Sell = 1,
};

/// The side an order of the given side trades against.
[[nodiscard]] constexpr Side opposite(Side side) noexcept {
    return side == Side::Buy ? Side::Sell : Side::Buy;
}

/// True if `side` holds one of the two defined enumerators.
///
/// An `enum class` does not constrain its value range: a static_cast from an
/// arbitrary integer yields a Side that is neither Buy nor Sell. Decoding
/// inbound wire data is exactly where that happens, which is why this exists.
[[nodiscard]] constexpr bool is_valid(Side side) noexcept {
    return side == Side::Buy || side == Side::Sell;
}

[[nodiscard]] constexpr std::string_view to_string(Side side) noexcept {
    switch (side) {
        case Side::Buy:
            return "Buy";
        case Side::Sell:
            return "Sell";
    }
    // Reached only for a Side produced by casting an out-of-range integer.
    return "Invalid";
}

// ---------------------------------------------------------------------------
// Price
// ---------------------------------------------------------------------------

/// A price, expressed as a signed integer value of ticks.
///
/// The engine matches on ticks and never sees a decimal. Tick size, and the
/// mapping from ticks to a displayed currency amount, are instrument
/// configuration. That keeps comparison exact (no floating point) and keeps
/// this type eight bytes.
///
/// Signed because negative prices exist: oil futures settled below zero
/// in 2020, and calendar-spread instruments quote negative routinely. Whether a
/// particular venue permits them is venue policy, checked once at the engine
/// boundary, not a property of this type.

class Price {
public:
    /// The underlying representation, named so that narrowing it (to shrink
    /// Order) is a one-line change here rather than an edit at every use site.
    using Rep = std::int64_t;

    constexpr Price() noexcept = default;

    constexpr explicit Price(Rep ticks) noexcept : ticks_(ticks) {}

    [[nodiscard]] constexpr Rep ticks() const noexcept {
        return ticks_;
    }

    friend constexpr bool operator==(Price, Price) noexcept = default;

    friend constexpr auto operator<=>(Price, Price) noexcept = default;

private:
    Rep ticks_ = 0;
};

// ---------------------------------------------------------------------------
// Quantity
// ---------------------------------------------------------------------------

/// A number of units: shares, lots, or contracts.
///
/// Unsigned, because a negative quantity is meaningless here and making it
/// unrepresentable is cheaper and more reliable than validating against it.
class Quantity {
public:
    using Rep = std::uint64_t;

    constexpr Quantity() noexcept = default;

    constexpr explicit Quantity(Rep units) noexcept : units_(units) {}

    [[nodiscard]] constexpr Rep value() const noexcept {
        return units_;
    }

    /// A zero quantity is the only malformed state reachable: the type already
    /// makes negatives unrepresentable.
    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return units_ > 0;
    }

    constexpr Quantity& operator+=(Quantity rhs) noexcept {
        units_ += rhs.units_;
        return *this;
    }

    /// Precondition: `rhs <= *this`.
    ///
    /// Unsigned subtraction wraps rather than trapping, and wraparound is *not*
    /// undefined behaviour, so UBSan will not flag it. This assert is the only
    /// thing between an over-fill bug and a resting quantity of 1.8e19. It costs
    /// nothing in release builds, where NDEBUG removes it entirely.
    constexpr Quantity& operator-=(Quantity rhs) noexcept {
        assert(rhs.units_ <= units_ && "Quantity subtraction would wrap around");
        units_ -= rhs.units_;
        return *this;
    }

    [[nodiscard]] friend constexpr Quantity operator+(Quantity lhs, Quantity rhs) noexcept {
        lhs += rhs;
        return lhs;
    }

    [[nodiscard]] friend constexpr Quantity operator-(Quantity lhs, Quantity rhs) noexcept {
        lhs -= rhs;
        return lhs;
    }

    friend constexpr bool operator==(Quantity, Quantity) noexcept = default;

    friend constexpr auto operator<=>(Quantity, Quantity) noexcept = default;

private:
    Rep units_ = 0;
};

// ---------------------------------------------------------------------------
// OrderId
// ---------------------------------------------------------------------------

/// Identifies an order uniquely within the engine.
///
/// Zero is reserved to mean "no order", so a default-constructed OrderId is
/// detectably absent rather than silently colliding with a real one.
///
/// No std::hash specialisation yet. Hashing matters when the book gains its
/// order-lookup map at Milestone 4.
class OrderId {
public:
    using Rep = std::uint64_t;

    /// Reserved value meaning "no order". Held by a default-constructed OrderId.
    static constexpr Rep kNone = 0;

    constexpr OrderId() noexcept = default;

    constexpr explicit OrderId(Rep value) noexcept : value_(value) {}

    [[nodiscard]] constexpr Rep value() const noexcept {
        return value_;
    }

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return value_ != kNone;
    }

    friend constexpr bool operator==(OrderId, OrderId) noexcept = default;

    friend constexpr auto operator<=>(OrderId, OrderId) noexcept = default;

private:
    Rep value_ = kNone;
};

}  // namespace flashpoint

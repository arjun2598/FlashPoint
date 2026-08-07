// Stream inserters for the core domain types.
//
// Kept out of types.hpp and order.hpp on purpose. <ostream> is a heavy header
// and instantiates a great deal of machinery; the engine's hot path shouldn't include it. 
// Tests, the demo, and diagnostic code include this
// header explicitly. The library itself never does.
//
// GoogleTest falls back to a
// raw byte dump for types with no inserter, which turns a failed
// EXPECT_EQ on two Prices into unreadable hex.

#pragma once

#include "flashpoint/order.hpp"
#include "flashpoint/types.hpp"

#include <ostream>

namespace flashpoint {

inline std::ostream& operator<<(std::ostream& os, Side side) {
    return os << to_string(side);
}

inline std::ostream& operator<<(std::ostream& os, Price price) {
    // The trailing 't' is because these are ticks, not currency.
    return os << price.ticks() << 't';
}

inline std::ostream& operator<<(std::ostream& os, Quantity quantity) {
    return os << quantity.value();
}

inline std::ostream& operator<<(std::ostream& os, OrderId id) {
    return os << '#' << id.value();
}

inline std::ostream& operator<<(std::ostream& os, const Order& order) {
    return os << "Order{" << order.id() << ", " << order.side() << ", " << order.price() << ", "
              << order.quantity() << '}';
}

}  // namespace flashpoint

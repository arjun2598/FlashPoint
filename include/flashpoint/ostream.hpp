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

#include "flashpoint/event.hpp"
#include "flashpoint/market_data.hpp"
#include "flashpoint/matching_engine.hpp"
#include "flashpoint/order.hpp"
#include "flashpoint/trade.hpp"
#include "flashpoint/types.hpp"

#include <ostream>

namespace flashpoint {

inline std::ostream& operator<<(std::ostream& os, Side side) {
    return os << to_string(side);
}

inline std::ostream& operator<<(std::ostream& os, OrderType type) {
    return os << to_string(type);
}

inline std::ostream& operator<<(std::ostream& os, TimeInForce tif) {
    return os << to_string(tif);
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
    return os << "Order{" << order.id() << ", " << order.side() << ", " << order.type() << ", "
              << order.price() << ", " << order.quantity() << ", " << order.time_in_force() << '}';
}

inline std::ostream& operator<<(std::ostream& os, const Trade& trade) {
    return os << "Trade{maker=" << trade.maker_id << ", taker=" << trade.taker_id << ", "
              << trade.price << ", " << trade.quantity << ", aggressor=" << trade.aggressor << '}';
}

inline std::ostream& operator<<(std::ostream& os, SubmitStatus status) {
    return os << to_string(status);
}

inline std::ostream& operator<<(std::ostream& os, CancelStatus status) {
    return os << to_string(status);
}

inline std::ostream& operator<<(std::ostream& os, QueuePriority priority) {
    return os << to_string(priority);
}

inline std::ostream& operator<<(std::ostream& os, ModifyStatus status) {
    return os << to_string(status);
}

inline std::ostream& operator<<(std::ostream& os, const ModifyResult& result) {
    return os << "ModifyResult{" << result.status << ", filled=" << result.filled
              << ", resting=" << result.resting << ", priority=" << result.priority << '}';
}

inline std::ostream& operator<<(std::ostream& os, SequenceNumber sequence) {
    return os << 'v' << sequence.value();
}

inline std::ostream& operator<<(std::ostream& os, EventType type) {
    return os << to_string(type);
}

inline std::ostream& operator<<(std::ostream& os, RejectReason reason) {
    return os << to_string(reason);
}

inline std::ostream& operator<<(std::ostream& os, const Event& event) {
    os << "Event{" << event.sequence << ' ' << event.type << ' ' << event.order_id;
    if (event.type == EventType::Trade) {
        os << " vs " << event.counterparty_id;
    }
    if (event.type == EventType::Rejected) {
        return os << ' ' << event.reason << '}';
    }
    os << ' ' << event.side << ' ' << event.price << " x " << event.quantity;
    if (event.type == EventType::Modified) {
        os << ' ' << event.priority;
    }
    return os << '}';
}

inline std::ostream& operator<<(std::ostream& os, const LevelSnapshot& level) {
    return os << "Level{" << level.price << " x " << level.quantity << " in " << level.order_count
              << " orders}";
}

inline std::ostream& operator<<(std::ostream& os, const TopOfBook& top) {
    os << "TopOfBook{";
    if (top.has_bid()) {
        os << top.bid_quantity << " @ " << *top.bid_price;
    } else {
        os << "no bid";
    }
    os << " | ";
    if (top.has_ask()) {
        os << top.ask_quantity << " @ " << *top.ask_price;
    } else {
        os << "no ask";
    }
    return os << '}';
}

inline std::ostream& operator<<(std::ostream& os, const RestingOrder& order) {
    return os << "RestingOrder{" << order.id << ", " << order.side << ", " << order.price << ", "
              << order.remaining << '}';
}

inline std::ostream& operator<<(std::ostream& os, const CancelResult& result) {
    return os << "CancelResult{" << result.status << ", cancelled=" << result.cancelled << '}';
}

inline std::ostream& operator<<(std::ostream& os, const SubmitResult& result) {
    return os << "SubmitResult{" << result.status << ", filled=" << result.filled
              << ", resting=" << result.resting << ", cancelled=" << result.cancelled << '}';
}

}  // namespace flashpoint

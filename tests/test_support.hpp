// Shared helpers for the engine tests.
//
// Every mutating engine call takes an event sink. Most tests only care about a
// subset of the stream, so this records everything and offers filters.

#pragma once

#include "flashpoint/event.hpp"
#include "flashpoint/trade.hpp"
#include "flashpoint/types.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace flashpoint {
namespace testing_support {

/// A sink that discards everything, for tests that only check the result value.
inline void ignore_events(const Event&) {}

/// Records the whole stream so a test can assert on order and content.
struct EventRecorder {
    std::vector<Event> events;

    void operator()(const Event& event) {
        events.push_back(event);
    }

    [[nodiscard]] std::vector<Event> of_type(EventType type) const {
        std::vector<Event> matching;
        std::copy_if(events.begin(), events.end(), std::back_inserter(matching),
                     [type](const Event& event) { return event.type == type; });
        return matching;
    }

    [[nodiscard]] std::size_t count_of(EventType type) const {
        return static_cast<std::size_t>(
            std::count_if(events.begin(), events.end(),
                          [type](const Event& event) { return event.type == type; }));
    }

    /// The executions, in the order they happened.
    [[nodiscard]] std::vector<Trade> trades() const {
        std::vector<Trade> executions;
        for (const Event& event : events) {
            if (event.type == EventType::Trade) {
                executions.push_back(to_trade(event));
            }
        }
        return executions;
    }

    [[nodiscard]] std::vector<EventType> types() const {
        std::vector<EventType> sequence;
        sequence.reserve(events.size());
        for (const Event& event : events) {
            sequence.push_back(event.type);
        }
        return sequence;
    }
};

}  // namespace testing_support
}  // namespace flashpoint

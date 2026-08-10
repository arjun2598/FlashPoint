// Turning engine output into something readable in a terminal.

#pragma once

#include "flashpoint/event.hpp"
#include "flashpoint/order_book.hpp"

#include <cstddef>
#include <iosfwd>
#include <string>

namespace flashpoint::demo {

/// One line describing an event, in the order the engine published it.
///
/// Deliberately not the `ostream.hpp` inserter, which prints every field for
/// debugging. This shows only what each event type actually carries, so the
/// output reads like a trading system's log rather than a struct dump.
[[nodiscard]] std::string format_event(const Event& event);

/// Draws both sides of the book side by side, best price innermost, with bars
/// scaled to the largest quantity on display.
///
/// `depth` is how many price levels to show per side.
void print_book(std::ostream& out, const OrderBook& book, std::size_t depth);

/// A heading, for scenarios that narrate themselves.
void print_heading(std::ostream& out, std::string_view text);

}  // namespace flashpoint::demo

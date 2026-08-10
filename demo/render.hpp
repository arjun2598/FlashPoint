// Turning engine output into something readable in a terminal.

#pragma once

#include "flashpoint/event.hpp"
#include "flashpoint/market_data.hpp"
#include "flashpoint/order_book.hpp"

#include "session.hpp"

#include <cstddef>
#include <iosfwd>
#include <span>
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

/// The same picture, from rows already copied out of a book.
///
/// The generator holds a snapshot rather than the book it came from, so the
/// drawing is separated from the fetching.
void print_levels(std::ostream& out, std::span<const LevelSnapshot> bids,
                  std::span<const LevelSnapshot> asks, const TopOfBook& top);

/// A heading, for scenarios that narrate themselves.
void print_heading(std::ostream& out, std::string_view text);

/// Reports a session to a terminal.
class TextReporter final : public Reporter {
public:
    explicit TextReporter(std::ostream& out) : out_(out) {}

    void heading(std::string_view text) override;
    void event(const Event& event) override;
    void error(std::string_view message) override;
    void show(const OrderBook& book, std::size_t depth) override;
    void prompt() override;

private:
    std::ostream& out_;
};

}  // namespace flashpoint::demo

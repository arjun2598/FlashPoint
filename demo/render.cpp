#include "render.hpp"

#include "flashpoint/market_data.hpp"
#include "flashpoint/types.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iomanip>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace flashpoint::demo {
namespace {

/// The widest book either side can show at once. More than this and the bars
/// stop being readable in a terminal.
constexpr std::size_t kMaxDepth = 20;

/// Length of the longest bar. Everything is scaled against the largest quantity
/// currently on screen, so the shape is relative, not absolute.
constexpr std::size_t kBarWidth = 12;

[[nodiscard]] std::string bar(Quantity quantity, Quantity largest) {
    if (largest.value() == 0) {
        return {};
    }
    const auto scaled = static_cast<std::size_t>(
        (quantity.value() * kBarWidth + largest.value() - 1) / largest.value());
    return std::string(std::min(scaled, kBarWidth), '#');
}

[[nodiscard]] std::string side_word(Side side) {
    return side == Side::Buy ? "buy" : "sell";
}

/// Column layout for one half of the ladder: bar, quantity, price.
constexpr int kBarColumn = 12;
constexpr int kQuantityColumn = 6;
constexpr int kPriceColumn = 7;
constexpr std::size_t kHalfWidth = kBarColumn + 1 + kQuantityColumn + 2 + kPriceColumn;

[[nodiscard]] std::string pad_to(std::string text, std::size_t width) {
    if (text.size() < width) {
        text.append(width - text.size(), ' ');
    }
    return text;
}

[[nodiscard]] std::string centre(std::string_view text, std::size_t width) {
    if (text.size() >= width) {
        return std::string{text};
    }
    const std::size_t left = (width - text.size()) / 2;
    return std::string(left, ' ') + std::string{text} +
           std::string(width - text.size() - left, ' ');
}

}  // namespace

std::string format_event(const Event& event) {
    std::ostringstream line;
    // Padded so the columns stay aligned once sequence numbers reach two digits.
    line << "  [" << std::setw(3) << event.sequence.value() << "] ";

    switch (event.type) {
        case EventType::Accepted:
            line << "ACCEPTED  #" << event.order_id.value() << "  " << side_word(event.side) << ' '
                 << event.quantity.value();
            if (event.price != Price{}) {
                line << " @ " << event.price.ticks();
            } else {
                line << " @ market";
            }
            break;

        case EventType::Trade:
            // The taker is named first, then who it traded against.
            line << "TRADE     #" << event.order_id.value() << " <- #"
                 << event.counterparty_id.value() << "  " << event.quantity.value() << " @ "
                 << event.price.ticks();
            break;

        case EventType::Cancelled:
            line << "CANCELLED #" << event.order_id.value() << "  " << event.quantity.value()
                 << " removed";
            break;

        case EventType::Modified:
            line << "MODIFIED  #" << event.order_id.value() << "  now " << event.quantity.value()
                 << " @ " << event.price.ticks() << "  (priority " << to_string(event.priority)
                 << ')';
            break;

        case EventType::Rejected:
            line << "REJECTED  #" << event.order_id.value() << "  " << to_string(event.reason);
            break;
    }

    return line.str();
}

void print_book(std::ostream& out, const OrderBook& book, std::size_t depth) {
    const std::size_t rows = std::min(depth == 0 ? std::size_t{5} : depth, kMaxDepth);

    std::array<LevelSnapshot, kMaxDepth> bid_rows{};
    std::array<LevelSnapshot, kMaxDepth> ask_rows{};
    const std::size_t bids =
        book.snapshot(Side::Buy, std::span<LevelSnapshot>{bid_rows.data(), rows});
    const std::size_t asks =
        book.snapshot(Side::Sell, std::span<LevelSnapshot>{ask_rows.data(), rows});

    // Bars are scaled against the largest quantity on screen, so the two sides
    // are directly comparable.
    Quantity largest{};
    for (std::size_t i = 0; i < bids; ++i) {
        largest = std::max(largest, bid_rows[i].quantity);
    }
    for (std::size_t i = 0; i < asks; ++i) {
        largest = std::max(largest, ask_rows[i].quantity);
    }

    const std::string rule(kHalfWidth, '-');

    out << '\n'
        << "  " << centre("B I D S", kHalfWidth) << '|' << centre("A S K S", kHalfWidth) << '\n';

    std::ostringstream head;
    head << std::setw(kBarColumn) << "" << ' ' << std::setw(kQuantityColumn) << "qty" << "  "
         << std::setw(kPriceColumn) << "price";
    out << "  " << pad_to(head.str(), kHalfWidth) << '|' << "  " << std::setw(kPriceColumn)
        << "price" << "  " << std::setw(kQuantityColumn) << "qty" << '\n';

    out << "  " << rule << '+' << rule << '\n';

    for (std::size_t i = 0; i < std::max(bids, asks); ++i) {
        std::ostringstream left;
        if (i < bids) {
            left << std::setw(kBarColumn) << bar(bid_rows[i].quantity, largest) << ' '
                 << std::setw(kQuantityColumn) << bid_rows[i].quantity.value() << "  "
                 << std::setw(kPriceColumn) << bid_rows[i].price.ticks();
        }
        out << "  " << pad_to(left.str(), kHalfWidth) << '|';

        if (i < asks) {
            out << "  " << std::setw(kPriceColumn) << ask_rows[i].price.ticks() << "  "
                << std::setw(kQuantityColumn) << ask_rows[i].quantity.value() << ' '
                << bar(ask_rows[i].quantity, largest);
        }
        out << '\n';
    }

    if (bids == 0 && asks == 0) {
        out << "  " << pad_to("(no bids)", kHalfWidth) << '|' << "  (no asks)\n";
    }

    out << "  " << rule << '+' << rule << '\n';

    const TopOfBook top = book.top_of_book();
    out << "  ";
    if (top.has_bid()) {
        out << "bid " << top.bid_price->ticks();
    } else {
        out << "no bid";
    }
    out << "   |   ";
    if (top.has_ask()) {
        out << "ask " << top.ask_price->ticks();
    } else {
        out << "no ask";
    }
    if (const auto spread = top.spread(); spread.has_value()) {
        out << "   |   spread " << *spread;
    }
    out << "   |   " << book.size() << (book.size() == 1 ? " order" : " orders") << " resting\n\n";
}

void print_heading(std::ostream& out, std::string_view text) {
    out << '\n' << text << '\n';
    out << std::string(text.size(), '=') << '\n';
}

}  // namespace flashpoint::demo

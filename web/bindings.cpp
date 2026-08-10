// The WebAssembly surface.
//
// Three entry points, each returning a JSON string the page renders. The engine
// itself needed no changes to run here: it has no I/O, no threads and no
// platform calls, and events already go to a caller-supplied sink, so a
// JavaScript callback would have been a valid sink too.
//
// JSON rather than handing the flat Event records across raw. That zero-encoding
// property is real and is why DD-034 chose the layout, but it pays off for a
// file or a socket, where the alternative is writing a serialiser. A browser has
// to build DOM nodes regardless, so encoding costs nothing here and the struct
// layout stays on one side of the language boundary.

#include "generator.hpp"
#include "session.hpp"

#include "flashpoint/event.hpp"
#include "flashpoint/market_data.hpp"
#include "flashpoint/order_book.hpp"
#include "flashpoint/trade.hpp"
#include "flashpoint/types.hpp"
#include "flashpoint/version.hpp"

#include <emscripten/emscripten.h>

#include <array>
#include <cstddef>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

namespace flashpoint::demo {
namespace {

// ---------------------------------------------------------------------------
// A very small JSON writer
//
// Enough for the shapes below and nothing more. Pulling in a JSON library for
// four object types would be the larger cost, and DD-005's one-command build
// applies to this target too.
// ---------------------------------------------------------------------------

void write_escaped(std::ostringstream& out, std::string_view text) {
    out << '"';
    for (const char c : text) {
        switch (c) {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                // Control characters must be escaped; everything else, including
                // UTF-8 continuation bytes, passes through unchanged.
                if (static_cast<unsigned char>(c) < 0x20) {
                    out << "\\u00" << "0123456789abcdef"[(c >> 4) & 0xF]
                        << "0123456789abcdef"[c & 0xF];
                } else {
                    out << c;
                }
        }
    }
    out << '"';
}

void write_levels(std::ostringstream& out, std::span<const LevelSnapshot> rows) {
    out << '[';
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (i > 0) {
            out << ',';
        }
        out << "{\"price\":" << rows[i].price.ticks()
            << ",\"quantity\":" << rows[i].quantity.value() << ",\"orders\":" << rows[i].order_count
            << '}';
    }
    out << ']';
}

void write_top(std::ostringstream& out, const TopOfBook& top) {
    out << "{\"hasBid\":" << (top.has_bid() ? "true" : "false");
    if (top.has_bid()) {
        out << ",\"bidPrice\":" << top.bid_price->ticks()
            << ",\"bidQuantity\":" << top.bid_quantity.value();
    }
    out << ",\"hasAsk\":" << (top.has_ask() ? "true" : "false");
    if (top.has_ask()) {
        out << ",\"askPrice\":" << top.ask_price->ticks()
            << ",\"askQuantity\":" << top.ask_quantity.value();
    }
    if (const auto spread = top.spread(); spread.has_value()) {
        out << ",\"spread\":" << *spread;
    }
    out << '}';
}

void write_book(std::ostringstream& out, const OrderBook& book, std::size_t depth) {
    std::array<LevelSnapshot, 32> bids{};
    std::array<LevelSnapshot, 32> asks{};
    const std::size_t bid_rows =
        book.snapshot(Side::Buy, std::span<LevelSnapshot>{bids.data(), depth});
    const std::size_t ask_rows =
        book.snapshot(Side::Sell, std::span<LevelSnapshot>{asks.data(), depth});

    out << "{\"bids\":";
    write_levels(out, std::span<const LevelSnapshot>{bids.data(), bid_rows});
    out << ",\"asks\":";
    write_levels(out, std::span<const LevelSnapshot>{asks.data(), ask_rows});
    out << ",\"top\":";
    write_top(out, book.top_of_book());
    out << ",\"resting\":" << book.size() << '}';
}

void write_event(std::ostringstream& out, const Event& event) {
    out << "{\"kind\":\"event\",\"sequence\":" << event.sequence.value() << ",\"type\":";
    write_escaped(out, to_string(event.type));
    out << ",\"order\":" << event.order_id.value() << ",\"quantity\":" << event.quantity.value()
        << ",\"side\":";
    write_escaped(out, to_string(event.side));

    // A market order has no price of its own, so the page shows "market" rather
    // than a zero it would otherwise have to interpret.
    out << ",\"hasPrice\":" << (event.price == Price{} ? "false" : "true")
        << ",\"price\":" << event.price.ticks();

    if (event.type == EventType::Trade) {
        out << ",\"counterparty\":" << event.counterparty_id.value();
    }
    if (event.type == EventType::Rejected) {
        out << ",\"reason\":";
        write_escaped(out, to_string(event.reason));
    }
    if (event.type == EventType::Modified) {
        out << ",\"priority\":";
        write_escaped(out, to_string(event.priority));
    }
    out << '}';
}

// ---------------------------------------------------------------------------
// The reporter the browser uses
// ---------------------------------------------------------------------------

/// Accumulates a session as an ordered list of JSON steps.
///
/// Order matters: the page replays headings, events and book snapshots in the
/// sequence they happened, which is how a scenario reads as a narrative rather
/// than a pile of results.
class JsonReporter final : public Reporter {
public:
    void heading(std::string_view text) override {
        separate();
        steps_ << "{\"kind\":\"heading\",\"text\":";
        write_escaped(steps_, text);
        steps_ << '}';
    }

    void event(const Event& event) override {
        separate();
        write_event(steps_, event);
    }

    void error(std::string_view message) override {
        separate();
        steps_ << "{\"kind\":\"error\",\"text\":";
        write_escaped(steps_, message);
        steps_ << '}';
    }

    void show(const OrderBook& book, std::size_t depth) override {
        separate();
        steps_ << "{\"kind\":\"book\",\"book\":";
        write_book(steps_, book, depth == 0 ? 5 : std::min<std::size_t>(depth, 32));
        steps_ << '}';
    }

    [[nodiscard]] std::string steps() const {
        return steps_.str();
    }

private:
    void separate() {
        if (any_) {
            steps_ << ',';
        }
        any_ = true;
    }

    std::ostringstream steps_;
    bool any_ = false;
};

/// Held across the call so the returned pointer stays valid until the next one.
std::string g_result;

}  // namespace
}  // namespace flashpoint::demo

extern "C" {

/// Replays a scenario script. Returns the steps in the order they happened.
EMSCRIPTEN_KEEPALIVE const char* fp_run_script(const char* text) {
    using namespace flashpoint;
    using namespace flashpoint::demo;

    JsonReporter reporter;
    Session session{reporter};

    std::istringstream input{text == nullptr ? std::string{} : std::string{text}};
    session.run(input, false);

    std::ostringstream out;
    out << "{\"steps\":[" << reporter.steps() << "],\"final\":";
    write_book(out, session.engine().book(), 10);
    out << ",\"hadError\":" << (session.had_error() ? "true" : "false") << '}';

    g_result = out.str();
    return g_result.c_str();
}

/// Runs the synthetic feed. Returns per-chunk throughput, event counts, the
/// most recent executions, and the closing book.
EMSCRIPTEN_KEEPALIVE const char* fp_generate(int orders, int seed, int protection_ticks,
                                             int keep_trades) {
    using namespace flashpoint;
    using namespace flashpoint::demo;

    GeneratorConfig config;
    config.orders = orders < 1 ? 1 : static_cast<std::size_t>(orders);
    config.seed = static_cast<std::uint32_t>(seed < 0 ? 0 : seed);
    config.protection_ticks = protection_ticks < 0 ? 0 : protection_ticks;
    config.keep_trades = keep_trades < 0 ? 0 : static_cast<std::size_t>(keep_trades);
    // Ten progress rows regardless of run size, so the trend is legible.
    config.chunk = config.orders / 10 + 1;

    const GeneratorResult result = run_generator(config);

    std::ostringstream out;
    out << "{\"submitted\":" << result.submitted << ",\"chunks\":[";
    for (std::size_t i = 0; i < result.chunks.size(); ++i) {
        const ChunkStats& chunk = result.chunks[i];
        if (i > 0) {
            out << ',';
        }
        out << "{\"orders\":" << chunk.orders_so_far
            << ",\"nsPerOrder\":" << chunk.nanoseconds_per_order
            << ",\"ordersPerSecond\":" << chunk.orders_per_second
            << ",\"resting\":" << chunk.resting << ",\"bidLevels\":" << chunk.bid_levels
            << ",\"askLevels\":" << chunk.ask_levels << '}';
    }

    out << "],\"tally\":{\"accepted\":" << result.tally.accepted
        << ",\"trades\":" << result.tally.trades << ",\"cancelled\":" << result.tally.cancelled
        << ",\"modified\":" << result.tally.modified << ",\"rejected\":" << result.tally.rejected
        << ",\"volume\":" << result.tally.volume.value()
        << ",\"sequences\":" << result.tally.sequences.value() << '}';

    out << ",\"trades\":[";
    for (std::size_t i = 0; i < result.recent_trades.size(); ++i) {
        const Trade& trade = result.recent_trades[i];
        if (i > 0) {
            out << ',';
        }
        out << "{\"maker\":" << trade.maker_id.value() << ",\"taker\":" << trade.taker_id.value()
            << ",\"price\":" << trade.price.ticks() << ",\"quantity\":" << trade.quantity.value()
            << ",\"aggressor\":";
        write_escaped(out, to_string(trade.aggressor));
        out << '}';
    }

    out << "],\"book\":{\"bids\":";
    write_levels(out, result.bids);
    out << ",\"asks\":";
    write_levels(out, result.asks);
    out << ",\"top\":";
    write_top(out, result.top);
    out << "}}";

    g_result = out.str();
    return g_result.c_str();
}

/// The scenario the terminal demo runs by default, so the page opens on it.
EMSCRIPTEN_KEEPALIVE const char* fp_default_scenario() {
    flashpoint::demo::g_result = std::string{flashpoint::demo::default_scenario()};
    return flashpoint::demo::g_result.c_str();
}

EMSCRIPTEN_KEEPALIVE const char* fp_version() {
    flashpoint::demo::g_result = std::string{flashpoint::version()};
    return flashpoint::demo::g_result.c_str();
}

}  // extern "C"

#include "session.hpp"

#include "default_scenario.hpp"
#include "render.hpp"

#include "flashpoint/event.hpp"
#include "flashpoint/order.hpp"
#include "flashpoint/types.hpp"

#include <charconv>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace flashpoint::demo {
namespace {

[[nodiscard]] std::vector<std::string_view> split(std::string_view line) {
    std::vector<std::string_view> words;
    std::size_t start = 0;

    while (start < line.size()) {
        while (start < line.size() && line[start] == ' ') {
            ++start;
        }
        std::size_t end = start;
        while (end < line.size() && line[end] != ' ') {
            ++end;
        }
        if (end > start) {
            words.push_back(line.substr(start, end - start));
        }
        start = end;
    }
    return words;
}

/// Parses a whole word as an integer. Rejects trailing junk, so "10x" is an
/// error rather than 10.
template <typename Integer>
[[nodiscard]] std::optional<Integer> parse_number(std::string_view word) {
    Integer value{};
    const auto* const end = word.data() + word.size();
    const auto result = std::from_chars(word.data(), end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<Side> parse_side(std::string_view word) {
    if (word == "buy") {
        return Side::Buy;
    }
    if (word == "sell") {
        return Side::Sell;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<TimeInForce> parse_time_in_force(std::string_view word) {
    if (word == "gtc") {
        return TimeInForce::GoodTillCancel;
    }
    if (word == "ioc") {
        return TimeInForce::ImmediateOrCancel;
    }
    if (word == "fok") {
        return TimeInForce::FillOrKill;
    }
    return std::nullopt;
}

[[nodiscard]] std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

constexpr std::string_view kUsage =
    "  add <id> <buy|sell> <price|market> <qty> [gtc|ioc|fok]\n"
    "  cancel <id>\n"
    "  modify <id> <price> <qty>\n"
    "  show [depth]\n"
    "  help\n"
    "  quit\n";

}  // namespace

std::string_view default_scenario() {
    return kDefaultScenario;
}

void Session::fail(std::string_view message) {
    out_ << "  ! " << message << '\n';
    had_error_ = true;
}

bool Session::execute(std::string_view line) {
    line = trim(line);

    // "##" narrates, "#" is a silent comment. That lets a scenario file explain
    // itself without a separate command.
    if (line.starts_with("##")) {
        print_heading(out_, trim(line.substr(2)));
        return true;
    }
    if (line.empty() || line.front() == '#') {
        return true;
    }

    const std::vector<std::string_view> words = split(line);
    if (words.empty()) {
        return true;
    }

    // Every mutating command publishes into the same sink, so the output shows
    // the engine's real event stream rather than a summary invented here.
    auto sink = [this](const Event& event) { out_ << format_event(event) << '\n'; };

    const std::string_view command = words[0];

    if (command == "quit" || command == "exit") {
        return false;
    }

    if (command == "help") {
        out_ << kUsage;
        return true;
    }

    if (command == "show") {
        std::size_t depth = 5;
        if (words.size() > 1) {
            const auto parsed = parse_number<std::size_t>(words[1]);
            if (!parsed.has_value()) {
                fail("show: depth must be a number");
                return true;
            }
            depth = *parsed;
        }
        print_book(out_, engine_.book(), depth);
        return true;
    }

    if (command == "add") {
        if (words.size() < 5 || words.size() > 6) {
            fail("add: expected <id> <buy|sell> <price|market> <qty> [gtc|ioc|fok]");
            return true;
        }

        const auto id = parse_number<OrderId::Rep>(words[1]);
        const auto side = parse_side(words[2]);
        const auto quantity = parse_number<Quantity::Rep>(words[4]);
        if (!id.has_value() || !side.has_value() || !quantity.has_value()) {
            fail("add: could not parse id, side or quantity");
            return true;
        }

        const bool is_market = words[3] == "market";
        std::optional<Price::Rep> price;
        if (!is_market) {
            price = parse_number<Price::Rep>(words[3]);
            if (!price.has_value()) {
                fail("add: price must be a number or the word 'market'");
                return true;
            }
        }

        // A market order defaults to immediate-or-cancel because it cannot rest.
        std::optional<TimeInForce> tif =
            is_market ? TimeInForce::ImmediateOrCancel : TimeInForce::GoodTillCancel;
        if (words.size() == 6) {
            tif = parse_time_in_force(words[5]);
            if (!tif.has_value()) {
                fail("add: time in force must be gtc, ioc or fok");
                return true;
            }
        }

        const Order order =
            is_market ? Order::market(OrderId{*id}, *side, Quantity{*quantity}, *tif)
                      : Order::limit(OrderId{*id}, *side, Price{*price}, Quantity{*quantity}, *tif);

        const SubmitResult result = engine_.submit(order, sink);
        if (!result.accepted()) {
            had_error_ = true;
        }
        return true;
    }

    if (command == "cancel") {
        if (words.size() != 2) {
            fail("cancel: expected <id>");
            return true;
        }
        const auto id = parse_number<OrderId::Rep>(words[1]);
        if (!id.has_value()) {
            fail("cancel: id must be a number");
            return true;
        }
        const CancelResult result = engine_.cancel(OrderId{*id}, sink);
        if (!result.succeeded()) {
            had_error_ = true;
        }
        return true;
    }

    if (command == "modify") {
        if (words.size() != 4) {
            fail("modify: expected <id> <price> <qty>");
            return true;
        }
        const auto id = parse_number<OrderId::Rep>(words[1]);
        const auto price = parse_number<Price::Rep>(words[2]);
        const auto quantity = parse_number<Quantity::Rep>(words[3]);
        if (!id.has_value() || !price.has_value() || !quantity.has_value()) {
            fail("modify: could not parse id, price or quantity");
            return true;
        }
        const ModifyResult result =
            engine_.modify(OrderId{*id}, Price{*price}, Quantity{*quantity}, sink);
        if (!result.modified()) {
            had_error_ = true;
        }
        return true;
    }

    fail(std::string{"unknown command: "} + std::string{command});
    return true;
}

void Session::run(std::istream& in, bool interactive) {
    if (interactive) {
        out_ << "FlashPoint demo. Type 'help' for commands, 'quit' to leave.\n";
    }

    std::string line;
    while (true) {
        if (interactive) {
            out_ << "> " << std::flush;
        }
        if (!std::getline(in, line)) {
            break;
        }
        if (!execute(line)) {
            break;
        }
    }

    if (interactive) {
        out_ << '\n';
    }
}

}  // namespace flashpoint::demo

// Reads commands from a stream and applies them to an engine.
//
// The same parser serves every front end. Given a file it replays a scenario;
// given a terminal it is an interactive prompt; given a browser it drives the
// WebAssembly build. What differs is only where the results go, which is what
// `Reporter` abstracts.

#pragma once

#include "flashpoint/event.hpp"
#include "flashpoint/matching_engine.hpp"
#include "flashpoint/order_book.hpp"

#include <cstddef>
#include <iosfwd>
#include <string_view>

namespace flashpoint::demo {

/// Where a session's output goes.
///
/// Virtual dispatch is fine here: this is the command path, not the matching
/// path, and one indirect call per line is nothing next to parsing it.
class Reporter {
public:
    Reporter() = default;
    Reporter(const Reporter&) = delete;
    Reporter& operator=(const Reporter&) = delete;
    Reporter(Reporter&&) = delete;
    Reporter& operator=(Reporter&&) = delete;
    virtual ~Reporter() = default;

    /// A `##` line from a scenario, used to narrate.
    virtual void heading(std::string_view text) = 0;

    /// One event published by the engine, in the order it happened.
    virtual void event(const Event& event) = 0;

    /// A line that could not be parsed, or a request the engine refused.
    virtual void error(std::string_view message) = 0;

    /// A `show` command: the book as it stands.
    virtual void show(const OrderBook& book, std::size_t depth) = 0;

    /// Interactive only. A terminal prints a prompt; other front ends ignore it.
    virtual void prompt() {}
};

class Session {
public:
    explicit Session(Reporter& reporter) : reporter_(reporter) {}

    /// Reads and executes lines until the stream ends or `quit` is seen.
    ///
    /// When `interactive` is true, asks the reporter for a prompt before each
    /// line. Either way a bad line is reported and reading continues, so a
    /// broken script shows every problem rather than only the first.
    void run(std::istream& in, bool interactive);

    /// Executes a single line. Returns false to stop reading.
    bool execute(std::string_view line);

    /// True if any line failed to parse or was rejected by the engine.
    [[nodiscard]] bool had_error() const noexcept {
        return had_error_;
    }

    [[nodiscard]] const MatchingEngine& engine() const noexcept {
        return engine_;
    }

private:
    void fail(std::string_view message);

    MatchingEngine engine_;
    Reporter& reporter_;
    bool had_error_ = false;
};

/// The scenario built into the binary, run when no file is given.
[[nodiscard]] std::string_view default_scenario();

}  // namespace flashpoint::demo

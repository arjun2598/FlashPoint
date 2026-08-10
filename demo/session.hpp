// Reads commands from a stream and applies them to an engine.
//
// The same parser serves both modes. Given a file it replays a scenario; given
// a terminal it is an interactive prompt. Nothing about the parsing differs, so
// the two are one piece of code rather than two.

#pragma once

#include "flashpoint/matching_engine.hpp"

#include <iosfwd>
#include <string_view>

namespace flashpoint::demo {

class Session {
public:
    explicit Session(std::ostream& out) : out_(out) {}

    /// Reads and executes lines until the stream ends or `quit` is seen.
    ///
    /// When `interactive` is true, prints a prompt and keeps going after a bad
    /// line. A scenario file also keeps going, but reports at the end that
    /// something failed so a broken script cannot pass silently.
    void run(std::istream& in, bool interactive);

    /// True if any line failed to parse or was rejected by the engine.
    [[nodiscard]] bool had_error() const noexcept {
        return had_error_;
    }

    [[nodiscard]] const MatchingEngine& engine() const noexcept {
        return engine_;
    }

private:
    /// Executes one line. Returns false to stop reading.
    bool execute(std::string_view line);

    void fail(std::string_view message);

    MatchingEngine engine_;
    std::ostream& out_;
    bool had_error_ = false;
};

/// The scenario built into the binary, run when no file is given.
[[nodiscard]] std::string_view default_scenario();

}  // namespace flashpoint::demo

// FlashPoint demo.
//
//   flashpoint_demo                    run the built-in tour
//   flashpoint_demo scenario.txt       replay a script
//   flashpoint_demo -i                 interactive prompt
//   cat script | flashpoint_demo -     replay from stdin
//   flashpoint_demo --generate 1000000 synthetic feed at volume
//
// The script and interactive modes are the same parser reading different
// streams, which is why both exist for the price of one.

#include "generator.hpp"
#include "session.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kUsage =
    "FlashPoint demo\n"
    "\n"
    "  flashpoint_demo                     run the built-in tour\n"
    "  flashpoint_demo <file>              replay a scenario script\n"
    "  flashpoint_demo -i                  interactive prompt\n"
    "  cat script | flashpoint_demo -      replay from standard input\n"
    "  flashpoint_demo --generate <n>      synthetic feed of n orders\n"
    "\n"
    "Options for --generate:\n"
    "  --seed <n>    random seed (default 1)\n"
    "  --chunk <n>   orders per timing chunk (default 100000)\n";

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

}  // namespace

int main(int argc, char** argv) {
    using flashpoint::demo::GeneratorConfig;
    using flashpoint::demo::Session;

    const std::vector<std::string_view> args{argv + 1, argv + argc};

    bool interactive = false;
    bool generate = false;
    GeneratorConfig config;
    std::string_view path;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];

        auto next_number = [&](auto& target, std::string_view name) {
            if (i + 1 >= args.size()) {
                std::cerr << name << " needs a value\n";
                return false;
            }
            const auto parsed = parse_number<std::remove_reference_t<decltype(target)>>(args[++i]);
            if (!parsed.has_value()) {
                std::cerr << name << " needs a number\n";
                return false;
            }
            target = *parsed;
            return true;
        };

        if (arg == "-h" || arg == "--help") {
            std::cout << kUsage;
            return 0;
        }
        if (arg == "-i" || arg == "--interactive") {
            interactive = true;
        } else if (arg == "--generate") {
            generate = true;
            if (!next_number(config.orders, "--generate")) {
                return 2;
            }
        } else if (arg == "--seed") {
            if (!next_number(config.seed, "--seed")) {
                return 2;
            }
        } else if (arg == "--chunk") {
            if (!next_number(config.chunk, "--chunk")) {
                return 2;
            }
        } else if (arg == "-") {
            // Explicit request to read standard input.
            path = arg;
        } else if (arg.starts_with('-')) {
            std::cerr << "unknown option: " << arg << "\n\n" << kUsage;
            return 2;
        } else {
            path = arg;
        }
    }

    if (generate) {
        if (config.orders == 0 || config.chunk == 0) {
            std::cerr << "--generate and --chunk must be greater than zero\n";
            return 2;
        }
        flashpoint::demo::run_generator(std::cout, config);
        return 0;
    }

    Session session{std::cout};

    if (path == "-") {
        session.run(std::cin, false);
    } else if (!path.empty()) {
        std::ifstream file{std::string{path}};
        if (!file) {
            std::cerr << "cannot open " << path << '\n';
            return 2;
        }
        session.run(file, false);
    } else if (interactive) {
        session.run(std::cin, true);
    } else {
        // Nothing asked for, so run the tour. Reading standard input here on the
        // guess that it might be a pipe would hang whenever it is an open pipe
        // with nothing on it, which is what happens under CI and make.
        std::istringstream tour{std::string{flashpoint::demo::default_scenario()}};
        session.run(tour, false);
    }

    return session.had_error() ? 1 : 0;
}

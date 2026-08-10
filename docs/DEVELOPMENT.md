# Developer Setup

## Requirements

| Tool | Minimum | Notes |
|------|---------|-------|
| CMake | 3.25 | `SYSTEM` on `FetchContent_Declare` requires 3.25 |
| C++ compiler | GCC 11+, Clang 14+, or Apple Clang 14+ | C++20 required |
| Git | any recent | GoogleTest is fetched at configure time |

### macOS

```bash
xcode-select --install
brew install cmake
```

### Ubuntu / Debian

```bash
sudo apt-get install build-essential cmake clang
```

### clang-format (version-pinned)

**Do not install `clang-format` from Homebrew or apt.** Its output changes
between major releases, so a system-provided formatter will disagree with CI and
your build will fail on code you never touched.

The required version lives in [`.clang-format-version`](../.clang-format-version).
CI installs exactly that version; install the same one locally:

```bash
pipx install "clang-format==$(cat .clang-format-version)"
```

No `pipx`? A virtualenv works identically:

```bash
python3 -m venv .venv && .venv/bin/pip install "clang-format==$(cat .clang-format-version)"
```

Confirm you match CI:

```bash
clang-format --version && cat .clang-format-version
```

### clang-tidy (optional)

Not enforced in CI, so the version does not need pinning. `brew install llvm` on
macOS (add `/opt/homebrew/opt/llvm/bin` to your `PATH`) or
`sudo apt-get install clang-tidy` on Debian/Ubuntu.

## Building

The project ships CMake presets. Nothing else needs installing — GoogleTest is
fetched automatically on the first configure (~30s).

Development build (Debug, AddressSanitizer + UndefinedBehaviorSanitizer):

```bash
cmake --preset dev && cmake --build --preset dev --parallel && ctest --preset dev
```

Optimised build (use this, and only this, for benchmarks):

```bash
cmake --preset release && cmake --build --preset release --parallel && ctest --preset release
```

Profiling build (optimised, with symbols):

```bash
cmake --preset relwithdebinfo && cmake --build --preset relwithdebinfo --parallel
```

Build trees go to `build/<preset>/` and are git-ignored. To start clean, delete
that directory.

## Running tests

`ctest` registers each `TEST()` individually, so a failure names the case.

```bash
ctest --preset dev
```

Run a subset by name, or run the binary directly for GoogleTest's own filtering:

```bash
ctest --preset dev -R Version
```

```bash
./build/dev/tests/flashpoint_tests --gtest_filter='Version.*'
```

## Formatting

CI fails on unformatted code. Format everything before committing:

```bash
git ls-files '*.cpp' '*.hpp' | xargs clang-format -i
```

Check without modifying — this is exactly what CI runs:

```bash
git ls-files '*.cpp' '*.hpp' | xargs clang-format --dry-run --Werror
```

Two things to know about this gate:

- **Your `clang-format` must match `.clang-format-version`.** A different major
  version formats differently and will disagree with CI. See the setup section
  above.
- **`.hpp.in` templates are excluded** — their `@VAR@` placeholders are not valid
  C++ tokens and `clang-format` mangles them. The trade-off is that templates are
  not format-checked at all, so keep them tidy by hand.

The most common violation is a missing namespace-closing comment. `.clang-format`
sets `FixNamespaceComments: true`, so every namespace must close as
`}  // namespace flashpoint`, not a bare `}`. Running `clang-format -i` adds
these automatically.

## Running the demo

Built by default, in any preset.

```bash
cmake --preset dev && cmake --build --preset dev --parallel
```

The built-in tour, which is what someone who just cloned the repository wants:

```bash
./build/dev/demo/flashpoint_demo
```

A scenario file of your own, or an interactive prompt:

```bash
./build/dev/demo/flashpoint_demo demo/scenarios/tour.txt
```

```bash
./build/dev/demo/flashpoint_demo -i
```

Reading standard input is deliberately explicit. Guessing from `isatty()` hangs
whenever standard input is an open pipe with nothing on it, which is what happens
under CI and `make` (DD-044):

```bash
cat demo/scenarios/tour.txt | ./build/dev/demo/flashpoint_demo -
```

Synthetic flow at volume. Use the release build; the dev preset's sanitizers cost
2–20×:

```bash
./build/release/demo/flashpoint_demo --generate 2000000 --seed 7
```

### Writing a scenario

One command per line. `#` is a silent comment, `##` prints as a heading, which is
how `demo/scenarios/tour.txt` narrates itself.

```
add <id> <buy|sell> <price|market> <qty> [gtc|ioc|fok]
cancel <id>
modify <id> <price> <qty>
show [depth]
help
quit
```

## Benchmarks

Benchmarks only mean anything in a Release build, so they are off in the `dev`
preset and on in `release` and `relwithdebinfo`. Building them fetches Google
Benchmark.

```bash
cmake --preset release && cmake --build --preset release --parallel
```

Latency distribution, per operation, at two book shapes:

```bash
./build/release/benchmarks/flashpoint_latency_bench
```

Sustained throughput:

```bash
./build/release/benchmarks/flashpoint_throughput_bench --benchmark_min_time=1s
```

Read the caveats in [`PERFORMANCE.md`](PERFORMANCE.md) before quoting anything
either one prints. In particular there is a measurement floor of roughly 42 ns
on Apple Silicon, and macOS gives no way to pin cores or fix the clock speed.

CI builds both targets but never runs them: a shared runner cannot produce a
number worth asserting on (DD-040).

## Static analysis

`clang-tidy` is configured but not yet enforced in CI. The `dev` preset writes
`compile_commands.json`, so:

```bash
run-clang-tidy -p build/dev
```

## Build options

All are settable with `-D` on the configure line; the presets set sensible
defaults.

| Option | Default | Effect |
|--------|---------|--------|
| `FLASHPOINT_BUILD_TESTS` | `ON` when top-level | Build the test executable |
| `FLASHPOINT_WARNINGS_AS_ERRORS` | `ON` when top-level | Add `-Werror` |
| `FLASHPOINT_ENABLE_SANITIZERS` | `OFF` | ASan + UBSan, no recovery |
| `FLASHPOINT_BUILD_BENCHMARKS` | `OFF`, `ON` in the release presets | Build the benchmark targets |
| `FLASHPOINT_BUILD_DEMO` | `ON` when top-level | Build the demo |

Both defaults fall to `OFF` when FlashPoint is consumed via `add_subdirectory()`,
so a downstream project inherits neither our tests nor our warning policy.

## Notes and gotchas

- **In-source builds are rejected** with a clear error. Configure into `build/`.
- **An unset `CMAKE_BUILD_TYPE` defaults to `Release`**, so nobody accidentally
  benchmarks an unoptimised binary.
- **`-Wconversion` and `-Wsign-conversion` are on.** Implicit narrowing between
  integer types is an error. This is deliberate — see DD-007 in
  [`DESIGN_DECISIONS.md`](DESIGN_DECISIONS.md). Add an explicit cast, or better,
  fix the type.
- **Never benchmark the `dev` preset.** Sanitizers cost 2–20×.

# Rift

The Rift programming language - C++-inspired, but explicitly simpler,
safer, and more consistent.

**Status:** M0 - scaffolding. The `riftc` binary builds and the test
harness runs; lexing and beyond land in M1+.

See [`docs/roadmap.md`](docs/roadmap.md) for the milestone plan and
[`docs/M0-scope.md`](docs/M0-scope.md) for what this milestone delivers.

## Building

Requires:

- CMake ≥ 3.25
- Ninja (recommended) or Make
- Clang 18+ or GCC 14+, with `-std=c++2c` support
- Python 3.10+ (test runner only)

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

The resulting binary is at `build/compiler/riftc`:

```bash
$ ./build/compiler/riftc --version
riftc 0.0.1 (M0)
```

## Testing

```bash
ctest --test-dir build --output-on-failure
```

Or invoke the runner directly:

```bash
python3 tests/run_tests.py --riftc build/compiler/riftc
```

The runner discovers both driver smoke tests (built in) and golden
`.rfts` tests under `tests/rfts/`. Golden tests are empty until M4
delivers codegen; the harness is ready for them.

## Layout

| Path | Purpose |
|---|---|
| `compiler/` | The `riftc` binary. Will split into `librift` + `riftc` driver in M1. |
| `tests/` | Test runner (`run_tests.py`) and golden inputs. |
| `examples/` | Example `.rfts` programs. |
| `docs/` | Milestone scope, roadmap. |

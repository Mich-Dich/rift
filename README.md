# Rift

The Rift programming language - C++-inspired, but explicitly simpler,
safer, and more consistent. The language design lives in [`docs/design/`](docs/design/README.md).

**Status:** M1 - lexer. `riftc` reads `.rifts`/`.rifth` files, tokenizes them per
spec 01, and reports lexical errors with stable diagnostic codes. Parsing comes next.

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

The binary is `build/compiler/riftc`:

```bash
$ ./build/compiler/riftc --version
riftc 0.0.1
$ ./build/compiler/riftc check examples/employees.rifts     # silent + exit 0 when clean
$ ./build/compiler/riftc lex examples/compile_check.rifts   # token dump
$ ./build/compiler/riftc explain E1003                      # what a diagnostic code means
```

## Testing

```bash
scripts/test.sh                 # build + run unit, feature and action tests
scripts/test.sh --suite feature # one suite
scripts/test.sh lexer           # filter by name/path
scripts/test.sh --update        # re-record golden files for action tests
```

| Suite | What | Where |
|---|---|---|
| unit | C++ tests of compiler internals | `tests/unit/test_*.cpp` |
| feature | good/bad `.rifts` files per spec rule, inline `//~ error[E1009]` annotations | `tests/feature/<chapter>/` |
| action | real `riftc` runs; stdout, stderr and exit code compared with golden files | `tests/action/**/*.rifts` + `.expected` |

Details, file formats and the `XFAIL` workflow for not-yet-implemented features:
[`docs/testing.md`](docs/testing.md). CTest runs the same suites:
`ctest --test-dir build --output-on-failure`.

## Layout

| Path | Purpose |
|---|---|
| `compiler/include/rift/` | Public headers of `librift` (source map, diagnostics, tokens, lexer, driver) |
| `compiler/include/rift/diagnostic_codes.def` | Every diagnostic code, in one table |
| `compiler/include/rift/token_kinds.def` | Every token kind, keyword and operator |
| `compiler/src/` | `librift` implementation |
| `compiler/driver/main.cpp` | The `riftc` executable (thin wrapper around `rift::run_driver`) |
| `tests/run_tests.py` | Test runner for all three suites |
| `tests/unit/`, `tests/feature/`, `tests/action/` | The test suites |
| `scripts/test.sh` | Build + test in one command |
| `examples/` | Example `.rifts` programs |
| `docs/design/` | Language specification (v0.3) |

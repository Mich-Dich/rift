# Rift

The Rift programming language - C++-inspired, but explicitly simpler,
safer, and more consistent. The language design lives in [`docs/design/`](docs/design/README.md).

**Status:** M6 - code generation. `riftc build` turns a program into a native executable:
the checked program is translated to C and compiled with the system's GCC or Clang. The
supported core is integers and floats with checked arithmetic (overflow, division by zero and
bad shifts panic, spec 16), `bool`, `char`, string literals, structs and classes with methods,
enums with and without payloads, `match`, `if`/blocks as values, `while`, `for` over ranges,
references, globals, several modules, and `std::print`/`println`/`eprintln`/`panic` with
f-strings. Everything else (the standard library, templates, lambdas, inheritance,
destructors, ...) is reported as E6001 where it is used. All front-end checks of M1-M5 run
before code generation.

## Building

Requires:

- CMake ≥ 3.25
- Ninja (recommended) or Make
- Clang 18+ or GCC 14+, with `-std=c++2c` support
- LLVM 18 development files (`llvm-18-dev`, plus `libzstd-dev` for its CMake exports); the backend emits LLVM IR
- Python 3.10+ (test runner only)
- To link Rift programs: a system linker driver such as GCC or Clang as `cc` (or set `RIFT_CC` / `--cc=`)

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

The binary is `build/compiler/riftc`:

```bash
$ ./build/compiler/riftc --version
riftc 0.0.1
$ ./build/compiler/riftc run examples/hello.rifts           # build in a temp dir and run
$ ./build/compiler/riftc build examples/hello.rifts         # -> examples/build/debug/hello
$ ./build/compiler/riftc build --mode=release examples/hello.rifts   # -> examples/build/release/hello
$ ./build/compiler/riftc emit-llvm examples/hello.rifts      # the generated LLVM IR
$ ./build/compiler/riftc check examples/employees.rifts     # silent + exit 0 when clean
$ ./build/compiler/riftc lex examples/compile_check.rifts   # token dump
$ ./build/compiler/riftc parse examples/employees.rifts     # syntax tree (S-expressions)
$ ./build/compiler/riftc symbols examples/employees.rifts   # merged module symbol table
$ ./build/compiler/riftc typed examples/employees.rifts     # syntax tree with every expression's type
$ ./build/compiler/riftc check --root=proj proj/main.rifts  # project root for imports
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
| action | real `riftc` runs; stdout, stderr and exit code compared with golden files (`tests/action/run/` runs compiled programs) | `tests/action/**/*.rifts` + `.expected` |

Details, file formats and the `XFAIL` workflow for not-yet-implemented features:
[`docs/testing.md`](docs/testing.md). CTest runs the same suites:
`ctest --test-dir build --output-on-failure`.

## Layout

| Path | Purpose |
|---|---|
| `compiler/include/rift/` | Public headers of `librift` (source map, diagnostics, tokens, lexer, AST, parser, driver) |
| `compiler/include/rift/ast.hpp` | The syntax tree; `ast_printer.hpp` prints it for `riftc parse` |
| `compiler/include/rift/symbols.hpp` | Modules and symbols; `modules.hpp` loads imports, `resolve.hpp` resolves names |
| `compiler/include/rift/types.hpp` | Types; `check.hpp` is the type checker, `ownership.hpp` the ownership checker |
| `compiler/include/rift/llvm_backend.hpp` | The LLVM backend: checked program to LLVM IR, object file, executable; the runtime is LLVM IR in `src/llvm_runtime.cpp` |
| `compiler/include/rift/diagnostic_codes.def` | Every diagnostic code, in one table |
| `compiler/include/rift/token_kinds.def` | Every token kind, keyword and operator |
| `compiler/src/` | `librift` implementation |
| `compiler/driver/main.cpp` | The `riftc` executable (thin wrapper around `rift::run_driver`) |
| `tests/run_tests.py` | Test runner for all three suites |
| `tests/unit/`, `tests/feature/`, `tests/action/` | The test suites |
| `scripts/test.sh` | Build + test in one command |
| `examples/` | Example `.rifts` programs |
| `docs/design/` | Language specification (v0.3) |

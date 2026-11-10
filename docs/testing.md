# Testing the Rift compiler

Everything runs from bash and prints plain text. One command builds and runs all suites:

```bash
scripts/test.sh
```

```
unit     60/60 ok
feature  17/17 ok, 3 xfail
action   18/18 ok
ALL TESTS PASSED
```

Only failures are printed in detail; `-v` lists every test. Exit status is `0` only if the
build succeeds and every test passes.

| Command | Does |
|---|---|
| `scripts/test.sh` | configure (first time), build, run all suites |
| `scripts/test.sh --suite unit` | one suite: `unit`, `feature` or `action` (repeatable) |
| `scripts/test.sh lexer` | only tests whose name or path contains `lexer` |
| `scripts/test.sh --update` | re-record action-test golden files, then review with `git diff` |
| `scripts/test.sh -v` | print every test, not only failures |
| `CXX=clang++ BUILD_DIR=build-clang scripts/test.sh` | separate build with another compiler |
| `ctest --test-dir build --output-on-failure` | the same three suites through CTest |
| `build/tests/rift_unit_tests [filter] [-v] [--list]` | the unit test binary directly |

## The three suites

### 1. Unit tests: `tests/unit/test_*.cpp`

C++ tests of compiler internals (source map, UTF-8, diagnostics, lexer, driver), linked
against `librift`. They use a tiny in-repo framework (`tests/unit/framework/rift_test.hpp`),
no external dependencies:

```cpp
TEST_CASE("lexer: integer suffixes") {
    Lexed l("42u32");
    REQUIRE_EQ(l.tokens.size(), 2u);                 // aborts this test case on failure
    CHECK_EQ(l.tokens[0].suffix, std::string_view("u32"));   // records and continues
}
```

Any new `tests/unit/test_*.cpp` file is picked up automatically (re-run CMake configure; the
glob uses `CONFIGURE_DEPENDS`, so a normal build does it). A failure prints the file, line,
expression and both values:

```
FAIL    unit     lexer: ranges and method calls are not floats
      at tests/unit/test_lexer.cpp:193
          tests/unit/test_lexer.cpp:195: CHECK_EQ(kinds("0..=255"), ...) failed
            left:  "int_literal dot_dot eq int_literal"
            right: "int_literal dot_dot_eq int_literal"
```

Use unit tests for **one component in isolation**, edge cases, and anything awkward to
express as a source file.

### 2. Feature tests: `tests/feature/<spec-chapter>/*.rifts`

One file per language rule, grouped by spec chapter (`01-lexical`, `02-types`, ...). The
runner compiles the file with `riftc check --diagnostic-format=short` and compares the
diagnostics with **inline annotations**:

```rift
u32 a = 0x;          //~ error[E1009]
u32 b = 0b102;       //~ error[E1010]: invalid digit '2'
u32 c = `5`;         //~ error[E1001]
//~^ error[E1001]                       ^ = one line up (^^ two), v = one line down
/* a /* b */         //~ warning[W1001]
```

* Annotation: `//~ <error|warning>[<code>]: <message substring>`; code and message are optional.
* Every error/warning must be annotated, and every annotation must be produced. Order does
  not matter; notes are ignored.
* No error annotations → the file must compile (exit 0). That is how **good files** are written.
* The annotation is read from the raw text, so it works even inside an unterminated comment
  or string.

Use feature tests to **pin down the spec**: one good file showing what is allowed and one
bad file showing what is rejected.

**Stopping early.** `// STOP-AFTER: parse` (or `lex`, `resolve`, `types`) runs only the phases up to
that one. Syntax tests use it, because their snippets refer to names that are never declared.

**Multi-file tests.** A directory whose name ends in `_project` is one test: `main.rifts` is
the entry, the directory is the project root (`--root`), and annotations in *every* file of
the directory are matched against diagnostics in that file:

```
tests/feature/09-modules/visibility_project/
    main.rifts        import lib;  ...  lib::account{ .amount = 5 };   //~ error[E3036]
    lib.rifth         the module's interface
    lib.rifts         its implementation
```

Files inside a `_project` directory are never run as tests on their own, in either suite, so
action tests can use them as fixtures:
`// RUN: riftc check --root=tests/feature/09-modules/counter_project tests/feature/09-modules/counter_project/main.rifts`.

### 3. Action tests: `tests/action/**/*.rifts` + `.expected`

End-to-end runs of the real `riftc` binary. Each test names its command line; the runner
compares **stdout, stderr and exit code exactly** with the golden file next to it:

```rift
// RUN: riftc check %s
u32 main() { return 0; }
```

`%s` is the test file and `%S` its directory, relative to the repository root (the runner's
working directory), so paths in the output are stable. The RUN line may point at any file,
e.g. `// RUN: riftc check examples/employees.rifts`.

Golden file format (`tests/action/check/errors_short.expected`):

```
exit: 1
--- stdout
--- stderr
tests/action/check/errors_short.rifts:2:10: error[E1010]: invalid digit '2' in base-2 literal
```

On a mismatch the runner prints a unified diff. If the change is intended, run
`scripts/test.sh --update` and review the golden files with `git diff` before committing.

Use action tests for **what a user sees**: CLI behavior, exit codes, the exact diagnostic
layout, token dumps (`riftc lex`), syntax trees (`riftc parse`), merged module symbol tables
(`riftc symbols`), typed syntax trees (`riftc typed`), and the example programs.
`tests/feature/99-programs/` holds complete programs that must check without any diagnostic;
they guard against false errors.
A `riftc parse` golden file is the easiest way to check *how* something parsed, e.g.
operator precedence in `tests/action/parse/precedence.rifts`.

## Tests for features that don't exist yet: `XFAIL`

Any feature or action test can say it is expected to fail:

```rift
// XFAIL: needs the type checker (spec 02)
u8 d = 300;          //~ error
```

It is reported as `XFAIL` and does not fail the run. Once the compiler implements the feature
the test starts passing, which is reported as `XPASS` **and fails the run**, so the marker gets
removed and the test becomes a real regression test. This lets you write spec tests ahead of
the implementation.

## Exit codes of `riftc`

| Code | Meaning |
|---|---|
| 0 | success (warnings allowed) |
| 1 | errors in the input |
| 2 | bad command line |
| 3 | internal compiler error |

## Diagnostic codes

Every diagnostic has a stable code from `compiler/include/rift/diagnostic_codes.def`
(`E0xxx` driver, `E1xxx` lexer, `E2xxx` parser, ...; `W` for warnings). `riftc explain`
lists them and `riftc explain E1003` describes one. Tests should match on codes rather than
message text, so messages can be improved without touching every test.

## Adding a compiler phase

1. Add its codes to `diagnostic_codes.def` (the `explain_all` action test will fail; `--update` it).
2. Call it from `run_frontend` in `compiler/src/driver.cpp` (and add a dump command like `lex` if useful, e.g. `riftc parse` printing the AST).
3. Unit-test the component, add good/bad feature files for each rule, remove `XFAIL` from the feature tests it now handles.

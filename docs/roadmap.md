# `Rift` Compiler Roadmap — Bootstrapping a C++26-implemented Compiler

Below is a phased roadmap aimed at **one thing at a time**, with each phase producing a working compiler. The roadmap is organized around a *walking skeleton* first (get `compile_check.rfts` to a real executable), then broaden language coverage, then tackle the design-doc features that are research-grade (templates-via-ThinLTO, borrow checking).

A few upfront recommendations are baked into the plan:

- **Backend: LLVM.** Writing an x86-64 backend is a second project. Use LLVM directly (C++ API or IR-text first, switch to in-process IRBuilder later). This satisfies "compiles to machine code".
- **Frontend: hand-written recursive descent + Pratt parsing.** Best diagnostics-to-effort ratio; no parser generator.
- **`librift` as a library, `riftc` as a thin driver.** Lets you reuse the frontend for tests, `riftfmt`, an LSP, and the eventual `rift build` (§Future thoughts).
- **Compile-all-TUs-in-one-process** for the first many months. This sidesteps building the binary module-metadata format (§5.4) and makes the merged-header-source model tractable early. Add on-disk metadata when incremental builds actually matter.
- **C++26 caveat.** Full C++26 support in shipping compilers is still partial in 2025. Target `-std=c++2c` on Clang 19+/GCC 15+ and be prepared to fall back to C++23 features for anything that isn't available. Do not let this block progress.

---

## Horizon 1 — Walking Skeleton (target: weeks, not months)

**Goal:** `compile_check.rfts` → an ELF/Mach-O/PE executable whose exit status is `0`.

### M0 — Repo & toolchain (½–1 week)
- Monorepo: `compiler/`, `tests/`, `examples/`, `docs/`.
- CMake (Ninja generator) with C++26 flags; add `LLVM` as a `FetchContent` or system dep only when M4 needs it.
- `riftc` CLI stub: `riftc compile <file.rfts> -o <out>`.
- Test harness: golden tests (`tests/rfts/*.rfts` with `// EXIT: 0` / `// EXPECT_STDOUT:` header comments) and a small Python/C++ runner. Use `lit` or hand-rolled; hand-rolled is fine here.
- CI on Linux at minimum (macOS/Windows later).

**Exit:** `riftc --version` works; empty test runner passes.

### M1 — Lexer (1–2 weeks)
- Token kinds for phase-1 vocabulary: identifiers, integer literals **with suffix** (`0u32`, `42i64`), `{`, `}`, `(`, `)`, `;`, `return`, `main`, primitive type names (`u32`, `i32`, etc.).
- Source spans on every token (byte offsets + line/col). **This is the foundation of every future diagnostic** — get it right.
- Diagnostics module: `Diagnostic { severity, code, span, message, notes }`, pretty-printing with a caret and the offending line. Budget for one-screen-per-error (§2 principle 8).
- Unit tests: tokenizing `compile_check.rfts`.

**Exit:** Lexer handles the test file and returns a token stream. All tokens have correct spans.

### M2 — Parser & AST (2–3 weeks)
- AST node design with source spans on every node. Keep nodes small and pointer-free where possible (index into arena is a good model).
- Recursive-descent for functions and blocks; **Pratt parser** for expressions from the start (worth the small setup cost).
- Parse: `<type> <name>(<params>) { <stmts> }`, `return <expr>;`, integer literals, parenthesized expressions, (prepare for) `mut`.
- Make `0u32` (suffix) produce a typed `IntLiteral` node; `0` (unsuffixed) should already emit an error per §16.2 (or be flagged for later context-driven typing — decide now).
- Diagnostics: parse errors with "expected X, found Y" + span + one-sentence hint.

**Exit:** `compile_check.rfts` parses into an AST, printable via `riftc dump-ast`.

### M3 — Semantic analysis, minimal (1–2 weeks)
- Symbol tables, type table for primitives (`u32`, `i32`, `u64`, `i64`, `u8`, `f32`, `f64`, …).
- Two-pass sema: collect declarations, then check bodies.
- Validate: single `main`, return type is `u32` (or `std::exit_code` later — for now, allow sized integers per §7.4), return-statement type matches function return type, integer literal fits its suffix type.
- Start the **diagnostic budget discipline**: every error must state what was expected, what was found, and where.

**Exit:** `compile_check.rfts` passes sema; a file with `u32 main() { return "no"; }` produces one clean error.

### M4 — Codegen to executable (2–4 weeks)
- First output: LLVM IR **as text** (`.ll`), piped to `llc`/`clang`. This is dramatically easier to debug than in-process IRBuilder. Switch to in-process IR later (post-M10) if build time becomes a problem.
- Emit `define i32 @main() { ret i32 0 }` and link with `cc`.
- Wire end-to-end: `riftc compile compile_check.rfts -o compile_check`.
- Verify exit code.

**Exit — the Horizon 1 milestone:**
```
$ riftc compile compile_check.rfts -o compile_check
$ ./compile_check ; echo $?
0
```

If you have this, you have a **compiler**, and every subsequent phase is additive.

---

## Horizon 2 — Core Language (months 2–5)

**Goal:** real, useful Rift programs in a **single file**, no imports yet. This is the sweet spot that makes the language usable for prototyping before the design-doc's harder features exist.

### M5 — Expressions, statements, control flow
- Arithmetic, comparison (`==`, `<`, …), logical (`&&`, `||`, `!`), unary `-`, `~`, bit operators `| & ^ << >>` (§16).
- `bit(n)` keyword expression: contextual typing, compile-time bounds check, `i32` default in unconstrained context.
- Variables: `T x = v;`, `mut T x = v;`, immutability enforcement, `default` zero-init.
- `if`/`else` as statement **and** expression; `while`; range-`for` with `std::range`.
- Unsuffixed integer literals: type inference from context, reject ambiguous cases. No implicit numeric conversions (§16.1) — enforce this now; it is easier to add widening later than to remove it.
- Function calls (Rift-to-Rift).

### M6 — `struct` and `enum`
- `struct` with fields, `T{ .field = v }` initializers, `.` access.
- Enforce §4.1 rules syntactically: all-public, no virtual, no base, no inherit. Defer `compile_time` methods to M11; for now, allow methods and reject them if they violate the rules after M11 exists.
- `enum` with mandatory underlying type, auto-increment, scoped `::` access, no implicit integer conversion.
- Layout: define an internal `Layout` type — size, align, field offsets — computed during sema. This is reused later by the merged-class model.

### M7 — References, casts, `unsafe`
- Parameter passing modes: `T`, `T&`, `mut T&`. Grouped parameters (`T& {a, b}`).
- `cast<T>`, `cast::dynamic<T>` (returns `std::option<T>`), `cast::reinterpret<T>` (must appear inside `unsafe { }`).
- `unsafe { ... }` block: parse, scope, sema gate for `cast::reinterpret`. Do **not** implement a borrow checker yet — `unsafe` is just a syntactic gate.
- Introduce a **minimal foreign-function escape hatch** (e.g., `extern "C" fn`) so you can bind to `printf`/`write` for `std::println`. You'll want it before the stdlib is written in Rift.

### M8 — `std::result`, `std::option`, `match`, `?`
- `std::result<T, E>` and `std::option<T>` as **built-in generic types** implemented by the frontend (do not wait for M12 templates).
- `match` expression: exhaustive, no fallthrough, patterns (literals, ranges, `_`, type patterns, struct destructuring, guards).
- `?` operator: `std::convert`-based error conversion; as a first cut, hardcode identity + a couple of common conversions; proper `std::convert` trait design comes with M12.
- `std::variant<T1, T2, …>` as a built-in tagged union.

### M9 — Classes (single-file only), interfaces, methods
- `class` with `public`/`protected`/`private` members (all in the same file for now).
- `this`, method syntax (`T& x` as receiver).
- `open`, `virtual`, `override`, vtable emission, inheritance.
- `interface` types — method-only signatures + dynamic dispatch.
- Composition is the default; no implicit base calls, no `super` sugar yet (add later).

**Horizon 2 exit criteria:** a 500–1000 line Rift program in one file that reads a text file, parses lines into `struct`s, uses `result`/`match`, defines a couple of classes with virtual methods, and exits with a status. **This is a real language.** It is not spec-complete, but the language is now usable for the compiler's own test programs.

---

## Horizon 3 — Modules & Design-Doc-Specific Features (months 5–10)

### M10 — `import`, TUs, module driver
- Parse `import` (§11) and split a program into TUs.
- **Compile-all-TUs-in-one-process**: the driver loads every imported file into a single in-memory module graph. No on-disk module metadata yet.
- Enforcement: `std::` qualification mandatory, no `using`.
- **Skip** §5.4 on-disk layout metadata for now. `sizeof`/stack allocation work because everything is in-memory. Add binary metadata later (M10.5, when incremental builds actually hurt).

### M11 — `compile_time`, `run_time`, `deduce`
- `compile_time` functions: constant evaluator over the AST (no bytecode VM needed — reuse sema infrastructure).
- `compile_time run_time`: both modes; constant-folding at call sites when arguments are constant.
- `deduce`: flow-dependent return types (§13.3) — this requires the sema pass to track return-type per branch. Keep the surface syntax simple: require `auto deduce` return, require every arg at every call to be a compile-time constant.
- Enable `compile_time` methods on `struct` (§4.1) — this unblocks the intended `struct` design.

### M12 — Merged header/source classes (§5)
- Two-file form: `test.h.rfts` declares the public interface; `test.rfts` declares the class again with private members + method bodies.
- Merge rules (§5.2) — implement every row as a specific diagnostic.
- Layout rule (§5.3): public members first (header order), then private (source order).
- Because M10 is compile-all-in-one-process, the "importer" already has the merged class in memory. **This is where the module-metadata simplification pays off.**
- Also support `void test::func()` out-of-line method syntax (§5.5).

### M13 — Templates
- Implement **the simple version first**: header-inline templates (C++-style), instantiate at first use in the driver's single process. This is not the §12.2 ThinLTO model, but it delivers template *functionality*.
- Then, if you actually want the two-phase/ThinLTO model: prototype separately. It's a research project — link-time instantiation with cross-TU diagnostic propagation. Consider publishing it as an extension.
- Generic *constraints* (traits, bounds, concepts) — deferred to Horizon 4; for now, `std::variant`/`result`/`option` are built-in.

### M14 — Casts, file-scope modifiers, `std::bitset`
- `cast::reinterpret` audit tooling (list every `unsafe` in a build).
- File-scope `private` / `static` / `private static` (§20). Note `u32 x = 0;` at file scope is a compile error — implement that diagnostic cleanly.
- `std::bitset<N>` as a small generic built-in.

**Horizon 3 exit:** the language from the design doc is *usable* end-to-end, minus the research-grade items, and including the merged class model that is the doc's most distinctive feature.

---

## Horizon 4 — Research-Grade / Deferred (months 10+)

These should each have their own design doc. Do **not** block shipping anything else on them.

| Feature | Notes |
|---|---|
| **Borrow checker** (`str` vs `string`, lifetimes) | §24 flags this as the largest gap. Multi-year if done with a Rust-quality experience. Start with a conservative, "no aliasing across function calls" approximation and iterate. |
| **ThinLTO two-phase templates** (§12) | Prototype separately; measure whether it beats header-inline templates for your workloads. |
| **`std::convert` trait + generic constraints** | Once templates + traits exist, `?` operator's conversion matrix should be trait-driven. |
| **SIMD** (§21) | After the codegen pipeline has settled; LLVM vectors are the natural mapping. |
| **Concurrency** | Add `std::async`, channels, or whatever the design settles on. Don't add OS threads before you've designed the safety story. |
| **Operator overloading** | The `/` for `std::filesystem::path` (§19) demands it eventually. Design rules first; hold the roadmap slot. |
| **`impl` keyword** (§22 vs §24) | Decide semantics before Horizon 3 ends. |
| **Build-system integration** (§Future thoughts) | Add `rift build` as a subcommand — the language-as-build-system can invoke `librift` in-process. Keep the driver thin until then. |

---

## Cross-Cutting Workstreams (run continuously)

These start at M0 and grow; they are not a single phase.

1. **Diagnostics infrastructure.** Every phase should add diagnostics, not just features. Keep spans, error codes, notes, hints. Write a `tests/diagnostics/` suite with one expected-error `.rfts` per diagnostic.
2. **Testing.** Three layers: unit (per module), golden end-to-end (`tests/rfts/`), and negative/diagnostic. Fuzz lexer + parser from M2 onward with libFuzzer.
3. **`librift` API hygiene.** Sema and AST should be frontend-library-clean — no `main`, no file IO in the library. This is what makes the LSP and formatter cheap to add.
4. **Determinism.** Same source + same flags → byte-identical output. Required for reproducible builds and for diff-based debugging.
5. **Benchmarking.** A small "self-test" program that exercises Horizon 2 features; track compile time per phase so you notice regressions.
6. **Formatting/linting.** `riftfmt` from Horizon 2 onwards; it enforces naming (§3).

---

## What to *Explicitly Not Do* in V1

These are all in the design doc but should be deferred or simplified — call this the "V0 subset". Document the subset in `docs/v0-subset.md`.

- ❌ **On-disk module metadata** (§5.4) — use compile-all-in-one-process until incremental builds matter.
- ❌ **ThinLTO templates** (§12) — header-inline first (M13).
- ❌ **Borrow checker** — `unsafe` as a syntactic gate first (M7).
- ❌ **Generic constraints / traits / concepts** — built-in generics for `result`/`option`/`variant` only (M8).
- ❌ **Full stdlib** — start with an FFI to `libc` (`write`, `open`, `read`, `close`, `malloc` — the last only inside `unsafe`); write the stdlib in Rift once M11 lands.
- ❌ **SIMD, concurrency, operator overloading** — deferred per Horizon 4.
- ❌ **Relative-path warning** (§19) — trivial, but add it once `std::file` exists.

The **V0 subset** in one sentence: `struct`, `enum`, `class`, `interface`, functions, references, `result`/`option`/`match`, mutable/immutable rules, `cast` family, `unsafe` blocks, single-file + merged two-file classes, in-process imports. That's a genuinely useful language — get there, then expand.

---

## Suggested Order of Work, Week-by-Week (first 12 weeks)

| Week | Phase | Concrete deliverable |
|---|---|---|
| 1 | M0 | Repo, CMake+C++26 flags, `riftc --version`, test runner |
| 2 | M1 | Lexer for `compile_check.rfts`, span-bearing tokens, unit tests |
| 3 | M1–M2 | Diagnostics module; parse function decl + `return` + int literal |
| 4 | M2 | Pratt parser skeleton; `dump-ast` command |
| 5–6 | M3 | Symbol/type table, sema on `compile_check.rfts`, clean errors |
| 7–9 | M4 | LLVM IR-text emit, `cc` link, `compile_check` runs and exits 0 |
| 10 | M5a | Arithmetic + comparisons + `mut` + immutability |
| 11 | M5b | `if`/`else`, `while`, `bit(n)`, unsuffixed literals inference |
| 12 | M5c | Range-`for` + `std::range` (introduce a minimal `librift` stdlib shim) |

Adjust for your pace, but this gets you to a **running binary from your own source language** in ~2 months of part-time work.

---

## Biggest Risks (and Mitigations)

1. **§5 merged-class model leaks into everything.** Get M10 (in-process modules) done before M12, and design `Layout` early (M6). Do not attempt on-disk metadata before M12.
2. **C++26 toolchain immaturity.** Pin a specific Clang; keep a C++23 fallback for any feature that isn't stable. Don't let language-standard drag block M4.
3. **Diagnostics retrofit is painful.** Build the `Diagnostic` type in M1; use it exclusively thereafter.
4. **Templates-via-ThinLTO is a research paper.** If you attempt it, budget a separate quarter and a benchmark to compare against header-inline templates. Ship header-inline first.
5. **The borrow checker is the long pole.** Under no circumstances should it block Horizon 2/3. `str`/`string` can coexist without a checker if you accept the safety holes documented in `unsafe`.
6. **Stdlib scope creep.** FFI-to-libc is fine for Horizon 2; port to Rift stdlib only after M11.
7. **Self-hosting temptation.** Resist. This is a multi-year goal; the compiler written in C++26 is the target.

---

## Suggested Immediate Next Step

Implement **M0 + M1 + a stub M2** this week: `riftc compile compile_check.rfts -o compile_check` should tokenize and parse and then stop with `TODO: codegen`. Commit. Every following week is a green checkmark against the roadmap. This week's exercise is not "make it work" — it's "make the plumbing not hurt later".

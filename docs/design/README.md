# Rift - Language Design (v0.3)

> **Status:** Draft v0.3. This folder replaces `docs/design_doc.md` (v0.2).
> **Lineage:** C++-inspired. Explicit, simple to use, high control.
> **Files:** `.rifts` = source, `.rifth` = header.

Each file below describes **one concept** and can be read on its own. Every file has the same layout: **Rules**, **Examples**, **Rationale**. Cross-references use the file number, e.g. *(see 12)*.

## Files

| # | File | Concept |
|---|---|---|
| 01 | [01-lexical.md](01-lexical.md) | Files, comments, literals, keywords, `;` and braces |
| 02 | [02-types.md](02-types.md) | Built-in types, literal typing, bool conversion, every implicit conversion |
| 03 | [03-variables.md](03-variables.md) | Declarations, `=`, `mut`, `auto`, `default`, file scope |
| 04 | [04-functions.md](04-functions.md) | Functions, parameter passing, call-site `mut`, `main` |
| 05 | [05-structs.md](05-structs.md) | `struct`: plain data with methods |
| 06 | [06-classes.md](06-classes.md) | `class`: encapsulation, `mut` methods, destructors |
| 07 | [07-inheritance-interfaces.md](07-inheritance-interfaces.md) | `open`, `virtual`, `override`, `abstract`, `interface` |
| 08 | [08-enums.md](08-enums.md) | Simple enums and enums with payloads |
| 09 | [09-headers-modules.md](09-headers-modules.md) | `.rifth` / `.rifts` merge, `import`, `private` |
| 10 | [10-control-flow.md](10-control-flow.md) | `if`, `while`, `for`, `match`, block values, `never` |
| 11 | [11-error-handling.md](11-error-handling.md) | `std::result`, `std::option`, `?`, panics |
| 12 | [12-ownership-references.md](12-ownership-references.md) | Copy, `move`, `clone`, references, view types, destruction |
| 13 | [13-pointers-unsafe.md](13-pointers-unsafe.md) | `std::raw_ptr`, `std::unique_ptr`, `std::shared_ptr`, `nullptr`, `unsafe`, FFI |
| 14 | [14-templates.md](14-templates.md) | `template<...>`, constraints, compilation model |
| 15 | [15-compile-time.md](15-compile-time.md) | `compile_time`, `run_time`, `deduce`, `if compile_time` |
| 16 | [16-operators.md](16-operators.md) | Precedence, overflow, bit operations, `bit(n)`, overloading |
| 17 | [17-casts.md](17-casts.md) | `cast<T>` and its explicit variants |
| 18 | [18-lambdas.md](18-lambdas.md) | Lambdas and captures |
| 19 | [19-strings-formatting.md](19-strings-formatting.md) | `std::str`, `std::string`, literals, `f"..."` |
| 20 | [20-standard-library.md](20-standard-library.md) | Naming rules and type catalogue |
| 21 | [21-simd.md](21-simd.md) | SIMD sketch (deferred) |
| 22 | [22-open-questions.md](22-open-questions.md) | Everything not decided yet |

A complete program written to these rules is in [`examples/employees.rifts`](../../examples/employees.rifts).

## Design principles

1. **Explicit beats implicit.** If a value's type, cost or behaviour can't be read from the source, the compiler refuses to guess. The *complete* list of implicit conversions fits in one table (see 02).
2. **One way to do each thing.** One way to initialize (`=`), one way to build a value (`T{...}`), one way to define a method body.
3. **Safe by default.** Safe code cannot trigger undefined behaviour. Overflow, out-of-bounds access and bad shifts panic. Unsafe operations live inside a visible `unsafe { }` block.
4. **Immutable by default.** Variables, parameters and `this` are immutable unless marked `mut`.
5. **No uninitialized values.** Every variable has a value from its declaration on.
6. **Composition and inheritance, both explicit.** Inheritance requires `open` on the base class.
7. **Nothing costs more than it looks.** Allocation, copies of owning types, moves and dynamic dispatch are always visible in the code.
8. **Control when you ask for it.** Every safe default has a named escape hatch: `+%` wrapping arithmetic, `cast::truncate`, `std::raw_ptr`, `unsafe`.
9. **Fast, legible compilation.** Each error fits on one screen.

## Changes from v0.2

| Change | Reason |
|---|---|
| Spec split into one file per concept | Easier to review and extend each concept on its own |
| Extensions `.rifts` / `.rifth` | Replaces `.slate`, `.rfts` and `.h.slate` |
| `const` removed everywhere | Everything is immutable by default |
| `run_time` (not `runtime`) | Matches the spelling of `compile_time` |
| Trailing `;` after `}` of types removed | It was only there because of C's `struct x {...} var;` |
| Initialization only with `=`; `T x{...}` is banned | One way to initialize |
| Explicit stdlib names: `std::error`, `std::darray`, `std::array` | `std::err` and `std::list` were abbreviations or misleading |
| Pointers in `std::`: `raw_ptr`, `unique_ptr`, `shared_ptr`, `weak_ptr` | Library types, not keywords |
| `nullptr` replaces `null`, only for `std::raw_ptr` | Absence of a value is `std::option`; one way each |
| Struct methods are normal methods | v0.2 made them `compile_time`, so they couldn't be called on runtime data |
| Mutating methods end with `mut` | Answers the open question of how a method declares that it changes `this` |
| Template parameters are declared: `template<type T>` | v0.2 used `T` without declaring it |
| Templates are type-checked at compile time; only codegen is deduplicated at link time | `sizeof`, `compile_time` and `deduce` need instantiation at compile time |
| Contextual bool conversion for integers and `std::raw_ptr` | `if (ptr)` and `if (flags & bit(4))` are allowed |
| `bit(n)` requires a known type; `auto x = bit(4)` is an error | The type must be visible somewhere |
| `std::string s = "literal";` is a warning | It allocates silently |
| Unsuffixed literals take their type from context | `u32 x = 0;` works; `0u32` is only needed without context |
| Defined: overflow, out-of-bounds, shifts, division by zero → panic | v0.2 promised no undefined behaviour but left these open |
| `move`, `.clone()`, call-site `mut` | Copies and mutation are visible at the call site |
| References are parameter-style (second-class) | Safety without lifetime annotations |
| Lossy casts have explicit names | `cast<T>` is lossless only |
| `f"..."` marks string interpolation | Plain strings are never interpolated |
| Mandatory braces, no `++`/`--`, assignment is a statement | One way, and `if (x = 0)` can't happen |
| Parameter grouping `T& {a, b}` removed | A second way to write a parameter list |
| `std::variant` removed; enums can carry payloads | One way to write a sum type |
| `?` requires matching error types; `.or_error()` / `.map_error()` convert | Error conversion is visible |
| Method bodies are defined only inside the `class`/`struct` block in `.rifts` | One way; out-of-line `test::f()` definitions removed |

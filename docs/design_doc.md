
# `Rift` - Language Design Document

> **Status:** Draft v0.2  
> **Working name:** TBD. Placeholder `Rift` used throughout.  
> **Lineage:** C++-inspired, but explicitly simpler, safer, and more consistent.  
> **Organization:** Each numbered section is **self-contained** - intended to be splittable into a dedicated per-feature document.  
> **Changelog from v0.1:** §4 (struct/class split reworked), §5 (header/source merged class definitions - replaced PIMPL-shaped draft), §6 (no `const` keyword), §7 (no `fn` anywhere), §12 (templates via Two-Phase + ThinLTO), §13 (`compile_time` hardened; added `deduce` and `runtime`), §14 (inheritance restored, with `open` opt-in), §16 (bit-set operator), §17 (tuples/lambdas unchanged), §20 (`private`/`static` at file scope), §21 (SIMD deferred with design sketch).  

---

## 1. Motivation

C++ is 40+ years old, and its design has been shaped by the rule "never break existing code". This has produced a powerful but bloated language: multiple ways to do every task, permanent memory-safety hazards, a header/source split that forces duplication, and template errors that span screens.

`Rift` keeps what makes C++ valuable - deterministic destruction, zero-cost abstractions, direct memory control - and systematically discards what makes it painful.

---

## 2. Design Principles

1. **Explicit beats implicit** If a value's type, cost, or behavior can't be inferred from the source line, the compiler refuses to guess.
2. **One way to do each thing** One string type for ownership, one for borrowing. One error type. One cast spelling per cast kind.
3. **Safety by default** Safe code cannot trigger undefined behavior. When unsafe operations are needed, they live inside a visible `unsafe` block.
4. **Immutable by default** Variables are immutable unless marked `mut`. Data members are immutable unless the class declares them `mut`.
5. **No uninitialized values** Every variable has a value at every point in its lifetime.
6. **Composition *and* inheritance, both explicit** Inheritance is opt-in at the class level (`open`); composition is the recommended default.
7. **Nothing costs more than it looks** Move construction, allocation, and indirection are always visible in the signature.
8. **Fast, legible compilation** Error messages are budgeted to one screen per error, not one per template instantiation.

---

## 3. Naming Conventions

| Category | Convention | Enforced? |
|---|---|---|
| Keywords | `snake_case` | Yes (compiler) |
| Built-in types | `snake_case` | Yes (compiler) |
| Standard library | `snake_case` | Yes (stdlib authoring rule) |
| User-defined types, functions, members | **Any casing** | No |

User code may use `snake_case`, `PascalCase`, `camelCase`, or anything else. The compiler reserves keywords and built-in type names; it does not lint user identifiers.

---

## 4. Type System - `struct` vs. `class`

`Rift` has two user-defined type kinds with distinct, mechanically enforceable roles.

### 4.1 `struct` - plain data

A `struct` is **purely for data storage**. All members are public. No inheritance. No virtual. Layout fully visible in the source.

```slate
struct employee {
    std::string name;
    u32         age;
    f64         salary;
};
```

**Compiler-enforced rules for `struct`:**

| Rule | Check |
|---|---|
| All data members are public | Syntactic |
| No `virtual` methods | Syntactic |
| Cannot be a base class | Syntactic |
| Cannot inherit from anything | Syntactic |
| No hidden state (no `private`, no `protected`) | Syntactic |
| Every method must be `compile_time` | Syntactic |

The `compile_time` rule is what enforces "purely for data storage": convenience functions like `is_valid()` are allowed only if they are computable from public fields without side effects.

```slate
struct employee {
    std::string name;
    u32         age;
    f64         salary;

    compile_time bool is_valid() {
        return !this.name.is_empty() && this.age < 150u32;
    }
};
```

If a `struct` method needs runtime behavior (I/O, allocation, non-`compile_time` calls), it does not belong in a `struct` - move the type to a `class`, or write a free function.

### 4.2 `class` - encapsulated, inheritable

A `class` supports encapsulation, inheritance, and virtual dispatch. Its **public and protected interface** lives in the header file; its **private members and implementation** live in the source file (§5).

```slate
open class shape {
public:
    virtual f64 area();
    virtual f64 perimeter();
};

class circle : shape {
public:
    f64 radius = 0.0;
    virtual f64 area()      override;
    virtual f64 perimeter() override;
};
```

**Rules for `class`:**

| Rule | Notes |
|---|---|
| May have `public`, `protected`, `private` members | Private members declared only in the source file |
| May be virtual and inherit | Only if the class is declared `open` |
| Can only be inherited from if declared `open` | See §14 |
| May not be stack-allocated in a TU that lacks its compiled layout metadata | See §5.4 - but with module metadata, this is transparent |

### 4.3 `enum`

```slate
enum parse_error : u8 {
    bad_line = 0,
    bad_number,       // implicitly 1
};
```

- Underlying type **mandatory**.
- Values auto-increment from the last explicit value.
- Always scoped: `parse_error::bad_line`. Never implicitly convertible to or from integers.

### 4.4 `interface`

A named set of method signatures, independent of any concrete class.

```slate
interface shape_like {
    f64 area(this);
    f64 perimeter(this);
}

class circle : shape_like {
public:
    f64 radius = 0.0;
    f64 area()      override;
    f64 perimeter() override;
}
```

`interface` methods use the **standard function form**.

### 4.5 `variant` (standard library)

For closed sets of alternatives, `std::variant<T1, T2, ...>` gives exhaustive pattern matching without inheritance.

```slate
std::variant<circle, square> s = circle{ .radius = 1.0 };
const f64 a = match (s) {
    circle c => 3.14159 * c.radius * c.radius,
    square q => q.side * q.side,
};
```

---

## 5. Header / Source Split - Merged Class Definitions

> **Design intent:** The header holds *only* the public/protected interface. The source file holds *only* the private members and method bodies. The compiler merges both declarations into a single class entity. This is **not PIMPL** - there is no pointer indirection and no per-instance heap allocation overhead. The merge happens once, at compile time, producing one class with one memory layout.

### 5.1 The two halves

```slate
// ── test.h.slate  (interface) ────────────────────────────────────
class test {
public:
    void func_1();
    void func_2();
    i32  public_member_x = 0;
};
```

```slate
// ── test.slate  (implementation + hidden state) ──────────────────
class test {
public:
    void func_1() {}
    void func_2() {}
private:
    void func_3() {}
    f32  private_member_x = 0;
};
```

The **compiler merges these two into one class**. The caller's view (from `test.h.slate`) is the public interface. The full layout - including `private_member_x` and `func_3` - is known to the compiler via module metadata (§5.4).

### 5.2 Merge rules

| Situation | Result |
|---|---|
| Header declares `void f()`; source defines `void f() { ... }` | ✓ Merged |
| Header declares `void f()`; source does **not** define `f` | Link-time error |
| Source defines `void g()` with no header declaration | Compile error (public method without header declaration) |
| Source declares `private void h()` | ✓ Allowed - adds hidden method |
| Source declares `private u32 x` | ✓ Allowed - adds hidden data member |
| Header declares `private u32 x` | Compile error (`private` in header is forbidden) |
| Source declares a `public` member not in header | Compile error |

### 5.3 Layout rule

Layout order is: **header's public/protected data members in header order, then source's private data members in source order**. The source may optionally redeclare public data members for readability, but the header is authoritative for their existence and default values.

```
[ public_member_x (i32, header) ][ private_member_x (f32, source) ]
```

Any change to either half that alters layout invalidates the compiled module interface - dependents must recompile. This is standard module versioning; it does not affect source-level compatibility.

### 5.4 Module metadata carries the layout

The compiler emits the **complete class layout** - size, alignment, member offsets - into the compiled module metadata for the header module. A TU that imports only the header receives:

- **Visible:** member names, types, access levels of public/protected members.
- **Hidden:** names and types of private members - but the *size and layout* of those private members is baked into the class's metadata, so `sizeof(test)` and stack allocation work without the importer seeing the private declarations.

```slate
// in another TU that imports test.h.slate:
test t = test{};                     // OK - stack allocation works
std::println("{sizeof(t)}");         // OK - size is in metadata
t.public_member_x = 5;               // OK - public member
// t.private_member_x = 5;           // COMPILE ERROR - private member not visible
```

This is the mechanism that makes the merged-class design work without PIMPL.

### 5.5 When the class has no private members

If a class has no private members, the header declaration is complete. The source file may still contain method bodies, but does not need to repeat the `class` block:

```slate
// test.h.slate
class test {
public:
    void func_1();
};

// test.slate
void test::func_1() { /* ... */ }
```

(Out-of-line method definitions like `void test::func_1()` are supported for this case and for defining public methods from the header.)

---

## 6. Variables and Mutability

- **Immutable by default.** `u32 x = 5;` is immutable. Mutation requires `mut`.
- **No `const` keyword.** It was removed because everything is const by default. There is no `const` in the grammar.
- **No uninitialized variables.** Every declaration must include an initializer. `u32 x;` is a compile error.
- **`default`** is a keyword producing the zero value for any type.

```slate
u32 x = 5;              // immutable
mut u32 y = 5;          // mutable
y = 6;                  // OK
x = 6;                  // COMPILE ERROR

u32 z;                  // COMPILE ERROR - no uninitialized values
u32 z = default;        // OK - zero value
```

### 6.1 File-scope variables

| Declaration | Meaning |
|---|---|
| `static u32 counter = 0;` (file scope) | Static storage duration - the variable persists for the program's lifetime |
| `u32 counter = 0;` (file scope) | Compile error - file-scope variables must declare their storage duration |
| `private u32 helper = 0;` | Visible only in this TU (internal linkage) |
| `private static u32 helper = 0;` | Both: TU-local and program-lifetime |

`private` and `static` are orthogonal at file scope.

---

## 7. Functions and Parameters

### 7.1 Declaration syntax

```
<return_type> <name>(<parameters>) { <body> }
```

```slate
std::result<employee, parse_error> parse_line(std::str line) {
    // ...
}

void log(std::str message) {
    std::println("[log] {message}");
}
```

### 7.2 Parameter passing - all explicit

There are no implicit reference bindings. All passing modes are written at the call site:

| Syntax | Meaning |
|---|---|
| `T x` | By value (copy) |
| `T& x` | By immutable reference |
| `mut T& x` | By mutable reference |
| `auto x` | Deduced from the caller - forbidden in public signatures |

```slate
void by_value(employee e);               // copies
void by_ref(employee& e);                // immutable reference
void by_mut_ref(mut employee& e);        // mutable reference
```

For primitives, `u32 x` is by value; a reference to a primitive is written `u32& x`.

### 7.3 Parameter grouping

Adjacent parameters of the same type may be grouped in braces:

```slate
void sort_by(employee& a, employee& b);
void sort_by(employee& {a, b});          // identical
```

Grouping applies to lambdas too:

```slate
filtered.sort_by(
    [](employee& {a, b}) { return b.salary <=> a.salary; });
```

### 7.4 `main`

```slate
std::exit_code main();

std::exit_code main(std::d_array<std::string> parameter);
```

- Return type: `std::exit_code`, or any sized integer (`i8` through `i128`, `u8` through `u128`).
- `parameter[0]` is the executable path; `parameter[1..]` are user arguments.
- `std::exit_code` is defined by the standard library.

---

## 8. Type Instantiation vs. Function Calls

- **Types instantiated with `{}`.** `test t = test{};`, `test t2 = test{ .x = 5 };`
- **Functions called with `()`.** `my_func(42)`, `n.abs()`.

```slate
test t = test{};                  // OK
test t2 = test{ .x = 5 };         // OK

// test t = test();                // COMPILE ERROR
// my_func{42};                    // COMPILE ERROR
```

This rule eliminates C++'s "most vexing parse" by construction.

---

## 9. Control Flow

### 9.1 `if` / `else`

`if` conditions must be `bool`. There is no int→bool or pointer→bool conversion.

```slate
if (ptr != null) { ... }         // OK
if (ptr) { ... }                 // COMPILE ERROR
```

`if` is also an expression:

```slate
std::str label = if (x > 0) { "positive" } else { "non-positive" };
```

### 9.2 `for` - range-based only

No C-style `for (i = 0; i < n; i++)`.

```slate
for (employee& e : employees) {
    std::println("{e.name}");
}

for (u64 i : std::range(0u64, employees.len())) {
    std::println("{i}: {employees[i].name}");
}
```

### 9.3 `while`

```slate
while (file.has_more()) {
    std::str line = file.read_line();
}
```

### 9.4 `match` - exhaustive pattern matching

`match` is an **expression**, **exhaustive** (compile error if any case is missing), has **no fallthrough**, and **destructures** tagged values.

```slate
const std::string label = match (code) {
    200 => "ok",
    404 => "not_found",
    _   => "other",
};

match (parse_line(line)) {
    std::ok(e)  => employees.push(e),
    std::error(_) => std::eprintln("skipping bad line: {line}"),
}
```

Arm patterns support literals, ranges (`0..=9`), type patterns (`std::ok(x)`, `std::error(e)`), struct destructuring (`employee{name, age, ..}`), wildcard (`_`), and guards (`x if x > 100`).

---

## 10. Error Handling

- `std::result<T, E>` for all fallible operations. Variants: `std::ok(x)` and `std::error(e)`.
- `std::option<T>` for "maybe absent". Variants: `std::some(x)` and `std::none`.
- **No exceptions in safe code.** No `throw`, `try`, `catch`. FFI that can throw must wrap in `unsafe`.
- The `?` operator propagates errors, converting `E` to the caller's error type via `std::convert`.

```slate
std::result<employee, parse_error> parse_line(std::str line) {
    std::list<std::str> parts = line.split(',');
    if (parts.len() != 3) return std::error(parse_error::bad_line);

    u32 age = parts[1].parse<u32>()?;
    f64 salary = parts[2].parse<f64>()?;

    return std::ok(employee {
        .name   = std::string::from(parts[0]),
        .age    = age,
        .salary = salary,
    });
}
```

`?` and `match` are the **only** ways to consume a `std::result` - there is no `operator*`, so silent dereference is impossible.

---

## 11. Modules and Imports

- **No header/source text duplication** in the C sense: headers exist as the *interface half* of a class (§5), but are not `#include`d. They are imported as modules.
- `import` is a keyword. `std::` qualification is mandatory - no `using namespace`, no `using` declarations.

```slate
import std;
import std::string;

std::println("hello");
std::string s = std::string::from("world");
```

---

## 12. Templates - Two-Phase + ThinLTO

Templates support the **header/source split** via a two-phase compilation model with ThinLTO-based link-time instantiation.

### 12.1 The model

- **Header** declares the template signature (name, parameters, return type).
- **Source** contains the template body.
- When a TU uses `my_template<T>`, the compiler emits an **instantiation request**, not an instantiated template.
- At link time, ThinLTO merges all requests and instantiates each unique `T` once.
- Each unique `T` produces exactly one instantiation across the program.

### 12.2 Consequences

| Property | Value |
|---|---|
| Templates open to new `T`? | **Yes** - unlike explicit-instantiation-only models |
| Header/source split preserved? | **Yes** |
| Compilation cost | Once per unique `T` at link time |
| Incremental builds | ThinLTO caches per-`T` results |
| Error-reporting site | Link-time instantiation site, not the source template |

The compiler must be designed with link-time diagnostic propagation in mind - template errors will point at the use site, with a trace back to the template body.

---

## 13. Compile-Time Evaluation (`compile_time`)

### 13.1 `compile_time` means compile-time only

- `compile_time` functions are **always evaluated at compile time**. There is no runtime fallback.
- Calling a `compile_time` function with a non-constant argument is a compile error.
- **No `consteval` keyword.** `compile_time` alone means compile-time-only.

```slate
compile_time u32 square(u32 x) { return x * x; }

u32 a = square(5u32);           // OK
u32 b = square(runtime_value);  // COMPILE ERROR
```

### 13.2 Runtime opt-in via `run_time` modifier

To allow a `compile_time` function to also be callable at runtime, add the `run_time` modifier:

```slate
compile_time run_time u32 clamp(u32 x, u32 lo, u32 hi) {
    return if (x < lo) { lo } else if (x > hi) { hi } else { x };
}

u32 a = clamp(5u32, 0u32, 10u32);           // compile-time
u32 b = clamp(runtime_value, 0u32, 10u32);  // run-time
```

Three cases:

| Function declared as | Callable at compile time? | Callable at runtime? |
|---|---|---|
| `foo(...)` | No | Yes |
| `compile_time foo(...)` | Yes | **No** |
| `compile_time runtime foo(...)` | Yes | Yes |

### 13.3 Flow-dependent return types - `deduce`

When a `compile_time` function returns **different types depending on the branch taken**, the declaration must use the `deduce` keyword:

```slate
compile_time auto deduce transform(T x) {
    if compile_time (is_integer<T>) { return cast<i64>(x) * 2i64; }
    if compile_time (is_float<T>)   { return x * 2.0; }
}

i64 a = transform(42i32);      // branch A → i64
f32 b = transform(3.14);       // branch B → f32
```

- `deduce` is required **only** when the branches produce different types.
- A `compile_time` function with a single return type does **not** use `deduce`.
- Every argument to a `deduce`-marked function must be a compile-time constant at every call site.

---

## 14. Inheritance and Polymorphism

### 14.1 `open` - explicit opt-in to inheritance

A `class` must be declared `open` to be inherited from. This is the opposite of C++'s `final`, and it makes inheritance a deliberate author decision.

```slate
open class shape {
public:
    virtual f64 area();
};

class circle : shape {           // OK - shape is open
public:
    f64 area() override;
};

class square : shape { ... };    // OK
```

```slate
class sealed_thing { ... };

class bad : sealed_thing { ... }; // COMPILE ERROR - sealed_thing is not open
```

`open` is a **keyword**, applied as a class modifier: `open class`.

### 14.2 Virtual dispatch

`virtual` and `override` are keywords - they carry semantic weight the compiler must enforce. `virtual` may only be in classes that are marked `open`

```slate
open class shape {
public:
    virtual f64 area();
    virtual f64 perimeter();
};

class circle : shape {
public:
    f64 radius = 0.0;
    f64 area()      override;
    f64 perimeter() override;
};
```

### 14.3 Composition as the default

Composition is preferred: use member fields, not inheritance, unless the "is-a" relationship is genuine and the base class is `open`.

---

## 15. Casts

| Syntax | Kind | Safety |
|---|---|---|
| `cast<T>(x)` | Static (default) | **Safe** - compile-time checked |
| `cast::dynamic<T>(x)` | Dynamic | **Checked** at runtime; returns `std::option<T>` |
| `cast::reinterpret<T>(x)` | Reinterpret | **Unsafe** - only legal inside `unsafe { ... }` |

No C-style casts (`(T)x`).

---

## 16. Bit Operations

`Rift` keeps C++-style bit operations (`|`, `&`, `^`, `~`, `<<`, `>>`) as they are. The only change is that the `(1 << n)` idiom is replaced by a dedicated `bit(n)` expression.

**Why replace `(1 << n)`:** In C++, the type of `1 << n` is determined by the type of `1` (which is `int`), not by the surrounding expression. That's why `1 << 31` and `1u << 31` behave differently, and why `flags & (1 << 4)` can silently produce the wrong type. `Rift` has no `int`, so `1 << 4` has no natural type — `bit(n)` fills that gap.

**What `bit(n)` means:** the value with exactly bit `n` set (i.e., `1 << n`), typed by context.

```slate
mut u32 flags = 0u32;

flags |=  bit(4);              // set bit 4
flags &= ~bit(4);              // clear bit 4
flags ^=  bit(4);              // toggle bit 4

if (flags & bit(4)) { ... }    // test bit 4

flags &= ~(bit(0) | bit(1));   // clear bits 0 and 1
const u32 mask = bit(0) | bit(3) | bit(7);   // combined mask
```

### 16.1 Rules

- **`bit` is a keyword.** `bit(n)` is a built-in expression, not a function call.
- **`n` must be a compile-time constant** in safe code.
- **The result type is inferred from context**, like an integer literal. In `flags |= bit(4)`, the result of `bit(4)` is `u32` because `flags` is `u32`.
- **Compile-time bounds check.** If `n >= bit_width(T)`, compile error. `bit(32)` on a `u32` is rejected at compile time. `bit(-1)` is always a compile error.
- **Integer types only.** `f32 x = bit(4);` is a compile error — `bit(n)` may only produce an integer type.
- **Unconstrained context defaults to `i32`**, matching the integer-literal default. `auto x = bit(4);` gives `i32`.
- **No cross-width mixing.** `a | b | bit(4)` where `a` is `u32` and `b` is `u64` is a compile error — same rule as every other operator (no implicit numeric conversion). The type of `bit(4)` is well-defined only when the surrounding expression has a single unambiguous integer type.
- **Precedence:** `bit(n)` binds like a primary expression, so `~bit(4)` and `bit(0) | bit(1)` behave exactly like `~(1 << 4)` and `(1 << 0) | (1 << 1)` in C++.

### 16.2 Dynamic bit indices

When the bit index is only known at runtime, `bit(n)` is **not** available — the compile-time bounds check cannot be performed. Use the explicit shift operator:

```slate
const u32 dynamic_mask = 1u32 << n;
```

- The left operand must have an explicit type (`1u32`, `1u64`, etc.) — an unsuffixed `1` is not permitted as the shift base, since it would default to `i32` and the language does not allow implicit widening.
- `1u32 << n` has the same semantics as `<<` in C++, with one open question (see below).

### 16.3 Open question: out-of-range shifts

`1u32 << n` where `n >= 32` is **undefined behavior** in C++. `Rift` does not permit UB in safe code, so the shift operator needs a defined out-of-range semantics. Options under consideration:

- **(a)** Compile error if `n` is a constant `>= 32`; **panic** at runtime if `n >= 32`. (Safe, predictable, slight runtime cost.)
- **(b)** Wrap: `n & 31` before shifting. (Zero cost, C++-like on x86, but "the compiler silently masks your bug".)
- **(c)** Only legal inside `unsafe { ... }`. Safe code must use a checked shift function from the stdlib.

This is deferred — it does not affect `bit(n)`, which is always bounds-checked.

### 16.4 `std::bitset<N>` — unrelated

For bit arrays longer than a single integer, the standard library provides `std::bitset<N>`:

```slate
mut std::bitset<128> b = std::bitset<128>{};
b.set(4);
b.clear(4);
if (b.test(4)) { ... }
```

`std::bitset<N>` is a **standard-library type** (requires `std::` qualification). `bit(n)` is a **language keyword expression** (always in scope, no qualification). They solve different problems and do not overlap.

### 16.5 Why this design

- **Familiar.** `x |= bit(4)` reads like `x |= (1 << 4)`. No new idioms to learn — only the `(1 << n)` sub-expression is spelled differently.
- **Type-correct by construction.** The result adopts the left-hand type, so `u8` vs `u32` vs `u64` confusion disappears.
- **Bounds-checked.** `bit(32)` on a `u32` fails at compile time instead of being silent UB.
- **Minimal.** The bit operators themselves — `|`, `&`, `^`, `~`, `<<`, `>>` — are untouched. Only the "1 shifted by n" pattern is replaced.

---

## 17. Lambdas

C++-style `[](...)` syntax, with explicit-capture rules.

```slate
mut u32 counter = 0;

auto inc = [&counter]() { counter += 1; };                // by reference
auto add = [threshold](u32 x) { return x + threshold; };  // by value
auto init = [value = 10](u32 x) { return x + value; };    // init-capture
auto noop = []() { };                                     // empty
```

- Capture list is **mandatory**, even if empty.
- **No `[=]` or `[&]`.** Every capture is named.
- Parameter rules are identical to free functions: no uninitialized values, grouping allowed (§7.3).
- Return type is deduced unless explicitly written - same as free functions.

---

## 18. Memory Model

- **Safe by default.** Safe code cannot produce undefined behavior.
- **RAII.** Values own resources; scope end releases them.
- **No manual `new`/`delete`, `malloc`/`free`** in safe code.
- **`std::str` (borrowed) vs. `std::string` (owned).** Distinct types. `string → str` is implicit; `str → string` requires `std::string::from(...)`.
- **`unsafe { ... }`** creates an auditable boundary for raw pointer work, FFI, custom allocators, and dynamic bit indices. Not a global switch - a per-block marker.

```slate
unsafe {
    u64 addr: u64 = 0xDEAD_BEEF;
    raw_ptr<u32> ptr = cast::reinterpret<raw_ptr<u32>>(addr);
    *ptr = 0;
}
```

---

## 19. File Paths

String literals passed to filesystem functions as relative paths produce a **warning**:

```slate
// WARN: relative path
std::file in = std::file::open("employees.csv");

// GOOD
std::file in = std::file::open(
    std::filesystem::get_executable_path() / "employees.csv");

// ALSO GOOD (caller's responsibility)
std::str user_path = parameter[1];
mut std::file in = std::file::open(user_path);
```

`std::filesystem::path` supports `/` for composition. `std::filesystem::get_executable_path()` returns the absolute path of the directory containing the current executable.

---

## 20. `private` and `static` at File Scope

Orthogonal file-scope modifiers:

| Declaration | Storage duration | Linkage |
|---|---|---|
| `u32 x = 0;` (file scope) | Compile error - must specify | - |
| `static u32 x = 0;` | Static (program lifetime) | External |
| `private u32 x = 0;` | Compile error - must specify | Internal (TU-local) |
| `private static u32 x = 0;` | Static | Internal |

`private` also serves as a class-member access keyword (§5), but in file scope it has a distinct, unambiguous meaning: **visible only in the current translation unit**.

---

## 21. SIMD - Design Sketch (Deferred)

SIMD is intended as a **language extension**, not a separate world of intrinsics. The design is deferred to a dedicated document, but the direction is:

- **Vector types are primitives.** `f32x4`, `f32x8`, `f64x4`, `i32x8`, `u8x16`, etc., are built-in, not `std::`.
- **Arithmetic operators work element-wise.** `a + b`, `a * 2.0`, `a / b`.
- **Comparisons produce mask types.** `a > b` yields `mask8`, not `bool`.
- **`select(mask, a, b)` is the only conditional.** No per-lane branching.
- **Reductions are built-in.** `reduce_add(v)`, `reduce_max(v)`, `reduce_min(v)`.
- **`xN` is a semantic width, not a hardware width.** `f32x8` may map to one 256-bit register, two 128-bit registers, or anything the target provides.
- **Raw intrinsics remain available inside `unsafe`.**
- **Loading from memory** needs a safe story (`std::slice::as_vector<f32x8>()` or `unsafe` raw loads).

Open questions deferred to the SIMD document:
- Mask type ergonomics - is `mask8` distinct from `bitset<u8>`?
- Alignment and ABI for vector types across function calls.
- Whether widths are fixed (`f32x8` always means 8 elements) or best-effort match to target hardware.

---

## 22. Keywords Reference

Reserved words with grammatical meaning. Lowercase. Always in scope.

`import`, `struct`, `class`, `open`, `enum`, `interface`, `impl`, `virtual`, `override`, `mut`, `match`, `for`, `in`, `if`, `else`, `return`, `true`, `false`, `null`, `cast`, `unsafe`, `auto`, `default`, `deduce`, `runtime`, `bit`, `private`, `static`, `compile_time`, `if compile_time`

**Removed from v0.1:**
- `const` - removed entirely (§6)
- `fn` - removed entirely (§7, §4.4)
- `consteval` - merged into `compile_time` (§13)

---

## 23. Standard Library Reference

Always `std::`-qualified. **Not** keywords - implemented in `Rift` itself, shadowable by full qualification.

**Types:** `std::string`, `std::str`, `std::list`, `std::array`, `std::file`, `std::filesystem::path`, `std::result`, `std::option`, `std::variant`, `std::box`, `std::dyn`, `std::io_error`, `std::exit_code`, `std::bitset`, `std::range`

**Functions / constructors:** `std::ok`, `std::error`, `std::some`, `std::none`, `std::println`, `std::eprintln`, `std::filesystem::get_executable_path`, `std::convert`

**Test for the boundary:** *Can you write a function with the same signature?* If yes, it belongs in `std::`, not as a keyword. (`match` must be compiler-enforced because of exhaustiveness; `std::result` does not.)


---

## 24. Open Questions

- **SIMD full design** - deferred to a dedicated document (§21).
- **`impl` keyword status** - is it a keyword or a stdlib-level trait mechanism?
- **Lifetimes / borrowing** - the `str` vs. `string` split implies a borrow checker, but the full rules are not yet specified. This is the largest remaining gap.
- **Concurrency model** - not yet designed.
- **Generic programming model** - templates are defined operationally (§12) but the type-level constraint system (traits, bounds, concepts) is not yet designed.
- **Operator overloading** - `std::filesystem::path`'s `/` implies some overloading is necessary, but the general rules are not yet specified.
- **`std::convert` semantics** - the `?` operator depends on it; the trait is not designed.
- **Name** - still TBD.

























# Future thoughts
- let language serve as build system as well










```cpp
const std::vector<GLT::argument_parser::argument_spec> specs = {

    {
        .name = "project_path",
        .short_name = "",
        .required = true,
        .positional = true,
        .position = 0,
        .type = "path",
        .default_value = std::filesystem::path(),
        .help_text = "Path to the project file"
    },
};
```


```cpp
compile_time std::list<std::argument_parser::argument_spec> specs = {

    [required]
    [positional]
    {
        .name = "project_path",
        .short_name = "",
        .help_text = "Path to the project file",
        .position = 0,
        .type = std::filesystem::path,
        .default_value = std::filesystem::path{},
    },
};
```

# 23 - Checks

A **check** is a condition that must hold. If it doesn't, you get either a **compile error** or a **panic**. Every check says *when* it is evaluated, using the same `compile_time` / `run_time` words as functions (see 15).

Checks are written in the **source** (`.rifts`) only. Headers never contain them.

## Rules

### Check vs. assert
A C/C++ `assert` is a run-time check; Rift's is `run_time check`. What's new is that the same statement can also be evaluated by the compiler:

| Statement | Evaluated | If the condition is false | In the binary? | Arguments may be |
|---|---|---|---|---|
| `run_time check (c) else msg;` | When the program runs | **Panic** (see 11) | Yes | Anything |
| `compile_time check (c) else msg;` | When the compiler compiles the use | **Compile error. The program isn't built** | No | **Compile-time values only.** A run-time value is itself a compile error |
| `compile_time run_time check (c) else msg;` | At compile time when possible, otherwise at run time | Compile error if found at compile time, otherwise panic | Only for calls with run-time values | Anything |

- A `compile_time check` is a **build requirement**. It must pass, or nothing gets built.
- A `run_time check` is the language's assert. Without a mode list it is compiled into **every** build mode (principle 3). There is no separate `std::assert`.
- Either modifier can be limited to certain build modes, e.g. `run_time(debug) check` (see below).
- `compile_time run_time check` gives one statement both jobs: an early error when the value is known at compile time, and a safety net otherwise.

### The two forms
**Condition form**, for anything that must hold:
```
<when> check (<bool expression>) else <message>;
```
`<message>` is a string literal or an `f"..."`. For `compile_time` it is formatted by the compiler; for `run_time` it becomes the panic message.

**Block form**, `compile_time` only, for complex logic, warnings and notes:
```
compile_time check {
    <compile-time code that calls std::compile_error / std::compile_warning / std::compile_note>
}
```
- The block is ordinary compile-time code: loops, `match`, `compile_time` helper functions, and so on. It produces no values and declares nothing outside itself.
- It fails only by calling `std::compile_error`, which returns `never`. If it only calls `std::compile_warning`, the build continues with a warning.
- There is no run-time block form. Complex run-time conditions go into a `compile_time run_time bool` helper function and are used in the condition form (example 3).

### Mode-gated checks
A `compile_time` or `run_time` modifier can take a list of **build modes** (see 24). The check is then only part of builds in those modes, and in every other mode it **doesn't exist**: no code in the binary and no evaluation.
```
run_time(debug) check (...) else ...;                        // only in debug-based builds
run_time(debug, profile) check (...) else ...;               // debug-based or profile-based builds
compile_time(release) check (...) else ...;                  // a build requirement for release-based builds only
compile_time run_time(debug) check (...) else ...;           // compile-time part always, run-time part only in debug
```
- A listed mode also covers every mode **based on** it (see 24): `run_time(release)` applies to `release`, `release_with_debug_info` and a user mode based on either.
- Built-in mode names are always valid. A user mode name is only valid if the build defines it, so library code should list built-in modes only.
- A check **without** a list is in every mode. Only a mode with `.run_time_checks = gated_only` or `none` and `.allow_undefined_behavior = true` can remove it (see 24).
- For entry checks the **caller's** mode decides (see 24, "Scope of a mode").
- Mode-gated checks have no side effects to lose: the condition and the message are only evaluated when the check is active. That's why removing them can't change what a correct program does.

```rift
f32 sample(std::span<f32> data, usize i) {
    run_time check (i < data.len()) else f"sample index {i} out of range (len {data.len()})";
    run_time(debug) check (data.is_sorted()) else "sample() expects sorted data";     // O(n): debug only
    ...
}

compile_time(release) check (!std::build::setting<bool>("dev_tools")) else "dev_tools must be off in release builds";
```

### Where checks can be written
| Written | `compile_time` checks run | `run_time` checks run |
|---|---|---|
| **File scope** | Once, when the module is compiled | Not allowed |
| **Type body** (struct, class, enum) | Once for the type; for templates, once **per instantiation** | Not allowed |
| **Entry of a function body** | At **every call**, with that call's arguments | At every call, **at the call site** |
| **Elsewhere in a function body** | Not allowed | Where they are written (a normal assert) |

The **entry** of a function is the run of `check` statements at the very start of its body, before any other statement. Entry checks are the function's preconditions: the reader sees them first, and the compiler evaluates them as part of each call.
```rift
std::result<u32, uart::error> configure(u32 baud) {
    compile_time run_time check (uart::is_supported(baud)) else f"{baud} baud is not supported by this UART";
    // ---- end of entry: normal body from here on ----
    ...
}
```

Entry-check rules:
- A `compile_time` entry check may read the function's parameters. It requires their arguments to be compile-time values at every call.
- A `compile_time` entry check makes the function unusable through function pointers or `std::function`. Taking its address is an error, because there's no call site to check at. `compile_time run_time` and `run_time` entry checks have no such restriction.
- Virtual, `override` and `abstract` methods may only have `run_time` entry checks, because the target of a dynamic call isn't known at compile time.
- **Order:** entry checks run before the compiler checks the call's constraints or instantiates the body. If a check fails, the compiler's own errors for that call (unsatisfied constraint, errors inside the instantiated body) are **suppressed**, so the author's message is the only one.

### Compile-time values
These count as compile-time values: literals, `compile_time` constants, template value parameters, results of `compile_time` functions, and `T{...}` or implicit conversions built only from those. An ordinary immutable variable (`u32 x = 5;`) is a run-time value.

### Diagnostic functions (in `std::`)
These are used in block-form checks, and anywhere else compile-time code runs (such as `if compile_time` branches, see 15):
```rift
compile_time never std::compile_error(std::format_args message);
compile_time never std::compile_error(std::static_str id, std::format_args message);
compile_time void  std::compile_warning(std::static_str id, std::format_args message);
compile_time void  std::compile_note(std::format_args message);
// each also has an overload taking a std::source_location first
compile_time std::source_location std::location_of(<parameter>);
compile_time std::static_str      std::type_name<T>();
compile_time bool                 std::is_compile_time(<parameter>);   // block form only, see example 7
```
- **Warnings require an id** (`"gfx::deprecated"`). Users switch them per id: `riftc --warn=gfx::deprecated=off|error`. An id on an error is optional; `riftc explain <id>` shows the `///` doc comment above the check.
- Notes emitted during a check are attached to that check's error or warning, in whatever order they're called.
- A **panic during compile-time evaluation** is a compile error with the panic's message.
- Check evaluation has a step budget. Exceeding it is an error, which keeps builds fast (principle 9).

### Where errors are reported
- A failed entry check is reported **at the call**, underlining the arguments the condition reads. One note points to the check (`check in uart.rifts:14`).
- A type-body check is reported at the code that instantiated the type.
- In general: at the first location *outside the module that wrote the check*, plus at most 3 notes for the call/instantiation chain. If everything is in one module, it is reported at the check itself.
- A failed `run_time` entry check is evaluated at the call site, so the panic message names **the caller's line**, followed by the check's location.

### Headers and compilation
Checks live **only in the `.rifts`**. The source is the single source of truth, and headers stay a plain list of declarations. The checks still reach importers through the **compiled module interface**, the same way template bodies do (see 09, 14). It carries:

| Carried | Not carried |
|---|---|
| Every function's entry checks | Checks elsewhere in function bodies (they're compiled into that function) |
| Type-body checks (for template instantiation) | File-scope checks (already evaluated when the module compiled) |

Since importers already wait for the source to compile (for class layout), checks add no new build dependency. Changing a check changes the interface, so importers are recompiled.

## Examples

### 1. Template argument rules (type body)
```rift
// net.rifts
template<type T, usize N>
class ring_buffer {
    compile_time check (N > 0) else f"ring_buffer<{std::type_name<T>()}, 0>: capacity must be at least 1";
    compile_time check ((N & (N - 1)) == 0) else f"ring_buffer capacity must be a power of two, got {N}";
public:
    void push(T value) mut { ... }
    std::option<T> pop() mut { ... }
private:
    std::array<T, N> items = default;
}
```
```rift
// main.rifts
net::ring_buffer<packet, 100> queue = default;
```
```
error: ring_buffer capacity must be a power of two, got 100
 --> main.rifts:4:5
  |
4 |     net::ring_buffer<packet, 100> queue = default;
  |     ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
note: check in net.rifts:4
```
The checks run in order, so the second one never sees `N == 0`.

### 2. A value that must be known at compile time
On a microcontroller, the pin number selects a register, so it has to be a constant:
```rift
// gpio.rifts
void configure_output(u8 pin) {
    compile_time check (pin < 32) else f"pin {pin} doesn't exist: this chip has pins 0..31";
    ...
}
```
```rift
gpio::configure_output(5);          // OK
gpio::configure_output(40);         // compile error: pin 40 doesn't exist ...
gpio::configure_output(user_pin);   // compile error: 'pin' must be a compile-time value
                                    //   (required by the compile_time check in gpio.rifts:3)
```

### 3. Known values early, run-time values safely
```rift
// uart.rifts
compile_time run_time bool is_supported(u32 baud) {
    return baud == 9600 || baud == 38400 || baud == 115200 || baud == 921600;
}

std::result<u32, uart::error> configure(u32 baud) {
    compile_time run_time check (is_supported(baud)) else f"{baud} baud is not supported (use 9600, 38400, 115200 or 921600)";
    ...
}
```
```rift
_ = uart::configure(115201);       // compile error at this line, underlining 115201
_ = uart::configure(user_rate);    // compiles; panics at this line if user_rate is unsupported
```
If a bad run-time value should return `uart::error::bad_baud` instead of panicking, don't use a check for it: write a normal `if` in the body. A failed check always means "the caller has a bug".

### 4. Replacing a generic error message (block form)
```rift
// log.rifts
template<type T : std::formattable>
void value(std::static_str label, T& item) {
    compile_time check {
        if (!std::implements<T, std::formattable>()) {
            std::compile_note("implement std::formattable for it, or log one of its fields");
            std::compile_error(f"log::value: '{std::type_name<T>()}' can't be logged");
        }
    }
    ...
}
```
The user sees this message *instead of* the generic "does not implement `std::formattable`".

### 5. Deprecation
```rift
void draw_quad(f32 x, f32 y) {
    compile_time check {
        std::compile_warning("gfx::deprecated", "draw_quad() is deprecated; use draw_rect(rect) instead");
    }
    ...
}
```
This is an entry check, so every call gets the warning at its own line.

### 6. Layout guarantees (file scope)
```rift
// telemetry.rifts
struct packet_header {
    u16 sync;
    u8  type;
    u8  flags;
    u32 sequence;
    u64 timestamp;
}

compile_time check (std::size_of<packet_header>() == 16) else f"packet_header is a wire format and must be 16 bytes, is {std::size_of<packet_header>()}";
compile_time check (std::align_of<packet_header>() == 8) else "packet_header must be 8-byte aligned";
```

### 7. The relative-path warning, as library code
The warning from 19 is an entry check in the standard library's source:
```rift
// std/file.rifts
class file {
public:
    static std::result<std::file, std::io_error> open(std::filesystem::path& path) {
        compile_time check {
            if (std::is_compile_time(path)) {
                if (path.is_relative() && !path.is_explicitly_relative()) {
                    std::compile_warning("std::relative_path", std::location_of(path),
                        f"\"{path}\" is relative to the working directory, not to the executable");
                }
            }
        }
        ...
    }
}
```
`std::is_compile_time(param)` lets a block-form check look at a parameter **only when** the call gives it a compile-time value. Without it, reading `path` in a `compile_time check` would make a compile-time argument required.

### 8. The `ASSERT` from the C++ example
```rift
run_time check (result == vk::result::success) else f"Failed to create {what} pipeline";
```
There are no macros, so a malformed check is a normal syntax or name error at that line. For example, `{wat}` in the message gives `unknown name 'wat'`, pointing at `wat`. That replaces 90 lines of macro-expansion notes.

## Rationale
- **`compile_time` / `run_time` on the check:** the difference from an assert isn't a different feature but a different *time*. The language already has words for that, so a check reads exactly like a function declaration's modifiers.
- **Source only:** the source is the single source of truth. Headers stay a clean list of declarations, and the compiled interface carries what importers need, the same mechanism as for templates.
- **Entry section:** preconditions are written where a reader looks first, at the top of the function. "At the top" also tells the compiler which checks belong to each call, with no extra syntax.
- **Mode lists on the modifier:** the build mode is just another "when". `run_time(debug)` reads as "at run time, in debug", so gating needs no new keyword.
- **`compile_time check` requires compile-time arguments instead of silently skipping:** a check you can't be sure ran is not a requirement. Code that wants "check if possible" says so with `compile_time run_time`.
- **Checks only report:** they can't generate code or change which overload is chosen. They can stop a build or a program, never change what it does.
- **The library can replace compiler rules:** the relative-path warning passes the boundary test (see 20), so it lives in `std`.

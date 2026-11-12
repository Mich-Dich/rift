# 24 - Build Modes

A **build mode** is a named set of settings that applies to a whole build: optimization, debug info, how arithmetic and checks behave, and which mode-gated checks (see 23) are compiled in. Rift has three built-in modes. Projects define their own in `build.rifts` (see 25).

## Rules

### Built-in modes
| Mode | Base | Optimization | Debug info | Overflow, shifts, `bit(n)` | Bounds | `run_time` checks |
|---|---|---|---|---|---|---|
| `debug` | - | none | full | panic | panic | ungated + `debug` |
| `release` | - | speed | none | panic | panic | ungated + `release` |
| `release_with_debug_info` | `release` | speed | full | panic | panic | ungated + `release` |

```
riftc build                                 # debug (the default)
riftc build --mode=release
riftc build --mode=profile                  # a mode defined in build.rifts
```

**All built-in modes are safe.** The language rules from 16 (overflow panics, bounds checks, etc.) hold in each of them.

### Mode inheritance
Every mode except `debug` and `release` has a **base** mode and inherits all its settings, then overrides some. A mode "is" its base: `release_with_debug_info` is a `release` mode. That matters for mode-gated checks and `std::build::mode_is`.

### Mode settings
A mode is a `std::build::mode` value:

| Field | Values | Default (from base) |
|---|---|---|
| `name` | `std::static_str`, `snake_case`, unique | - (required) |
| `base` | `std::build::modes::debug`, `::release`, `::release_with_debug_info`, or another user mode | - (required for user modes) |
| `optimization` | `none`, `size`, `speed` | base |
| `debug_info` | `none`, `line_tables`, `full` | base |
| `overflow` | `panic`, `wrap` | base |
| `shifts` | `panic`, `mask` (count `& (width - 1)`) | base |
| `bounds` | `panic`, **`unchecked`** | base |
| `run_time_checks` | `all`, `gated_only`, **`none`** | base |
| `panic` | `abort`, `trap` (CPU trap instruction, no message) | base |
| `link_time_optimization` | `bool` | base |
| `allow_undefined_behavior` | `bool` | `false` |

### Modes can change safety rules, explicitly
A user mode may relax the language's safety rules. There are two levels:

1. **Still defined behaviour:** `overflow = wrap`, `shifts = mask`, `panic = trap`. The program is still free of undefined behaviour; it just behaves differently from the built-in modes on overflow and on bad shifts.
2. **Undefined behaviour possible:** `bounds = unchecked` and `run_time_checks = none` (or `gated_only`) remove the guards that safe code relies on. These are only accepted together with **`.allow_undefined_behavior = true`**. Without it, the build script is a compile error.

Every build in a mode that relaxes a rule prints one line at the start, which also appears in CI logs:
```
note: building in mode 'fast' (base: release): overflow wraps, bounds checks off - undefined behaviour is allowed
```

Code that must keep a guarantee in **every** mode uses the explicit operators, which never change with the mode:

| Always | Operation |
|---|---|
| Wraps | `+%`, `-%`, `*%` |
| Checks and returns `std::option` | `std::checked_add`, `a.get(i)`, `cast::checked` |
| Saturates | `std::saturating_add`, `cast::saturate` |

`+`, `-`, `*`, `a[i]`, `<<` and `bit(n)` follow the mode.

### Reading the mode in code
All of these are `compile_time` functions:
```rift
std::build::mode m = std::build::current_mode();            // the full settings
bool is_dbg = std::build::mode_is(std::build::modes::debug); // true for debug and every mode based on it

if compile_time (std::build::current_mode().debug_info != std::build::debug_info::none) {
    ...
}
```
- `mode_is` with a user mode's name works only if the build defines that mode. Library code should use the built-in modes.
- Prefer testing a **setting** (`.overflow == wrap`) over a mode name: the code then also does the right thing in modes it has never heard of.

### Scope of a mode
- A build runs in **one mode**. Every module, including dependencies, is compiled in that mode, so compiled interfaces are kept per mode.
- `build.rifts` may override the mode **per target**, for example to build heavy dependencies with `release` inside a `debug` build (see 25). A target then uses that mode for all its own modules.
- **Entry checks follow the caller.** A `run_time` entry check (see 23) is compiled into each call, so the calling module's mode decides whether it's there. All other checks follow the mode of the module they're written in.

## Example
```rift
// build.rifts (excerpt, see 25)
g.add_mode(std::build::mode{
    .name = "profile",
    .base = std::build::modes::release_with_debug_info,
    .link_time_optimization = true,
});

g.add_mode(std::build::mode{
    .name     = "fast",
    .base     = std::build::modes::release,
    .overflow = std::build::overflow::wrap,
    .bounds   = std::build::bounds::unchecked,
    .allow_undefined_behavior = true,       // required because of .bounds
});
```

## Rationale
- **Three built-in modes, all safe:** anyone building a project knows what `debug` and `release` mean, and a library can rely on them.
- **User-defined modes inherit:** `profile` is a variation of `release_with_debug_info`, so checks and code written for `release` apply to it automatically.
- **Relaxing safety is possible, but never quiet:** the mode has to say what it relaxes; undefined behaviour additionally needs `allow_undefined_behavior`, and every build says so. That keeps principle 8 (control when you ask for it) without breaking principle 3 (safe by default).
- **Explicit operators don't follow the mode:** code that depends on wrapping or checking (hashes, checksums, parsers of untrusted input) says so with `+%` or `checked_*` and stays correct in every mode.

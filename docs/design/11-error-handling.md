# 11 - Error Handling

## Rules

### Recoverable errors: `std::result` and `std::option`
- `std::result<T, E>` is used for operations that can fail. Its alternatives are `std::ok(x)` and `std::error(e)`.
- `std::option<T>` is used for values that may be absent. Its alternatives are `std::some(x)` and `std::none`.
- No exceptions: there is no `throw`, `try` or `catch`. Foreign code that may throw must be wrapped in `unsafe` (see 13).
- **Results must be used.** Ignoring a returned `std::result` is a compile error. To discard one on purpose, write `_ = f();`.

### Consuming a result
| Form | Effect |
|---|---|
| `match (r) { ... }` | Handle each case |
| `r?` | Unwrap `ok`, or `return` the error from the current function |
| `r.or_error(e2)` | Replace the error with `e2`; returns `result<T, E2>` |
| `r.map_error(f)` | Convert the error with a function or lambda |
| `r.value_or(x)` | `ok` value, or `x` |
| `r.expect(f"...")` | `ok` value, or **panic** with the message |

`?` requires the **error types to be identical**. There is no hidden conversion; convert with `.or_error` or `.map_error`:
```rift
std::result<employee, parse_error> parse_line(std::str line) {
    std::darray<std::str> parts = line.split(',');
    if (parts.len() != 3) {
        return std::error(parse_error::bad_line);
    }
    u32 age    = parts[1].parse<u32>().or_error(parse_error::bad_number)?;
    f64 salary = parts[2].parse<f64>().or_error(parse_error::bad_number)?;
    return std::ok(employee{
        .name   = std::string::from(parts[0]),
        .age    = age,
        .salary = salary,
    });
}
```
`?` on a `std::option` works the same way, but only inside functions that return `std::option`.

### Unrecoverable errors: panic
A **panic** prints a message and the source location to stderr, then **aborts** the process. There is no unwinding and destructors don't run.

These things panic:

| Trigger | See |
|---|---|
| `std::panic(f"...")` | - |
| A failed `run_time check` (or the run-time half of `compile_time run_time check`) | 23 |
| Integer overflow, division by zero | 16 |
| Shift count ≥ bit width, `bit(n)` out of range at run time | 16 |
| Index out of bounds | 20 |
| `.expect(...)` on `error`/`none` | above |
| `main` returning an integer the OS can't represent | 04 |

`std::panic` has return type `never`.

## Rationale
- **No implicit error conversion:** v0.2 depended on an undesigned `std::convert` trait. `.or_error(parse_error::bad_number)` is short, and it shows exactly which error the caller gets.
- **Must-use results:** an ignored error is a silent bug. `_ =` makes ignoring it a visible decision.
- **Abort, not unwind:** there are no exceptions, so unwinding would only exist for panics. Abort keeps codegen simple and predictable. A build mode can replace the message + abort with a CPU trap (`.panic = trap`, see 24), for example on embedded targets. A custom panic handler is still open (see 22).

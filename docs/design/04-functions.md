# 04 - Functions

## Rules

### Declaration
```
[compile_time [run_time]] <return_type> <name>(<parameters>) { <body> }
```
```rift
std::result<employee, parse_error> parse_line(std::str line) { ... }
void log(std::str message) { std::println(message); }
```
- The return type is always written. `deduce` is only allowed in templates (see 15).
- No default arguments and no named arguments. Write an overload or pass a struct instead.
- **Overloading** by parameter types is allowed. A call that matches more than one overload is an error (for example `f(5)` with both `f(u32)` and `f(u64)`; write `f(5u32)`). Overloading on return type alone is not allowed.
- There are no nested functions; use lambdas (see 18).

### Parameter passing
| Parameter | Meaning | Call site |
|---|---|---|
| `T x` | By value. Copy types are copied; other types must be moved or cloned in | `f(v)`, `f(move s)`, `f(s.clone())` |
| `mut T x` | By value, mutable inside the function | Same as above |
| `T& x` | Immutable reference | `f(v)` |
| `mut T& x` | Mutable reference | **`f(mut v)`**, where `v` must be a `mut` variable |

```rift
void bump(mut u32& x) { x += 1; }

mut u32 counter = 0;
bump(mut counter);        // OK: you can see that counter may change
bump(counter);            // ERROR: mutable reference needs 'mut' at the call site
```
- Only the **receiver** of a method call (`obj.method()`) has no call-site marker. Whether the method is `mut` is checked against the variable (see 06).
- **One mutable reference at a time:** `f(mut a, a)` and `f(mut a, mut a)` are errors (see 12).
- References may be returned only under the rules in 12.

### `main`
```rift
std::exit_code main() { ... }
std::exit_code main(std::span<std::str> args) { ... }
```
- Return type: `void`, `std::exit_code`, or any integer type. If an integer return value is outside the platform's exit-status range, the program panics instead of silently truncating it.
- `args[0]` is the executable path; `args[1..]` are the user's arguments. If an argument isn't valid UTF-8, the program stops at startup with an error message. Raw bytes are available through `std::env::raw_args()`.

```rift
// examples/compile_check.rifts - valid:
u32 main() {
    return 0;            // 0 becomes u32 from the return type; 0u32 also works
}
```

## Rationale
- **`mut` at the call site:** this is the most useful single marker for readers. `sort(mut list)` tells you at the call that `list` changes, without looking up the signature. Immutable references stay unmarked because they can't cause surprises.
- **No receiver marker:** writing `mut employees.push(x)` on every method call would be noise. The variable itself is already declared `mut`, which is the visible marker.
- **No default arguments:** they hide values at the call site, and they interact badly with overloading.
- **`std::span<std::str>` for `main`:** the runtime already holds the arguments in memory, so viewing them costs nothing. `std::darray<std::string>` would allocate before `main` even runs.
- **Parameter grouping `T& {a, b}` removed:** it was a second way to write the same parameter list.

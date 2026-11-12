# 15 - Compile-Time Evaluation

## Rules

### Function modifiers
| Declaration | At compile time? | At run time? |
|---|---|---|
| `T foo(...)` | No | Yes |
| `compile_time T foo(...)` | **Yes** | No |
| `compile_time run_time T foo(...)` | Yes | Yes |

- `run_time` on its own is an error: it's the default.
- A `compile_time` function:
  - may only be called with compile-time arguments,
  - may only call other `compile_time` functions,
  - may not do I/O or touch `static mut`.
- A `compile_time run_time` function follows the same body restrictions, so it behaves identically in both modes.

```rift
compile_time u32 square(u32 x) { return x * x; }

compile_time run_time u32 clamp(u32 x, u32 lo, u32 hi) {
    return if (x < lo) { lo } else if (x > hi) { hi } else { x };
}

u32 a = square(5);                   // compile time
u32 b = square(runtime_value);       // ERROR: argument is not a compile-time value
u32 c = clamp(runtime_value, 0, 10); // run time
```

### Compile-time values
```rift
compile_time usize buffer_size = 4096;      // file scope or local
compile_time u32 area = square(16);
```
- These are immutable, with no storage and no address.
- They can be used as template arguments: `std::array<u8, buffer_size>`.
- Values that need heap memory (`std::string`, `std::darray`) may be built at compile time. They're stored in **read-only static memory** and can never be changed:
  ```rift
  compile_time std::darray<argument_spec> specs = build_specs();
  ```

### `if compile_time`
```rift
if compile_time (std::is_integer<T>()) { ... } else { ... }
```
- The condition must be a compile-time `bool`.
- The branch that isn't taken is **not type-checked** for this instantiation, so it may use operations `T` doesn't support.

### `deduce`: return type chosen by branch
Allowed only in templates. It replaces the return type when different `if compile_time` branches return different types:
```rift
template<type T>
deduce double_it(T x) {
    if compile_time (std::is_integer<T>()) {
        return cast<i64>(x) * 2;
    } else if compile_time (std::is_float<T>()) {
        return cast<f64>(x) * 2.0;
    } else {
        std::compile_error("double_it: T must be an integer or a float");
    }
}

i64 a = double_it(42i32);            // → i64
f64 b = double_it(3.14f32);          // → f64
f32 c = double_it(3.14f32);          // ERROR: f64 doesn't convert to f32 implicitly
```
- The return type is fixed **per instantiation**, so arguments may be ordinary run-time values.
- Within one instantiation, every `return` must have the same type.
- If no branch returns for a `T`, that's an error. `std::compile_error(...)` gives a readable message for it.

### Built-in compile-time functions
`std::is_integer<T>()`, `std::is_float<T>()`, `std::is_same<A, B>()`, `std::implements<T, iface>()`, `std::size_of<T>()`, `std::align_of<T>()`, `std::type_name<T>()`.

Checks (`compile_time check`, `run_time check`) and the diagnostic functions (`std::compile_error`, `std::compile_warning`, `std::compile_note`) are described in 23.

## Rationale
- **`run_time` spelling:** matches `compile_time`, so the two read as a pair.
- **`deduce` alone:** v0.2 wrote `auto deduce`, two words for one idea. Removing the "arguments must be constant" restriction is correct because the branch depends on the *type*, not on the value.
- **Compile-time heap data in static memory:** this makes the "argument spec table" idea from v0.2's *Future thoughts* possible without any runtime cost.

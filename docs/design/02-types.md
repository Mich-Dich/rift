# 02 - Built-in Types and Conversions

## Rules

### Built-in types
| Type | Meaning |
|---|---|
| `i8` `i16` `i32` `i64` `i128` | Signed integers, two's complement |
| `u8` `u16` `u32` `u64` `u128` | Unsigned integers |
| `isize` `usize` | Pointer-sized integers. Used for sizes and indices (`len()` returns `usize`) |
| `f32` `f64` | IEEE-754 floats |
| `bool` | `true` / `false` |
| `char` | One Unicode scalar value (4 bytes) |
| `void` | "No value": a return type only |
| `never` | The type of expressions that never finish: `return`, `break`, `continue`, `std::panic(...)` |

There is no `int`. Fixed-size arrays are `std::array<T, N>` and growable arrays are `std::darray<T>` (see 20). There are no C-style `T[N]` arrays.

### Literal typing
An unsuffixed literal **takes its type from context**. If there is no context, that is a compile error.

Context comes from:

1. the declared type of the variable being initialized or assigned,
2. the parameter type at a call,
3. the function's return type in `return`,
4. the other operand of a binary operator, if that operand's type is known,
5. the element or field type in `T{ ... }` and in container literals.

```rift
u32 a = 0;                 // OK: u32 from the declaration
u64 b = a + 1;             // ERROR: a is u32 and the result is u64 (no implicit widening)
u32 c = a + 1;             // OK: 1 becomes u32 from the other operand
u8  d = 300;               // ERROR: 300 doesn't fit in u8
f64 e = 0;                 // ERROR: an integer literal never becomes a float; write 0.0
auto f = 5;                // ERROR: no context; write 5u32 or `u32 f = 5;`
auto g = 5u32;             // OK: the suffix gives the type
if (3 < 4) { }             // ERROR: no context for either literal
```

### No implicit numeric conversion
Mixing integer widths, signedness or int/float in one operation is a compile error. Convert with `cast` (see 17).

### Contextual conversion to `bool`
In a **condition context**, integers and `std::raw_ptr` convert to `bool` implicitly:

| Source type | Becomes `true` when |
|---|---|
| Any integer type | value `!= 0` |
| `std::raw_ptr<T>` | pointer `!= nullptr` |

**Condition contexts:** the condition of `if` and `while`, operands of `!`, `&&` and `||`, and `match` guards.

Not converted: floats (NaN makes "is zero" ambiguous), `char`, enums, `std::option`, smart pointers (never null, see 13) and user-defined types.

```rift
if (count) { ... }                 // OK
if (flags & bit(4)) { ... }        // OK: u32 result, condition context
while (ptr) { ... }                // OK (std::raw_ptr)
bool b = count;                    // ERROR: not a condition context; write count != 0
take_flag(count);                  // ERROR: passing u32 to a bool parameter
if (ratio) { ... }                 // ERROR: f64 doesn't convert
```

### The complete list of implicit conversions
This is **every** implicit conversion in the language. Anything else needs a `cast` or a function call.

| From | To | Where | Note |
|---|---|---|---|
| Integer, `std::raw_ptr` | `bool` | Condition contexts | See above |
| `std::string` | `std::str` | Anywhere | Borrow, no cost |
| `std::static_str` (string literal) | `std::str` | Anywhere | No cost |
| `std::static_str` | `std::string` | Anywhere | **Compiler warning:** allocates. Use `std::string::from(...)` (see 19) |
| `derived&` / pointer to derived | `base&` / pointer to base or interface | Anywhere | Upcast is safe and lossless (see 07) |
| Unsuffixed literal | Context type | Where context exists | See literal typing |

## Rationale
- **Context-typed literals:** `u32 x = 0;` is clearly a `u32`, so requiring `0u32` would only be noise. When there's no context, guessing `i32` like C would hide a decision, so it's an error instead.
- **No int → float literals:** `f64 half = 1 / 2;` would otherwise be `0.5` here but `0` in C. Writing `1.0` removes the doubt.
- **Bool conversion only in conditions:** you get the convenience of `if (ptr)` and `if (flags & mask)` without the C++ pitfall where `5` silently binds to a `bool` parameter.
- **`usize`:** sizes depend on the target (32-bit embedded vs 64-bit desktop). Hard-coding `u64` would be wrong on small targets.

# 08 - Enums

One construct covers both simple enumerations and sum types (tagged unions).

## Rules

### Simple enums
```rift
enum parse_error : u8 {
    bad_line = 0,
    bad_number,          // 1
}
```
- The underlying (tag) type is **mandatory**.
- Values count up from the last explicit value.
- Always scoped: `parse_error::bad_line`.
- No implicit conversion. `cast<u8>(parse_error::bad_number)` gives `1`. Going the other way, `parse_error::from_value(1)` returns `std::option<parse_error>`.

### Enums with payloads
Each alternative may carry **one** value in parentheses:
```rift
enum token : u8 {
    number(f64),
    identifier(std::string),
    plus,
    end,
}

token t = token::number(1.5);
token u = token::end;
```
- An alternative with a payload is built with `()`. An alternative is a value constructor, not a type, so the "types are built with `{}`" rule (see 03) doesn't apply to it. An alternative without a payload is just its name.
- Several values go into a struct payload: `circle(circle_data)`.
- Explicit tag values (`= 3`) are only allowed for enums without payloads.
- Enums are copy types if every payload is a copy type.

### Matching
`match` on an enum must be exhaustive (see 10):
```rift
f64 value = match (t) {
    token::number(n)     => n,
    token::identifier(_) => 0.0,
    token::plus          => 0.0,
    token::end           => 0.0,
};
```

### Standard result types
`std::result` and `std::option` are ordinary payload enums from the standard library:
```rift
template<type T, type E>
enum result : u8 { ok(T), error(E) }

template<type T>
enum option : u8 { some(T), none }
```
**The one naming exception:** the standard library exports their alternatives directly in `std::`, so you write `std::ok(x)`, `std::error(e)`, `std::some(x)` and `std::none` instead of `std::result<T, E>::ok(x)`. Their template arguments come from context, in the same way as literals do:
```rift
std::result<u32, parse_error> r = std::error(parse_error::bad_line);   // T, E from the declaration
auto bad = std::ok(5u32);                                              // ERROR: E is unknown
```

## Rationale
- **Mandatory tag type:** layout and ABI are visible and can be controlled. This matters for serialization and for hardware registers.
- **Payload enums replace `std::variant`:** one way to write a sum type. Named alternatives also read better than `std::variant<circle, square>` with type patterns.
- **`std::ok` exception:** these four names appear on almost every line of error-handling code. Spelling out `std::result<employee, parse_error>::ok(...)` every time would push people towards avoiding results altogether.

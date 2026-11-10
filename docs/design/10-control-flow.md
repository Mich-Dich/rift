# 10 - Control Flow

## Rules

### General
- Braces are mandatory everywhere (see 01).
- **Assignment is a statement, not an expression**, so `if (x = 0)` can't be written.
- There is no `++` / `--`. Write `x += 1;`.
- Conditions are `bool` or contextually converted to it (integers, `std::raw_ptr`; see 02).

### Blocks as values
`if`, `match` and plain `{ }` blocks can be used as expressions. Their value is the **last expression without a `;`**:
```rift
std::str label = if (x > 0) { "positive" } else { "non-positive" };
```
- When used as an expression, an `if` must have an `else`, and every branch must produce the same type.
- Branches of type `never` (`return`, `break`, `continue`, `std::panic(...)`) fit with any type.

### `if` / `else`
```rift
if (ptr) { ... }                       // OK: std::raw_ptr in a condition
if (count > 10) { ... } else if (count) { ... } else { ... }
```

### `while`
```rift
while (file.has_more()) {
    ...
}
```
`break` and `continue` work in `while` and `for`.

### `for`: range-based only
```rift
for (employee& e : employees) { ... }         // immutable reference
for (mut employee& e : employees) { ... }     // mutable reference (employees must be mut)
for (u32 x : numbers) { ... }                 // by value: copy types only
for (usize i : 0..employees.len()) { ... }    // half-open range
for (u8 b : 0..=255) { ... }                  // inclusive range
```
- `a..b` and `a..=b` are range expressions. They use the same syntax as range patterns in `match`. Literal bounds take their type from the loop variable.
- There is no C-style `for (;;)`.
- While a loop holds references into a container, the container can't be changed (see 12).

### `match`
- It is an **expression**, it must be **exhaustive**, and there is **no fallthrough**.
- **Every arm ends with `,`.**
- Patterns: literals, ranges (`0..=9`), enum alternatives (`token::number(x)`, `std::ok(x)`), struct destructuring (`employee{ .age = a, .. }`), `_`, and guards (`x if x > 100`).
- **Bindings:**
  - If the matched expression is a variable, bindings are immutable references.
  - `match (mut v)` makes them mutable references (`v` must be `mut`).
  - If it is a temporary or `move v`, bindings own the values.

```rift
std::str label = match (code) {
    200 => "ok",
    404 => "not_found",
    _   => "other",
};

match (parse_line(line)) {                     // temporary: e is owned
    std::ok(e)    => employees.push(move e),
    std::error(_) => std::eprintln(f"skipping bad line: {line}"),
}

std::darray<employee> list = match (load(path)) {
    std::ok(l)    => move l,
    std::error(e) => {
        std::eprintln(f"error: {e}");
        return std::exit_code::failure;        // type never: fits any arm
    },
};
```

## Rationale
- **Assignment as a statement:** together with contextual bool conversion, this rules out the `if (x = 0)` bug completely.
- **No `++`/`--`:** `+= 1` already exists. One way.
- **Range syntax reused:** v0.2 had both `std::range(0, n)` and `0..=9` in patterns. Now loops and patterns share one syntax.
- **`,` after every arm:** one rule instead of "comma unless the arm is a block".

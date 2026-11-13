# 26 - Metaprogramming

Metaprogramming in Rift is **ordinary compile-time code** (see 15). There is no macro language, no token pasting and no code in strings. Three things are added on top of `compile_time` functions and `if compile_time`:

1. **Types are values** at compile time: `compile_time type t = u32;`.
2. **Reflection:** `std::meta` describes a type's fields and alternatives.
3. **`for compile_time`:** a loop that is written once and copied once per element, so each copy can work with a different type.

Together they cover generic printing, comparison, hashing, serialization and enum ↔ string conversion. Generating *new declarations* (types, fields, functions) is not part of this version (see 22).

## Rules

### Types as values
`type` is a built-in type whose values are types. It exists only at compile time.
```rift
compile_time type id = u32;

compile_time type counter_type(usize max) {
    return if (max <= 255) { u8 } else { u32 };
}

compile_time type small = counter_type(100);
small count = 0;                        // a u8
std::darray<small> history = default;   // a type value works wherever a type is written
```
- A type is written in an expression the same way as in a declaration: `u32`, `point`, `T`, `std::option<u32>`. Reference types (`T&`) cannot be written as values.
- A type value is used **as a type through a name**: a `compile_time` constant, a template parameter, or a parameter of a `compile_time` function. To use a type stored somewhere else (like `f.type` below), give it a name first: `compile_time type ft = f.type;`.
- Type values support `==` and `!=`. `std::type_name<T>()` gives the spelling (see 23).
- A template parameter `T` is a type value, so `T` can be passed to a `compile_time` function that takes a `type`.

**Compile-time-only types.** `type`, the `std::meta` types below, and every struct with a field of one of them exist only at compile time. They can be used in `compile_time` constants, in `compile_time` functions, and as `for compile_time` loop variables. Using one in run-time code (a run-time variable, a parameter of a run-time function, a field of a run-time struct) is a compile error. Their parts that are ordinary values can be used at run time: `f.name` is a `std::static_str`.

### Reflection: `std::meta`
All functions are `compile_time` and take the type as a template argument, like `std::is_integer<T>()` (see 15).

| Function | Result |
|---|---|
| `std::meta::fields<T>()` | `std::span<std::meta::field>`: the fields of a struct or class, in declaration order |
| `std::meta::alternatives<T>()` | `std::span<std::meta::alternative>`: the alternatives of an enum, in declaration order |
| `std::meta::is_struct<T>()`, `is_class<T>()`, `is_enum<T>()` | `bool` |
| `std::is_integer<T>()`, `std::is_float<T>()`, `std::is_same<A, B>()`, `std::size_of<T>()`, `std::align_of<T>()`, `std::type_name<T>()` | As in 15 and 23 |

```rift
struct field {                 // std::meta::field
    std::static_str name;
    type            type;      // the field's declared type
    usize           index;     // position in declaration order
    usize           offset;    // byte offset in the object (the layout of 13)
}

struct alternative {           // std::meta::alternative
    std::static_str name;
    usize           index;
    bool            has_payload;
    type            payload;   // the payload's type; `void` when there is none
}
```

**Using a field.** `f.get(value)` is a reference to that field of `value`. With `mut` it is a mutable reference, written like any other mutable reference argument (see 04):
```rift
u32&     a = f.get(person);          // when f.type is u32
mut u32& b = f.get(mut person);
```

**Using an alternative.**
| Method | Result |
|---|---|
| `a.is(value)` | `bool`: `value` holds this alternative |
| `a.get(value)` | `std::option<P&>`: the payload if `value` holds this alternative (only when `has_payload`) |
| `a.make()` / `a.make(payload)` | A new value of the enum holding this alternative |

- `f` and `a` must be **compile-time values** (a `for compile_time` loop variable, a `compile_time` constant). The type of `f.get(value)` depends on which field `f` is, so the compiler must know it.
- **Privacy is respected.** `std::meta::fields<T>()` lists the fields that are visible where the call is written (see 06, 09). A generic function in another module sees the public fields of a class, and every field of a struct. There is no reflective way around `private`.
- Reflection is read-only. It cannot add, remove or rename fields, alternatives or functions.

### `for compile_time`
```rift
for compile_time (std::meta::field f : std::meta::fields<T>()) {
    ...
}
```
- The range is a compile-time value: a `std::span` of compile-time values, or `a..b` / `a..=b` with compile-time bounds.
- The body is **copied once per element**, in order. In each copy the loop variable is a compile-time constant with that element's value, and each copy is type-checked on its own. This is how one loop body can work with fields of different types.
- `break` leaves the remaining copies; `continue` goes on with the next copy. `return` returns from the function. Both may sit inside run-time `if`s.
- The loop is a statement with no value.
- Inside a `compile_time` function, an ordinary `for` over `std::meta::fields<T>()` also runs at compile time. Its loop variable is not a compile-time constant, though, so it can read `f.name` or `f.index` but cannot call `f.get(...)`. Use `for compile_time` when the body depends on the element's type.

## Examples

### 1. Printing any struct
```rift
template<type T>
void describe(T& value) {
    compile_time check (std::meta::is_struct<T>()) else f"describe needs a struct, not {std::type_name<T>()}";
    std::print(f"{std::type_name<T>()} {{");
    for compile_time (std::meta::field f : std::meta::fields<T>()) {
        std::print(f" .{f.name} = {f.get(value)}");
    }
    std::println(" }");
}

struct point { f64 x; f64 y; }
describe(point{ .x = 1.0, .y = 2.5 });      // point { .x = 1.0 .y = 2.5 }
```

### 2. Field-by-field equality
```rift
template<type T>
bool fields_equal(T& a, T& b) {
    for compile_time (std::meta::field f : std::meta::fields<T>()) {
        if (f.get(a) != f.get(b)) {
            return false;
        }
    }
    return true;
}
```
Each copy compares one field with that field's own `!=`. A field type without `!=` is a compile error that points at this line and names the field's type.

### 3. Enum ↔ string
```rift
template<type E>
std::static_str name_of(E value) {
    for compile_time (std::meta::alternative a : std::meta::alternatives<E>()) {
        if (a.is(value)) {
            return a.name;
        }
    }
    std::panic("unreachable: every value holds one alternative");
}

template<type E>
std::option<E> parse_enum(std::str text) {
    for compile_time (std::meta::alternative a : std::meta::alternatives<E>()) {
        if compile_time (!a.has_payload) {
            if (text == a.name) {
                return std::some(a.make());
            }
        }
    }
    return std::none;
}
```

### 4. Choosing a type
```rift
compile_time usize max_players = 100;
compile_time type  player_id   = counter_type(max_players);   // u8

player_id next = 0;
counter_type(max_players) other = 0;   // ERROR: a type value is used as a type through a name
```

## Rationale
- **Ordinary compile-time code, not macros:** metaprograms use the same syntax, types and error messages as the rest of the language. A reader who knows Rift can read them, and the compiler can check them like any other code.
- **`type` as a value:** one more value kind instead of a second language for type computations. Functions such as `counter_type` read like normal code.
- **Type values are used through names:** a declaration `f.type x = ...;` or `counter_type(N) x = ...;` would make every type position also an expression position, which makes C++-like syntax ambiguous. Naming the type first keeps the grammar simple and the code readable.
- **`f.get(value)` instead of a splice operator:** no new symbol. It reads as what it does, and `mut` marks a mutable access the same way as everywhere else.
- **`for compile_time` mirrors `if compile_time`:** the same word says "decided by the compiler", and both type-check each copy or branch on its own.
- **Template-argument form for `std::meta`:** consistent with the traits of 15 (`std::is_integer<T>()`).
- **Privacy:** reflection that could read private fields would make `private` advisory. Listing only visible fields keeps 06 and 09 true.
- **No declaration generation yet:** most real uses (printing, comparing, hashing, serializing, enum names) only need to *read* types. Generating types adds hard questions (names, headers, error locations); see 22.

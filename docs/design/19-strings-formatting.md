# 19 - Strings and Formatting

## Rules

### String types
| Type | Owns? | Kind | Use |
|---|---|---|---|
| `std::string` | Yes (heap) | Owning | Building and storing text |
| `std::str` | No | `view` (see 12) | Parameters and borrowed text |
| `std::static_str` | No (static memory) | Normal value | Type of string literals; can be stored anywhere |

All three hold UTF-8.

### Conversions
| From → To | How |
|---|---|
| `std::string` → `std::str` | Implicit (borrow) |
| `std::static_str` → `std::str` | Implicit |
| `std::str` → `std::string` | `std::string::from(s)` (allocates) |
| `std::static_str` → `std::string` | Implicit, **with a compiler warning** |

```rift
std::str      a = "hello";                         // OK
std::string   b = std::string::from("hello");      // OK: allocation is visible
std::string   c = "hello";                         // WARNING: implicit allocation of std::string from a literal
std::static_str d = "config.toml";                 // OK: can be stored in a struct field
```

### String literals
- `"..."`: escapes `\n \t \r \\ \" \0 \u{XXXX}`. **Never interpolated**, so `"{x}"` is just the characters `{x}`.
- `r"..."`: raw, with no escape processing.
- `f"..."`: format string (below).

### Format strings: `f"..."`
```rift
std::println(f"{e.name:<20} {e.age:>3} {e.salary:>10.2}");
std::string s = std::format(f"{count} items");      // allocates: visible through the function
```
- `{expr}` inserts any expression whose type implements `std::formattable`.
- `{expr:spec}` adds a format spec: alignment `<` `>` `^`, width, `.precision`, `x`/`b`/`o` for hex/binary/octal, `+` for the sign.
- `{{` and `}}` produce a literal `{` and `}`.
- The **spec and the types are checked at compile time**.
- An `f"..."` produces a `std::format_args`, a view type that **doesn't allocate**. `std::println`, `std::eprintln`, `std::format` and `std::panic` take it. Only `std::format` allocates, and it returns `std::string`.

### File paths
```rift
std::file in = std::file::open("employees.csv")?;                     // WARNING: relative path literal
std::file in = std::file::open(std::filesystem::executable_dir() / "employees.csv")?;   // OK
std::file in = std::file::open(std::filesystem::path::relative("employees.csv"))?;     // OK: explicitly relative
std::file in = std::file::open(user_path)?;                           // OK: run-time value, caller's choice
```
- The warning only applies to paths known at compile time, such as **string literals**.
- It isn't built into the compiler: it's an entry check of `std::file::open` in the standard library's source (see 23, example 7). Silence it with `--warn=std::relative_path=off`.
- `std::filesystem::executable_dir()` returns the directory that contains the executable. It was called `get_executable_path` in v0.2; the new name says it's a directory.

## Rationale
- **`std::static_str`:** literals live forever, so they don't need view restrictions. That lets you write `struct option_spec { std::static_str name; }` for tables of options.
- **Warning on `std::string s = "..."`:** it's legal and convenient, but it hides an allocation. The warning keeps principle 7 without making every string declaration noisy.
- **`f` prefix:** in v0.2, every string with `{` was implicitly a format string. An explicit prefix makes interpolation visible, and plain strings stay plain.

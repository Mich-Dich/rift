# 01 - Lexical Structure

## Rules

### Files
| Extension | Meaning |
|---|---|
| `.rifts` | Source file (implementation) |
| `.rifth` | Header file (interface), see 09 |

- Source text is UTF-8.
- A `.rifts` file without a matching `.rifth` is a standalone file. It can't be imported, which suits a program's `main` file.

### Comments
```rift
// line comment
/* block comment, does not nest */
```

### Identifiers and naming
| Category | Convention | Enforced? |
|---|---|---|
| Keywords | `snake_case` | Yes (compiler) |
| Built-in types | `snake_case` | Yes (compiler) |
| Standard library | `snake_case`, written out in full (see 20) | Yes (stdlib authoring rule) |
| User-defined names | Any casing | No |

Identifiers are ASCII letters, digits and `_`, and must not start with a digit. `_` alone is the wildcard (in patterns, and in `_ = expr;`).

### Keywords
```
import  struct  class  interface  enum  template  type
open  virtual  override  abstract  operator
public  protected  private  static  extern
mut  move  this  view
compile_time  run_time  deduce  check
if  else  for  while  break  continue  return  match
true  false  nullptr  default  auto  cast  bit  unsafe
```
Built-in type names, which are reserved like keywords:
```
bool  char  void  never
i8 i16 i32 i64 i128 isize
u8 u16 u32 u64 u128 usize
f32 f64
```

### Literals
| Kind | Examples | Notes |
|---|---|---|
| Integer | `42`, `0xFF`, `0b1010`, `0o17`, `1_000_000` | `_` separators are allowed anywhere after the first digit |
| Integer with suffix | `42u32`, `0xFFu8`, `7usize` | Suffix = any integer type name |
| Float | `1.0`, `0.5`, `1e-3`, `2.5e10` | A float must contain `.` or `e` |
| Float with suffix | `1.0f32`, `3.14f64` | |
| Bool | `true`, `false` | |
| Char | `'a'`, `'\n'`, `'\u{1F600}'` | Type `char` |
| String | `"hello\n"` | Type `std::static_str`, never interpolated (see 19) |
| Raw string | `r"C:\path\no\escapes"` | No escape processing |
| Format string | `f"x = {x}"` | Interpolated (see 19) |
| Null pointer | `nullptr` | Only for `std::raw_ptr` (see 13) |

Unsuffixed literals take their type from context (see 02).

### Statements, `;` and braces
- `;` ends **statements** and **declarations without a body** (`void f();` in a header).
- A `}` that closes a type, function, block or control-flow statement is **never** followed by `;`.
- A declaration whose initializer ends in `}` still ends with `;`, because the declaration is the statement:
  ```rift
  u32 x = match (code) { 0 => 1, _ => 2, };
  auto f = [](u32 x) { return x + 1; };
  ```
- Braces are **mandatory** for `if`, `else`, `for`, `while`, `match` and every function body.

## Examples
```rift
struct point {
    f64 x = 0.0;
    f64 y = 0.0;
}

enum color : u8 {
    red = 0,
    green,
}

void log(std::str message);    // header declaration: ';' ends it
```

## Rationale
- **No `;` after types:** C needs it for `struct x {...} instance;`. Rift doesn't have that form, so the `;` only causes errors when it's forgotten.
- **Mandatory braces:** this rules out goto-fail-style bugs where an unbraced second line looks like part of the `if`. It's also required for `if` to work as an expression (see 10).
- **Removed keywords:** each one was either redundant (`const`, `consteval`), replaced by something else (`null` → `nullptr`, `runtime` → `run_time`) or unused (`in`, `impl`).

# 16 - Operators

## Rules

### Precedence (highest first)
| Level | Operators | Associativity |
|---|---|---|
| 1 | Postfix: `f()` `a[i]` `.` `->` `?` | Left |
| 2 | Prefix: `-` `!` `~` `*` (deref) `&` / `mut &` (address) `move` | Right |
| 3 | `*` `/` `%` `*%` | Left |
| 4 | `+` `-` `+%` `-%` | Left |
| 5 | `<<` `>>` | Left |
| 6 | `&` | Left |
| 7 | `^` | Left |
| 8 | `\|` | Left |
| 9 | `..` `..=` | None |
| 10 | `==` `!=` `<` `<=` `>` `>=` `<=>` | **None**: `a < b < c` is an error |
| 11 | `&&` | Left |
| 12 | `\|\|` | Left |

Assignment (`=`, `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`, `+%=`, `-%=`, `*%=`) is a **statement**, not an expression (see 10).

**Unlike C, bit operators bind tighter than comparisons**, so `flags & mask == 0` means `(flags & mask) == 0`.

### Arithmetic is checked
| Situation | Default `+ - *` | Wrapping `+% -% *%` |
|---|---|---|
| Overflow | **panic** | wraps around (two's complement) |

- The default is **the same in debug and release builds**.
- Division or `%` by zero panics. So does `i32::min / -1`.
- More explicit variants: `std::checked_add(a, b)` returns `std::option`; `std::saturating_add(a, b)` clamps (and the same for sub/mul).
- Integer division truncates toward zero. The sign of `%` follows the left operand.
- Floats follow IEEE-754 and never panic.

### Shifts
- `x << n` and `x >> n`: the count `n` takes its type from `x` if it's an untyped literal.
- `n >= bit width`: **compile error** if `n` is constant, **panic** at run time otherwise.
- `>>` on signed types is arithmetic (copies the sign bit); on unsigned types it's logical.

### `bit(n)`
`bit(n)` is the value with only bit `n` set, the replacement for `1 << n`.

- `bit` is a **keyword expression**.
- **The type must be known from context** (see 02), or written explicitly as `bit<T>(n)`. There is no fallback type.
- **Integer types only.**
- **Bounds:** for a constant `n`, `n >= bit width` is a compile error. A run-time `n` is allowed and panics when it's out of range.

```rift
u16 x = bit(4);                       // OK: u16
auto y = bit(4);                      // ERROR: type unknown; write u16 y = bit(4) or bit<u16>(4)
auto z = bit<u128>(100);              // OK: explicit

mut u32 flags = 0;
flags |=  bit(4);                     // u32 from flags
flags &= ~bit(4);
flags ^=  bit(4);
if (flags & bit(4)) { ... }           // u32 from flags; contextual bool (see 02)
if (bit(4)) { ... }                   // ERROR: no type anywhere
u32 mask = bit(0) | bit(3) | bit(7);  // u32 from the declaration
u32 dyn  = bit(n);                    // run-time n: panics if n >= 32
u8  bad  = bit(8);                    // ERROR: 8 >= width of u8
```

`std::bitset<N>` is the library type for bit arrays wider than one integer (see 20).

### Operator overloading
Allowed for user types with the `operator` keyword, as a method or a free function:
```rift
std::filesystem::path operator/(std::filesystem::path& lhs, std::str rhs) { ... }
bool operator==(employee& other) = default;
std::ordering operator<=>(version& other) { ... }
T& operator[](usize i) { ... }
mut T& operator[](usize i) mut { ... }
```
- **Overloadable:** `+ - * / % & | ^ ~ << >>`, unary `-`, `== <=>`, `[]`.
- **Not overloadable:** `&& || ! = . -> ? move`, `+% -% *%`, casts, and conversion to `bool`.
- `!=`, `<`, `<=`, `>`, `>=` come from `==` and `<=>` automatically.
- `a += b` uses `operator+=` if it's defined, and `a = a + b` otherwise.

## Rationale
- **Precedence fix:** C's precedence for `&` and `|` is a known mistake from 1972. Go, Rust and Swift have all fixed it.
- **Overflow panics in release too:** "no undefined behaviour" must hold in the build that ships. Code that relies on wrapping says so with `+%`. The check is one predictable branch, and you can opt out of it explicitly.
- **`bit(n)` with no fallback type:** consistent with literals and `auto` (see 02, 03). The type is always written somewhere.
- **Run-time `bit(n)`:** the same check-then-panic rule as indexing. Before, you had to switch to `1u32 << n`, which gave no extra safety.

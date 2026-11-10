# 17 - Casts

## Rules

There are no C-style casts (`(T)x`) and no constructor-style casts (`T(x)`). Every conversion is named after **what it may lose**:

| Spelling | Allowed conversions | On loss | Safe? |
|---|---|---|---|
| `cast<T>(x)` | **Lossless only**: integer widening, `u8`/`u16`/`i8`/`i16` → `f32`, any ≤ 32-bit integer → `f64`, `f32` → `f64`, simple enum → its tag type, `bool` → integer | Compile error if any value could be lost | Yes |
| `cast::truncate<T>(x)` | Integer → narrower integer, signed ↔ unsigned | Keeps the low bits | Yes |
| `cast::saturate<T>(x)` | Integer → integer, float → integer | Clamps to the range; NaN → 0 | Yes |
| `cast::round<T>(x)` | Float → narrower float, wide integer → float | Rounds to nearest | Yes |
| `cast::checked<T>(x)` | Any numeric → numeric | Returns `std::option<T>`: `none` if the value doesn't fit exactly | Yes |
| `cast::dynamic<T>(x)` | Base/interface reference or pointer → derived | Returns `std::option<T>` | Yes |
| `cast::reinterpret<T>(x)` | Bit-level reinterpretation, integer ↔ `std::raw_ptr` | - | **`unsafe` only** |

```rift
u32 small = 300;
u64 wide  = cast<u64>(small);                     // OK: lossless
u8  a     = cast<u8>(small);                      // ERROR: may lose data
u8  b     = cast::truncate<u8>(small);            // 44
u8  c     = cast::saturate<u8>(small);            // 255
std::option<u8> d = cast::checked<u8>(small);     // std::none
i32 e     = cast::saturate<i32>(3.9);             // 3 (float → int: toward zero)
std::option<circle&> f = cast::dynamic<circle&>(some_shape);
```

Upcasts (derived → base) are implicit (see 02). Integer → `bool` is contextual only (see 02); outside conditions, write `x != 0`.

## Rationale
- **Each lossy conversion has its own name:** in C++ `static_cast<u8>(x)` hides *how* data is lost. Here a reviewer can grep for `cast::truncate` and see exactly where data is dropped, and what the behaviour is.
- **`cast<T>` lossless only:** the short, common spelling can never lose data, so the safe choice is also the easiest one to type.

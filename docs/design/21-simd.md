# 21 - SIMD (Design Sketch, Deferred)

SIMD is planned as a **language feature**, not a separate world of intrinsics. This file only records the direction; a full design will come in a later version.

## Direction
- **Vector types are built-ins:** `f32x4`, `f32x8`, `f64x4`, `i32x8`, `u8x16`, and so on. They follow the naming of the scalar types.
- **Arithmetic is element-wise:** `a + b`, `a * b`. Scalars must be broadcast explicitly with `f32x8::splat(2.0)`, consistent with "no implicit conversion" (see 02).
- **Overflow:** integer vectors follow the scalar rules, panicking on overflow by default, with `+%` for wrapping (see 16).
- **Comparisons produce mask types:** `a > b` gives `mask32x8`, not `bool`. Masks do **not** convert to `bool`, not even in conditions. Use `mask.any()` / `mask.all()`.
- **`select(mask, a, b)`** is the only per-lane conditional.
- **Reductions:** `v.reduce_add()`, `v.reduce_max()`, `v.reduce_min()`.
- **`xN` is a semantic width:** `f32x8` always has 8 lanes, mapped to whatever registers the target has.
- **Loads and stores:** the safe form is `std::span<f32>::load<f32x8>(index)`, which is bounds-checked. Unchecked or unaligned loads and raw intrinsics are in `unsafe`.

## Open questions
- Mask types: separate types or `std::bitset`-like?
- Alignment and calling convention for vector types across function calls.
- Lane widths that the target doesn't support: emulate, or compile error?

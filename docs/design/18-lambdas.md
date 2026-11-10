# 18 - Lambdas

## Rules

```
[<captures>](<parameters>) [-> <return_type>] { <body> }
```
- The capture list is **mandatory**, even when empty: `[]`.
- **Every capture is named.** There is no `[=]` or `[&]`.
- Parameter types are always written. Parameters follow the rules for functions (see 04).
- The return type is deduced from the `return` statements, or written explicitly with `-> T`.

| Capture | Meaning |
|---|---|
| `[x]` | Copy (copy types only) |
| `[move x]` | Move `x` into the lambda |
| `[&x]` | Immutable reference |
| `[mut &x]` | Mutable reference (`x` must be `mut`) |
| `[name = expr]` | Initialize a new captured value |

```rift
mut u32 counter = 0;
u32 threshold = 10;

auto inc  = [mut &counter]() { counter += 1; };
auto add  = [threshold](u32 x) { return x + threshold; };
auto init = [value = 10u32](u32 x) -> u32 { return x + value; };
auto own  = [move name](u32 x) { return std::format(f"{name}: {x}"); };
auto noop = []() { };

employees.retain([](employee& e) { return e.age >= 30; });
```

### Types and storage
- Every lambda has its own unnamed type, so lambdas are stored with `auto` or passed to template parameters.
- A lambda with **reference captures is a view type** (see 12). It can be passed down and called, but not stored in a field or returned.
- To store a callable, use `std::function<R(Args...)>`. It **always** puts the callable on the heap, and the type makes that visible.
- Lambdas with no captures convert to `std::fn_ptr<R(Args...)>`, a plain function pointer for callbacks and FFI.

## Rationale
- **`[mut &x]`:** it uses the same `mut` marker as references and call sites, so a lambda that changes a variable says so in its capture list.
- **Reference-capturing lambdas are views:** this closes the classic C++ dangling-capture bug (returning `[&]` lambdas) without any lifetime syntax.

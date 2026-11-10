# 14 - Templates

## Rules

### Declaring template parameters
Template parameters are **always declared** with `template<...>` in front of the item:
```rift
template<type T>
T larger(T a, T b) {
    return if (a > b) { a } else { b };
}

template<type T, usize N>
struct array { ... }

template<type K, type V>
class hash_map { ... }
```
| Parameter | Meaning |
|---|---|
| `type T` | Any type |
| `type T : iface` | A type that implements `iface` |
| `type T : iface_a & iface_b` | A type that implements both |
| `usize N`, `bool B`, ... | A compile-time value of that type |

### Using templates
- **Types:** template arguments are always written out: `std::darray<employee>`, `std::array<u8, 16>`.
- **Functions:** arguments are deduced from the call arguments, or written explicitly:
  ```rift
  u32 m = larger(a, b);              // T = u32 from a and b
  u32 n = larger<u32>(3, 4);         // explicit; the literals take u32
  u32 o = larger(3, 4);              // ERROR: T can't be deduced from untyped literals
  ```

### Constraints
```rift
template<type T : std::comparable>
T larger(T a, T b) { ... }
```
- **Constrained** templates are checked **when they are defined**. The body may only use what the constraints provide. At the call site the compiler checks that `T` satisfies them, and errors point at the call: "`point` does not implement `std::comparable`".
- **Unconstrained** templates are checked when they are instantiated, and the error shows the instantiation chain (at most one screen). Constraints are recommended for every public template.

### No specialization
There is no template specialization. Behaviour that depends on the type is written with `if compile_time` (see 15):
```rift
template<type T>
std::string describe(T value) {
    if compile_time (std::is_integer<T>()) {
        return std::format(f"integer {value}");
    } else {
        return std::string::from("something else");
    }
}
```

### Compilation model
1. The template **declaration** is in the `.rifth`; the **body** is in the `.rifts`.
2. Compiling the `.rifts` stores the template body in the module's compiled interface (see 09).
3. An importer that uses `larger<u32>` **instantiates and type-checks at its own compile time**. Layout, `sizeof`, `compile_time` evaluation and `deduce` return types are all available there.
4. Identical instantiations from different files are **merged at link time**. Each unique instantiation ends up in the binary only once.

## Rationale
- **Declared parameters:** in v0.2, `T` appeared from nowhere in examples. `template<type T>` is familiar from C++, and `type` is shorter than `typename`.
- **Instantiation at compile time:** v0.2 deferred instantiation to link time (ThinLTO). But a file using `std::darray<employee>` needs its size immediately to put it on the stack, and `deduce` needs the return type to type-check the caller. Only the *duplicate machine code* can wait until link time. The header/source split is still kept, because the body travels in the compiled interface.
- **Interfaces as constraints:** one concept (interface) serves both dynamic dispatch and generic constraints, so there's nothing extra like C++ "concepts" to learn.
- **`if compile_time` instead of specialization:** all variants are in one place and can be read top to bottom.

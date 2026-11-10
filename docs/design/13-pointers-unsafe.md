# 13 - Pointers and `unsafe`

## Rules

### Pointer types (all in `std::`)
| Type | Ownership | Can be null? | Copy? | Deref in safe code? |
|---|---|---|---|---|
| `std::unique_ptr<T>` | Sole owner (heap) | **No** | No (move only) | Yes |
| `std::shared_ptr<T>` | Shared owner, reference counted | **No** | No (`.clone()` increments the count) | Yes (read-only, see below) |
| `std::weak_ptr<T>` | Non-owning observer of a `shared_ptr` | - | No (`.clone()`) | Via `.lock()` → `std::option<std::shared_ptr<T>>` |
| `std::raw_ptr<T>` / `std::raw_ptr<mut T>` | None | **Yes** (`nullptr`) | Yes | **No**, only in `unsafe` |

- **`nullptr` exists only for `std::raw_ptr`.** Smart pointers are never null; a "maybe-pointer" is written `std::option<std::unique_ptr<T>>`.
- Creation always says what it costs:
  ```rift
  std::unique_ptr<circle> c = std::unique_ptr<circle>::make(circle{ .radius = 1.0 });  // heap allocation
  std::shared_ptr<config> s = std::shared_ptr<config>::make(load_config());
  std::shared_ptr<config> s2 = s.clone();                                               // count + 1
  ```
- Access: `*p` and `p->member`. A `mut std::unique_ptr<T>` allows changes to the pointee. A `std::shared_ptr<T>` gives **read-only** access to its pointee; shared mutation needs a thread-safe wrapper such as `std::shared_ptr<std::mutex<T>>` (see 22).
- Smart pointers to interfaces (`std::unique_ptr<printable>`) are fat pointers (see 07).

### Raw pointers
- **Creating, copying and comparing raw pointers is safe.** **Dereferencing them and doing pointer arithmetic is not.**
- `&x` gives `std::raw_ptr<T>`. `mut &x` gives `std::raw_ptr<mut T>` and requires `x` to be `mut`.
- In a condition context a raw pointer converts to `bool` (see 02):
```rift
mut u32 value = 5;
std::raw_ptr<mut u32> p = mut &value;

if (p) {                         // OK: contextual bool
    unsafe {
        *p = 6;                  // deref only inside unsafe
        std::raw_ptr<mut u32> q = p.offset(1);   // pointer arithmetic: unsafe
    }
}
```

### `unsafe`
`unsafe { ... }` is a **per-block** marker. It is never a file or module switch. Only these operations require it:

1. Dereferencing a `std::raw_ptr`, and pointer arithmetic (`.offset(n)`)
2. `cast::reinterpret<T>(x)` (see 17)
3. Calling an `unsafe` function, including all `extern` functions
4. Accessing a `static mut` that isn't a thread-safe type (see 03)

Functions whose callers must follow a contract are declared `unsafe`:
```rift
unsafe u32 read_register(usize address) { ... }
```

### FFI
```rift
extern "C" i32 puts(std::raw_ptr<u8> text);

unsafe {
    i32 written = puts(message.as_ptr());
}
```
- `extern "C"` functions are always `unsafe` to call.
- If a foreign function can throw a C++ exception, it must be wrapped so the exception can't cross into Rift. Letting one through aborts the program.

## Rationale
- **Non-null smart pointers:** a null check is either forgotten or redundant. `std::option` makes "maybe absent" part of the type and forces handling. `nullptr` remains only for raw pointers, where C APIs need it.
- **Pointers in `std::`:** they can be written in Rift (with `unsafe` inside), so they're library types, not keywords.
- **`mut &x`:** this is the same marker as call-site `mut` (see 04). Producing a mutable pointer looks like producing a mutable reference.
- **Read-only `shared_ptr`:** shared and mutable at once is where data races come from. Requiring a `mutex` (or similar) wrapper makes that cost visible.

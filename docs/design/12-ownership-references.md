# 12 - Ownership, Copies, Moves and References

## Rules

### Copy types vs. owning types
- **Copy types** are copied implicitly by assignment and by-value passing. These are: all built-in types, `std::raw_ptr`, enums whose payloads are copy types, and structs/classes whose fields are all copy types and that have no destructor.
- **Every other type** (anything owning memory or a resource, e.g. `std::string`, `std::darray`, `std::unique_ptr`, `std::file`) is **never copied implicitly**. You either:
  - `move x` to transfer ownership; `x` can't be used afterwards (checked at compile time), or
  - `x.clone()` to make a visible copy, which usually allocates.

```rift
std::string a = std::string::from("hi");
std::string b = a;                // ERROR: std::string is not a copy type
std::string c = a.clone();        // OK: visible allocation
std::string d = move a;           // OK: a is now unusable
std::println(a);                  // ERROR: a was moved
```

### Where moves are implicit
1. **Temporaries** are always moved: `push(employee{...})`, `std::ok(parse())`.
2. **`return x;`** of a local variable or by-value parameter.

Everything else needs `move`.

### Moving out of fields
`move` works on local variables and by-value parameters only. To take a field's value, use `std::take(mut obj.field)`, which leaves the field's default behind, or `std::replace(mut obj.field, new_value)`.

### Conditional moves
If a variable is moved in only some branches, it counts as moved after them. It can be used again only after a new assignment, which requires it to be `mut`.

### Destruction (RAII)
- Locals are destroyed at the end of their scope, in **reverse declaration order**.
- A moved-from variable is not destroyed.
- Temporaries are destroyed at the end of the full statement.
- An object's destructor body runs first, then its fields are destroyed in reverse declaration order.

### References
`T&` (immutable) and `mut T&` (mutable) never own anything, and they are **second-class**: they can't outlive what they point to, and this is enforced *without* lifetime annotations.

| References may appear as | References may **not** appear as |
|---|---|
| Function and method parameters | Struct/class fields (except in `view` types) |
| `for` loop variables, `match` bindings | Return types (except by the rule below) |
| Local variables (`employee& e = list[0];`) | Elements stored in non-`view` containers |
| Lambda captures (the lambda becomes a view) | |

**Returning a reference:** allowed only if it points into `this` (for methods) or into the **only** reference/view parameter. The compiler checks this inside the function body.
```rift
std::str trim(std::str s) { ... }                    // OK: borrows from s
T& operator[](usize i) { ... }                       // OK: borrows from this
std::str pick(std::str a, std::str b) { ... }        // ERROR: ambiguous source
```

### Aliasing rule
Within one function the compiler checks that, while a reference is alive:

- **a `mut T&` is the only way to reach that object**, and
- **a `T&` means the object is not changed**.

```rift
for (employee& e : employees) {
    employees.push(e.clone());            // ERROR: employees is borrowed by the loop
}
swap(mut a, mut a);                       // ERROR: two mutable references to a
```

### `view` types
A `view` type holds references or pointers into memory it doesn't own. `std::str`, `std::span<T>`, `std::format_args` and iterators are view types. View types follow **the same rules as references**.
```rift
view struct str {
    std::raw_ptr<u8> data;
    usize            length;
}
```
- A type with a field of a view type must itself be declared `view`.
- A template instantiated with a reference or view argument is a view type: `std::darray<std::str>` and `std::option<employee&>` can be locals but can't be returned (except by the rule above) or stored in fields.
- `std::static_str` (string literals) is **not** a view. It points to static memory and can be stored anywhere.

## Rationale
- **Nothing costs more than it looks:** in v0.2, copying an `employee` silently allocated a new `std::string`. Now every allocation is either a visible `.clone()` or a constructor/factory call.
- **Second-class references:** most real lifetime needs are "borrow for the duration of this call or loop". Restricting references to those places gives memory safety with no lifetime syntax. Full lifetime annotations can be added later if needed (see 22).
- **Local-only aliasing check:** it is per-function, so it stays fast and its error messages stay local. That keeps principle 9.

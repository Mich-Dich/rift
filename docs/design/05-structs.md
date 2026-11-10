# 05 - `struct`

A `struct` is **plain, fully visible data**. It may have methods.

## Rules

| Rule | Check |
|---|---|
| All fields and methods are public; no `public:`/`protected:`/`private:` | Syntax |
| No inheritance, no `virtual`, cannot be a base | Syntax |
| May implement interfaces (see 07) | Allowed |
| No destructor | Syntax. If it needs one, it's a `class` |
| Methods: normal, `mut`, `static`, `compile_time`, `compile_time run_time` | Allowed, no restriction |
| Fields: `T name;` or `T name = <default>;` | No `mut` on fields (see 03) |

### Construction
Only with designated fields: `T{ .field = value, ... }`.

- Fields may be listed in **any order**. They are evaluated left to right as written.
- Fields left out take their default. Leaving out a field **without** a default is an error.
- Positional construction `employee{"Ann", 30, 1.0}` is not allowed.

```rift
struct employee {
    std::string name;
    u32         age    = 0;
    f64         salary = 0.0;

    bool is_senior() {
        return this.age >= 30;
    }

    void give_raise(f64 percent) mut {
        this.salary *= 1.0 + percent / 100.0;
    }
}

mut employee e = employee{ .name = std::string::from("Ann"), .age = 41 };
e.give_raise(5.0);              // OK: e is mut and give_raise is mut
bool s = e.is_senior();         // OK: a normal runtime method

employee f = employee{ .age = 3 };            // ERROR: .name has no default
employee g = employee{ std::string::from("x"), 1, 2.0 };  // ERROR: positional
```

### Copying and comparing
- A struct is a **copy type** if all its fields are copy types (see 12). Otherwise it is moved or cloned.
- `clone()` is provided automatically if every field can be cloned.
- `==` is **opt-in**: `bool operator==(employee& other) = default;` compares field by field.

### Header / source
A struct used by other modules is declared **completely** in the `.rifth` (all fields plus method declarations). Its method bodies go in a `struct` block in the `.rifts`, which may not add fields (see 09). A struct without methods only needs the header.

## Rationale
- **Methods are no longer `compile_time`:** v0.2 required that, so methods could never run on runtime data. "Plain data" is now enforced by what matters: everything is visible, no hidden state, no inheritance, no destructor.
- **Designated-only construction:** `employee{"Ann", 30, 1.0}` breaks silently when fields are reordered. Names are explicit and survive reordering.
- **No destructor on structs:** a type that runs code on destruction owns a resource, and that is the job of a `class`.

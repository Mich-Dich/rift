# 06 - `class`

A `class` provides **encapsulation**: hidden state, invariants and resource ownership. Inheritance is covered in 07, and the header/source merge in 09.

## Rules

### Access sections
C++-style labels: `public:`, `protected:`, `private:`. Public and protected members are declared in the `.rifth`; private members exist only in the `.rifts` (see 09).

### Methods and `this`
- `this` is an implicit, immutable reference to the object. **Members are always accessed through `this.`**: write `this.value`, not `value`.
- A method that changes the object ends with **`mut`**:
  ```rift
  void inc() mut { this.value += 1; }
  ```
  Inside a `mut` method, `this` is a mutable reference.
- `mut` methods can only be called on `mut` variables or through `mut` references:
  ```rift
  counter a = counter{};
  a.inc();                 // ERROR: a is immutable
  mut counter b = counter{};
  b.inc();                 // OK
  ```
- A non-`mut` method cannot call a `mut` method on `this`.
- **`static` methods** have no `this` and are called as `counter::starting_at(5)`.

### Construction
- `T{ .field = value }` is allowed **wherever every field without a default is visible**. Outside the class's own module, private fields therefore always take their defaults.
- If the private fields have no defaults, or the class has invariants to enforce, provide `static` factory methods. Inside the class's `.rifts` all fields are visible.

### Destructor
- `~name()` runs when the object is destroyed (see 12 for when). It is implicitly `mut`.
- It is declared in the `.rifth` because whether a class has a destructor affects how it can be copied, and importers need to know that.
- A class with a destructor is never a copy type.

### Moving and copying
- Moving an object is always a **bitwise move**. There are no user-defined move constructors.
- Copy type: only if all fields are copy types and there is no destructor. Otherwise the class must implement `clone()` explicitly (interface `std::cloneable`) to be cloneable.

## Example

```rift
// counter.rifth
class counter {
public:
    u32  get();
    void inc() mut;
    static counter starting_at(u32 value);
    ~counter();
}
```

```rift
// counter.rifts
class counter {
public:
    u32 get() {
        return this.value;
    }
    void inc() mut {
        this.value += 1;
    }
    static counter starting_at(u32 value) {
        return counter{ .value = value };     // private field is visible here
    }
    ~counter() {
        std::println(f"counter dropped at {this.value}");
    }
private:
    u32 value = 0;
}
```

```rift
// main.rifts
import counter;

std::exit_code main() {
    mut counter::counter c = counter::counter::starting_at(10);
    c.inc();
    std::println(f"{c.get()}");               // 11
    return std::exit_code::success;
}                                             // destructor runs here
```

## Rationale
- **Trailing `mut`:** it works like C++'s trailing `const`, but inverted to match immutable-by-default. The marker is on the declaration, so it shows up in the header where users of the class read it.
- **Explicit `this.`:** you can always tell a member from a local or a parameter. This is also why parameters can share a name with a field (`value` vs `this.value`).
- **Bitwise moves only:** moving can never run user code or fail, so `move` costs exactly what it looks like. The trade-off is that self-referential types aren't possible (see 22).

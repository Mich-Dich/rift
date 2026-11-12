# 09 - Headers, Sources and Modules

## Rules

### Module = header + source
- `geometry.rifth` (header) + `geometry.rifts` (source) form the module **`geometry`**.
- Directories become `::` in module names: `shapes/circle.rifth` is module `shapes::circle`, with paths relative to the project root.
- A module is also a namespace. After `import geometry;` its contents are always used **fully qualified**: `geometry::circle`. There is no `using`.
- A `.rifts` automatically sees its own `.rifth`, so you don't import it.
- Inside a module, declaration order doesn't matter.

### `import`
```rift
import std;                 // the whole standard library
import std::filesystem;     // only std::filesystem
import geometry;            // ./geometry.rifth
```
Names stay fully qualified no matter which form you use: `std::filesystem::path`. The narrower import only reduces what the compiler loads.

### What goes where
| Item | `.rifth` | `.rifts` |
|---|---|---|
| Function | Declaration `T f(...);` | Definition with body |
| Struct | All fields + method declarations | `struct` block with method bodies only (no new fields) |
| Class | `public:` / `protected:` members, destructor declaration | `class` block: method bodies + `private:` members |
| Enum, interface | Complete | - |
| Template | Declaration | Body (see 14) |
| `compile_time` constant | Complete, with its value | - |
| `static` variable | Complete, with its initializer | - |
| Module-local items | - | Marked `private` |

**Headers never contain function bodies.** Headers never contain `check`s either: checks are written only in the `.rifts` (see 23). **Method bodies are only ever defined inside the `class`/`struct` block in the `.rifts`.** There are no out-of-line `void test::f() { }` definitions.

### Class merge rules
| Situation | Result |
|---|---|
| Header declares `void f();`, source defines it in the class block | ✓ Merged |
| Header declares `void f();`, source never defines it | Error (at compile time of the source) |
| Source defines a `public`/`protected` member the header doesn't declare | Error |
| Source adds `private:` methods or fields | ✓ Allowed |
| Header contains `private:` | Error |
| Source signature differs from header (including `mut`) | Error |

**Layout:** header data members in header order, then private fields in source order.

### `private` at file scope
In a `.rifts` **that has a `.rifth`**, every top-level item not declared in the header must be marked `private`; otherwise it's an error. In a standalone `.rifts` (no header) nothing can be exported, so `private` isn't needed.
```rift
// geometry.rifts
private f64 square(f64 x) { return x * x; }       // module-local helper
f64 geometry::distance(...)                        // ERROR: no out-of-line definitions
```

### Compiled module interface
The compiler turns each module into a compiled interface. Importers read **only** this file, never the `.rifts`. It contains:

- every name from the `.rifth`,
- the **complete layout** of each class (size, alignment, offsets), with private member *names* hidden,
- whether each type is a copy type and whether it has a destructor,
- template bodies and `compile_time` function bodies, in compiled form, so importers can instantiate and evaluate them (see 14, 15),
- every function's entry checks and every type-body check, so they run at the importer's calls and instantiations (see 23).

```rift
// other.rifts
import test;
test::test t = test::test{};        // OK: the size is in the interface
t.private_member_x = 5;             // ERROR: private member not visible
```

### Import graph
- The compiled interface needs the source to be compiled first, because private layout lives there. So **module imports must form an acyclic graph**, counting imports from both `.rifth` and `.rifts` files.
- Changing a private field changes the layout, which recompiles importers. Changing only a method body does not.

## Rationale
- **What the split is for:** a readable header with only the public interface, private names that can't be reached, and **no PIMPL indirection**. It is not a compile firewall. Layout changes still recompile dependents, and the doc says so openly.
- **No cycles:** this is the honest cost of putting the full layout into the interface. Cycles between modules are usually a design smell anyway.
- **One place for method bodies:** v0.2 allowed both the class block and `test::f()`. Now there is one way.
- **Required `private`:** mirrors the class rule that every public member must be in the header. You can't accidentally export something.

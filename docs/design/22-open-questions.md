# 22 - Open Questions

Ordered roughly by how much they affect the rest of the design.

| # | Topic | Current state | Notes |
|---|---|---|---|
| 1 | **Lifetimes beyond second-class references** | Reference elision rule only (see 12) | Is "borrow from `this` or the only parameter" enough? If not: explicit lifetime parameters, or more `std::shared_ptr` use |
| 2 | **Concurrency** | Not designed | Threads, `std::atomic`, `std::mutex<T>`. Which types may cross threads (like Rust's `Send`/`Sync`)? Needed to finish the `static mut` and `shared_ptr` rules |
| 3 | **`shared_ptr` reference count** | Unspecified | Always atomic (simple, small cost), or a separate `std::local_shared_ptr`? |
| 4 | **Consuming methods** | Not available | A trailing `move` (like trailing `mut`) for methods that consume `this`, e.g. `std::string into_upper() move` |
| 5 | **Panic strategy** | Abort | Configurable panic handler / trap for embedded and safety-critical targets |
| 6 | **Interface default methods** | Not allowed | Useful, but adds rules about which implementation wins |
| 7 | **Variadic templates** | Not available | Needed for `std::function<R(Args...)>`, which currently relies on compiler magic |
| 8 | **Tuples** | Removed | Structs are the one way. Revisit if multi-value returns get too verbose |
| 9 | **Self-referential types** | Impossible (bitwise moves) | A "pinned" concept, if ever needed |
| 10 | **Allocators** | Global allocator only | Per-container allocators (`std::darray<T, A>`) matter for embedded use |
| 11 | **Compile-time heap** | Allowed, stored in static memory (see 15) | Exact rules for which types support this |
| 12 | **Module aliases** | Not available | `import std::filesystem as fs;` would be short *and* explicit. Does it break "always fully qualified"? |
| 13 | **Attributes** | Not designed | v0.2 *Future thoughts* used `[required]`, which clashes with lambda syntax `[...]`. Proposal: `@required` |
| 14 | **Types as values / reflection** | Not designed | `.type = std::filesystem::path` from *Future thoughts* needs it |
| 15 | **Language as build system** | Idea | From *Future thoughts*. A `build.rifts` evaluated with `compile_time`? |
| 16 | **SIMD** | Sketch (see 21) | |
| 17 | **Name** | `Rift` (working name) | |

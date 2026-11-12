# 22 - Open Questions

Ordered roughly by how much they affect the rest of the design.

| # | Topic | Current state | Notes |
|---|---|---|---|
| 1 | **Lifetimes beyond second-class references** | Reference elision rule only (see 12) | Is "borrow from `this` or the only parameter" enough? If not: explicit lifetime parameters, or more `std::shared_ptr` use |
| 2 | **Concurrency** | Not designed | Threads, `std::atomic`, `std::mutex<T>`. Which types may cross threads (like Rust's `Send`/`Sync`)? Needed to finish the `static mut` and `shared_ptr` rules |
| 3 | **`shared_ptr` reference count** | Unspecified | Always atomic (simple, small cost), or a separate `std::local_shared_ptr`? |
| 4 | **Consuming methods** | Not available | A trailing `move` (like trailing `mut`) for methods that consume `this`, e.g. `std::string into_upper() move` |
| 5 | **Panic strategy** | Abort, or trap via build mode (see 24) | A user-defined panic handler for embedded and safety-critical targets |
| 6 | **Interface default methods** | Not allowed | Useful, but adds rules about which implementation wins |
| 7 | **Variadic templates** | Not available | Needed for `std::function<R(Args...)>`, which currently relies on compiler magic |
| 8 | **Tuples** | Removed | Structs are the one way. Revisit if multi-value returns get too verbose |
| 9 | **Self-referential types** | Impossible (bitwise moves) | A "pinned" concept, if ever needed |
| 10 | **Allocators** | Global allocator only | Per-container allocators (`std::darray<T, A>`) matter for embedded use |
| 11 | **Compile-time heap** | Allowed, stored in static memory (see 15) | Exact rules for which types support this |
| 12 | **Module aliases** | Not available | `import std::filesystem as fs;` would be short *and* explicit. Does it break "always fully qualified"? |
| 13 | **Attributes** | Not designed | v0.2 *Future thoughts* used `[required]`, which clashes with lambda syntax `[...]`. Proposal: `@required` |
| 14 | **Types as values / reflection** | Not designed | `.type = std::filesystem::path` from *Future thoughts* needs it |
| 15 | **Language as build system** | **Designed** (see 25) | |
| 16 | **SIMD** | Sketch (see 21) | |
| 18 | **`compile_time` parameters** | Covered by `compile_time check` entry checks (see 23) | A parameter modifier would only be needed if the *body* must use the value at compile time (e.g. to pick a type) |
| 19 | **In-source warning control** | Command line only (`--warn=id=off`) | Needs attributes (#13), e.g. `@allow("gfx::deprecated")` on a statement or function |
| 20 | **Fix-it suggestions** | Not available | `std::compile_fixit(location, replacement)` so tools can apply the author's suggested fix |
| 21 | **Postconditions** | Entry checks are preconditions (see 23); debug-only checks are `run_time(debug) check` | Checks on return values, e.g. a `check` that sees the function's result? |
| 22 | **Implicit `std::static_str` → `std::filesystem::path`** | Used in 19 and 25 (`std::file::open("x")`, `.root = "core"`) | Not in 02's list of implicit conversions. Add it there, or require `std::filesystem::path::from(...)`? |
| 23 | **List literals** | Not available | `[a, b, c]` would shorten build scripts and container setup. Clashes with lambda captures `[x](...)` unless resolved by the token after `]` |
| 24 | **C/C++ headers → Rift headers** | Hand-written `extern "C"` `.rifth` | A build step that generates `.rifth` from C headers (like `bindgen`). C++ classes are much harder than C |
| 25 | **Build sandboxing and remote cache** | Steps declare inputs/outputs (see 25) | Enforce declarations with a sandbox; share build outputs between machines and CI |
| 26 | **Package registry** | Git and path dependencies only | A central registry with versions instead of commits? |
| 17 | **Name** | `Rift` (working name) | |

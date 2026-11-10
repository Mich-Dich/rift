# 20 - Standard Library Overview

## Rules

### Naming
- Always `std::`-qualified. There is no `using`.
- `snake_case`, with **full words over abbreviations**: `std::error`, not `std::err`. Established short forms are kept where they are clearer than the long name: `darray`, `str`, `ptr`, `io`.
- Names say what something *is*. `std::list` was removed because in C++ it means a linked list.
- **The boundary test:** if a feature can be written as an ordinary Rift function or type, it belongs in `std::`, not in the language.

### Catalogue (v0.3)
| Area | Names |
|---|---|
| Containers | `std::array<T, N>` (fixed size, inline), `std::darray<T>` (growable, heap), `std::span<T>` / `std::span<mut T>` (view of contiguous elements), `std::hash_map<K, V>`, `std::tree_map<K, V>`, `std::bitset<N>` |
| Strings | `std::string`, `std::str`, `std::static_str`, `std::format`, `std::format_args` |
| Results | `std::result<T, E>`, `std::option<T>`, `std::ok`, `std::error`, `std::some`, `std::none` |
| Pointers | `std::unique_ptr<T>`, `std::shared_ptr<T>`, `std::weak_ptr<T>`, `std::raw_ptr<T>` |
| Callables | `std::function<R(Args...)>`, `std::fn_ptr<R(Args...)>` |
| Ownership | `std::take`, `std::replace`, `std::swap` |
| Arithmetic | `std::checked_add/sub/mul`, `std::saturating_add/sub/mul`, `std::ordering` |
| Concurrency | `std::atomic<T>`, `std::mutex<T>` (design pending, see 22) |
| I/O | `std::println`, `std::eprintln`, `std::file`, `std::io_error` |
| Filesystem | `std::filesystem::path`, `std::filesystem::executable_dir` |
| Program | `std::exit_code` (`success`, `failure`), `std::env::raw_args`, `std::panic` |
| Interfaces | `std::comparable`, `std::cloneable`, `std::formattable` |
| Compile time | `std::is_integer`, `std::is_float`, `std::is_same`, `std::implements`, `std::size_of`, `std::align_of`, `std::compile_error` |
| Sorting | `std::sort_order::ascending`, `std::sort_order::descending` |

### Container access
- `a[i]` panics when out of bounds. `a.get(i)` returns `std::option<T&>`.
- `std::darray<T>` methods include `push(T value) mut`, `pop() mut → std::option<T>`, `len() → usize`, `retain(pred) mut`, `sort_by_key(order, key) mut`.
- An empty container is created with `default` or `T{}`: `std::darray<employee> list = default;`.

### Removed from v0.2
| Removed | Replacement |
|---|---|
| `std::err` | `std::error` |
| `std::list`, `std::d_array` | `std::darray` |
| `std::variant` | Enums with payloads (see 08) |
| `std::box`, `std::dyn` | `std::unique_ptr`, interface pointers (see 07) |
| `std::range` | `a..b` / `a..=b` (see 10) |
| `std::convert` | `.or_error()` / `.map_error()` (see 11) |
| `std::filesystem::get_executable_path` | `std::filesystem::executable_dir` |

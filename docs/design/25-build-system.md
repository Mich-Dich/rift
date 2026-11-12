# 25 - Rift as a Build System

A project is built by **`build.rifts`**, a Rift file at the project root. It describes targets (programs, libraries, tests), build modes, options, dependencies and custom steps. It can build **Rift and C/C++** code, so a mixed code base needs no CMake.

## Rules

### How `build.rifts` runs
- `riftc` evaluates `build.rifts` with its **compile-time interpreter** (see 15). It isn't compiled into a program and never runs as one.
- It defines exactly one function:
  ```rift
  compile_time void build(mut std::build::graph& g) { ... }
  ```
  The function adds things to `g`. The result is a **build graph**, which `riftc` then executes, in parallel and incrementally.
- The full language is available: variables, loops, `if`, `match`, helper functions, structs, `import`s of other build modules (see "Reuse" below).
- **The build script is deterministic.** Like all compile-time code it has no I/O of its own. Its only view of the outside world is through `g`, and **every outside value it reads is recorded as an input**:

| Read through `g` | Recorded as input |
|---|---|
| `g.glob("src/**/*.rifts")` | The matching file list |
| `g.option<T>(name, default, help)` | The option value |
| `g.env("VULKAN_SDK")` | The variable's value |
| `g.find_program("slangc")` | The program's path and hash |
| `g.host()`, `g.target()` | Host and target platform |

  If none of the recorded inputs changed, `riftc` reuses the previous graph without evaluating `build.rifts` again.

### Command line
```
riftc build [--mode=<mode>] [--target=<platform>] [-D<option>=<value>] [<target>...]
riftc run   [--mode=<mode>] <target> [-- <program args>]
riftc test  [--mode=<mode>] [<test>...]
riftc build --list            # targets, modes and options with their help text
riftc build --compile-commands  # writes compile_commands.json for IDEs (clangd for the C/C++ parts)
```
- Without `build.rifts`, `riftc build main.rifts` builds that one file as a program in `debug` mode.
- Build output goes to `build/<mode>/` (`build/<mode>/<platform>/` when cross-compiling).

### Targets
| Method | Builds |
|---|---|
| `g.add_executable(std::build::rift_target{...})` | A Rift program (module with `main`) |
| `g.add_library(std::build::rift_target{...})` | A Rift library (one or more modules) |
| `g.add_c_library(std::build::c_target{...})` | A static library from C or C++ |
| `g.add_test(std::build::rift_target{...})` | A test program, run by `riftc test` |

Each method returns a **target handle** (`std::build::target`), a small copy type used to connect targets:
```rift
std::build::target core = g.add_library(std::build::rift_target{ .name = "core", .root = "core" });
std::build::target app  = g.add_executable(std::build::rift_target{ .name = "app", .root = "app" });
g.link(app, core);              // app imports core's modules and links it
```

Settings that are lists are added with methods, never with list literals:
```rift
std::build::target vma = g.add_c_library(std::build::c_target{
    .name     = "vma",
    .language = std::build::language::cpp,
    .standard = std::build::cpp_standard::cpp20,
});
g.add_sources(vma, g.glob("vendor/VulkanMemoryAllocator/src/*.cpp"));
g.add_include_dir(vma, "vendor/VulkanMemoryAllocator/include", std::build::visibility::public);
g.define(vma, "VMA_STATIC_VULKAN_FUNCTIONS", "0");
```
- `visibility::public` means "also applies to everything that links this target", like CMake's `PUBLIC`. `private` is the default.
- **Rift calling C/C++:** a C library target can come with a Rift header (`.rifth`) of `extern "C"` declarations (see 13). Generating those headers from C headers is an open question (see 22).

### Modes and options
```rift
g.add_mode(std::build::mode{ .name = "profile", .base = std::build::modes::release_with_debug_info });

bool validation = g.option<bool>("vulkan_validation", std::build::mode_is(std::build::modes::debug),
                                 "Enable the Vulkan validation layers");
g.set(app, "vulkan_validation", validation);           // visible to app's code

g.set_mode(vma, std::build::modes::release);           // always optimize this dependency
```
- Build modes are described in 24.
- **Options** are values given on the command line (`-Dvulkan_validation=true`), with a default and a help text.
- **Settings** pass compile-time values from the build into a target's code:
  ```rift
  // in app's code
  compile_time bool validation = std::build::setting<bool>("vulkan_validation");
  ```
  Reading a setting the build didn't `set` for this target is a compile error, so there are no silently missing `#define`s.

### Custom steps: tools and generated files
A step runs a program with **declared** inputs and outputs. The program is either a target of the same build (a Rift tool) or an external program:
```rift
std::build::program slangc = g.find_program("slangc");
for (std::build::path shader : g.glob("shaders/*.slang")) {
    std::build::path spirv = g.output_path(shader.with_extension("spv"));
    std::build::step s = g.add_step(std::build::step_spec{ .name = "compile shader", .program = slangc });
    g.add_argument(s, shader);
    g.add_argument(s, "-o");
    g.add_argument(s, spirv);
    g.add_input(s, shader);
    g.add_output(s, spirv);
    g.depends_on(app, s);
}
```
- A step only runs when an input changed (or its outputs are missing).
- After a step runs, every declared output must exist; otherwise the build fails. Writing files that aren't declared is an error once sandboxing exists (see 22).
- Steps run in parallel whenever the graph allows it.

### Dependencies
```rift
std::build::project glm = g.dependency(std::build::git{
    .url    = "https://github.com/g-truc/glm",
    .commit = "33b0eb9fa336ffd8551024b1d2690e418014553b",
});
```
- Dependencies are a path or a git commit, **always pinned**. A branch name isn't accepted.
- A dependency with its own `build.rifts` contributes its targets (`glm.target("glm")`). A dependency without one is described in our own `build.rifts` as a C/C++ target.
- `riftc` writes `build.lock` with each dependency's content hash. The lock file belongs in version control.

### Reuse
Build logic can live in ordinary modules and be imported:
```rift
// build.rifts
import build_support::vulkan;          // build_support/vulkan.rifts

compile_time void build(mut std::build::graph& g) {
    std::build::target vulkan = build_support::vulkan::add(mut g);
    ...
}
```
Imported modules follow the same rules: `compile_time` only, with the outside world reachable only through `g`.

## Example: an engine with C++ dependencies and shaders
```rift
// build.rifts
import std::build;

compile_time void build(mut std::build::graph& g) {
    g.add_mode(std::build::mode{
        .name = "profile",
        .base = std::build::modes::release_with_debug_info,
        .link_time_optimization = true,
    });

    bool validation = g.option<bool>("vulkan_validation",
        std::build::mode_is(std::build::modes::debug),
        "Enable the Vulkan validation layers");

    // --- C/C++ dependencies -----------------------------------------
    std::build::target imgui = g.add_c_library(std::build::c_target{
        .name = "imgui",
        .language = std::build::language::cpp,
        .standard = std::build::cpp_standard::cpp20,
    });
    g.add_sources(imgui, g.glob("vendor/imgui/*.cpp"));
    g.add_include_dir(imgui, "vendor/imgui", std::build::visibility::public);
    g.set_mode(imgui, std::build::modes::release);

    std::build::target vulkan = g.add_system_library(std::build::system_library{
        .name = "vulkan",
        .root = g.env("VULKAN_SDK"),
    });

    // --- Rift code --------------------------------------------------
    std::build::target core = g.add_library(std::build::rift_target{ .name = "core", .root = "core" });

    std::build::target renderer = g.add_library(std::build::rift_target{ .name = "renderer", .root = "renderer" });
    g.link(renderer, core);
    g.link(renderer, imgui);
    g.link(renderer, vulkan);
    g.set(renderer, "vulkan_validation", validation);

    std::build::target app = g.add_executable(std::build::rift_target{ .name = "engine", .root = "app" });
    g.link(app, renderer);

    // --- Generated files --------------------------------------------
    std::build::program slangc = g.find_program("slangc");
    for (std::build::path shader : g.glob("shaders/*.slang")) {
        std::build::path spirv = g.output_path(shader.with_extension("spv"));
        std::build::step s = g.add_step(std::build::step_spec{ .name = "slang", .program = slangc });
        g.add_argument(s, shader);
        g.add_argument(s, "-o");
        g.add_argument(s, spirv);
        g.add_input(s, shader);
        g.add_output(s, spirv);
        g.depends_on(app, s);
    }

    // --- Tests ------------------------------------------------------
    for (std::build::path test : g.glob("tests/*.rifts")) {
        std::build::target t = g.add_test(std::build::rift_target{ .name = test.stem(), .root = test });
        g.link(t, core);
    }
}
```
```
riftc build                         # debug, validation on
riftc build --mode=profile          # profile, validation off
riftc run engine -Dvulkan_validation=true --mode=release
riftc test
```

## Rationale
- **One language:** build logic uses the same syntax, types and error messages as the code it builds. There is no second language like CMake's to learn.
- **Evaluated at compile time:** the build script can't do I/O except through recorded inputs, so builds are reproducible and the graph can be cached reliably. Anything that needs real I/O (code generators, shader compilers) is a step with declared inputs and outputs, which keeps incremental builds correct.
- **A graph, not a script that does the building:** evaluating `build.rifts` only *describes* the work. `riftc` decides the order, the parallelism and what is up to date.
- **Methods instead of list literals:** the build API works with the language as it is today. If list literals are added later (see 22), the API can accept them as well.
- **Pinned dependencies and `build.lock`:** a build from the same commit always uses the same dependency sources.
- **Settings instead of `#define`s:** values from the build reach the code as typed `compile_time` constants, and a missing one is an error instead of an empty macro.

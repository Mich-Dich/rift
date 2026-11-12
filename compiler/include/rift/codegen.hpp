#pragma once
// Code generation (M6): a checked program becomes one C translation unit, which a C compiler
// (GCC or Clang) turns into a native executable.
//
// Only functions reachable from `main` are generated. Constructs the backend does not handle
// yet are reported as E6001 at the construct, so unused code may still use them.
//
// The generated C keeps Rift's semantics: integer overflow, division by zero and bad shifts
// panic (spec 16), evaluation order is left to right, and a panic prints the Rift source
// location (spec 11). The runtime it relies on is `c_runtime` (src/runtime.cpp).

#include "rift/diagnostics.hpp"
#include "rift/source.hpp"
#include "rift/symbols.hpp"

#include <string>
#include <string_view>

namespace rift {

/// The C runtime prepended to every generated program.
extern const std::string_view c_runtime;

struct CodegenOptions {
    /// Generate every non-template function, not only those reachable from `main` (a missing
    /// `main` is then allowed). For testing the backend on code that is never run.
    bool all_functions = false;
};

/// Translates `program` (resolved, type checked and ownership checked) to C. The result is
/// only meaningful when no error was added to `diags`.
std::string generate_c(sema::Program& program, const SourceManager& sources, DiagnosticEngine& diags,
                       CodegenOptions options = {});

} // namespace rift

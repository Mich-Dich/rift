#pragma once
// LLVM backend. A checked program becomes LLVM IR, which LLVM compiles to an object file for the
// host; the system linker turns that into an executable. No C code is generated.
//
// Only this backend's translation units include LLVM headers. Everything in this header is plain
// C++, so the front end, the driver and the unit tests do not depend on LLVM.
//
// Layout rules for C/C++ interop (see claude/compiler_status.md, "Interop layout"): Rift types
// that have a C counterpart use the same size, alignment and field order as the C compiler's
// layout for the matching C type, and references and raw pointers are plain LLVM `ptr`.

#include "rift/diagnostics.hpp"
#include "rift/source.hpp"
#include "rift/symbols.hpp"

#include <string>
#include <string_view>

namespace rift {

struct LlvmOptions {
    /// Generate every non-template function in the entry module, not only those reachable from
    /// `main`. For testing the backend on code that is never run.
    bool all_functions = false;
};

/// Lowers the checked program to textual LLVM IR (LLVM 18 syntax). Returns an empty string when
/// errors were added to `diags`.
std::string generate_llvm_ir(sema::Program& program, const SourceManager& sources, DiagnosticEngine& diags,
                             LlvmOptions options = {});

/// Compiles LLVM IR text to an object file for the host and links an executable at `out_path`
/// with `linker` (for example "cc"). Returns false and sets `error` on failure.
bool build_executable(std::string_view ir, const std::string& out_path, const std::string& linker, std::string& error);

/// The runtime every generated module is linked with, as LLVM IR text (llvm_runtime.cpp).
std::string llvm_runtime_ir();

} // namespace rift

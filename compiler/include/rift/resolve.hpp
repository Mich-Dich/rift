#pragma once
// Name resolution (M3): builds every module's symbol table, merges headers with sources
// (spec 09), checks class hierarchies (spec 07) and resolves every name in every body
// (spec 03, 06, 10, 12, 18). Resolved symbols are stored in the AST (`symbol` fields).

#include "rift/diagnostics.hpp"
#include "rift/symbols.hpp"

#include <iosfwd>

namespace rift {

void resolve_program(sema::Program& program, DiagnosticEngine& diags);

/// `riftc symbols`: the merged module interface, members in layout order.
void print_symbols(std::ostream& out, const sema::Module& module, const SourceManager& sources);

} // namespace rift

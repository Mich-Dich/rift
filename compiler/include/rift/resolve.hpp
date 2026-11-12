#pragma once
// Name resolution (M3): builds every module's symbol table, merges headers with sources
// (spec 09), checks class hierarchies (spec 07) and resolves every name in every body
// (spec 03, 06, 10, 12, 18). Resolved symbols are stored in the AST (`symbol` fields).

#include "rift/diagnostics.hpp"
#include "rift/symbols.hpp"

#include <iosfwd>

namespace rift {

void resolve_program(sema::Program& program, DiagnosticEngine& diags);

/// Resolves an instance of the template type `templ` (spec 14): `decl` is a fresh copy of the
/// template's declaration, which must outlive the program. Returns the instance's symbol (not
/// registered in the module); `params` receives the symbols of its template parameters.
sema::Symbol* resolve_instance(sema::Program& program, DiagnosticEngine& diags, const sema::Symbol* templ,
                               ast::Decl& decl, std::vector<sema::Symbol*>& params);

/// Resolves an instance of the function template `templ` (spec 14): `decl` is a fresh copy of the
/// template's function, signature and body included. Same contract as resolve_instance.
sema::Symbol* resolve_function_instance(sema::Program& program, DiagnosticEngine& diags, const sema::Symbol* templ,
                                        ast::FunctionDecl& decl, std::vector<sema::Symbol*>& params);

/// `riftc symbols`: the merged module interface, members in layout order.
void print_symbols(std::ostream& out, const sema::Module& module, const SourceManager& sources);

} // namespace rift

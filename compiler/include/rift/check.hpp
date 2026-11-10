#pragma once
// Type checking (M4): gives every expression a type (stored in Expr::type) and enforces the
// rules of spec 02 (literal typing, no implicit conversions, contextual bool), 03/04/06
// (mutability, call-site `mut`, `mut` methods), 10 (if/match types, exhaustiveness),
// 13 (unsafe), 15 (compile_time calls), 16 (operators, shifts, bit(n)) and 17 (casts).
//
// The standard library does not exist yet: `std::` types are "external" and conversions
// between them, their members and their functions are not checked. Template bodies are
// checked with their parameters treated as unknown types.

#include "rift/diagnostics.hpp"
#include "rift/symbols.hpp"

namespace rift {

void check_types(sema::Program& program, DiagnosticEngine& diags);

} // namespace rift

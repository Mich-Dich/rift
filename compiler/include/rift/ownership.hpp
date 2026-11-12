#pragma once
// Ownership and references (M5, spec 12): copy vs. owning types, use after `move` (with a
// flow analysis over branches and loops), second-class references (no reference fields, the
// return rule), the aliasing rule for `mut` arguments, and containers borrowed by loops or
// by local references.
//
// Runs after type checking and only reasons about types it knows; standard-library types
// with an unknown shape are not checked.

#include "rift/diagnostics.hpp"
#include "rift/symbols.hpp"

namespace rift {

void check_ownership(sema::Program& program, DiagnosticEngine& diags);

} // namespace rift

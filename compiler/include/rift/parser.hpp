#pragma once
// Parser: tokens -> AST (spec 01-19 syntax).
//
// Recursive descent for declarations and statements, precedence climbing for expressions
// (spec 16). On a syntax error the parser reports one diagnostic, skips to a synchronization
// point (end of statement / declaration / closing delimiter) and continues, so one run reports
// independent errors without cascades.

#include "rift/ast.hpp"
#include "rift/diagnostics.hpp"
#include "rift/token.hpp"

#include <vector>

namespace rift {

/// Parses a whole file. `tokens` must end with eof (as produced by tokenize()).
ast::File parse(const SourceFile& file, FileId id, const std::vector<Token>& tokens,
                DiagnosticEngine& diags);

/// Parses a single expression (unit tests). Reports an error if tokens remain.
ast::ExprPtr parse_expression(const SourceFile& file, FileId id, const std::vector<Token>& tokens,
                              DiagnosticEngine& diags);

} // namespace rift

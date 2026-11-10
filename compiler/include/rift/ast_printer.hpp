#pragma once
// Prints the AST as indented S-expressions (`riftc parse`). Short subtrees stay on one line:
//
//   (fn main -> u32
//     (block
//       (var count : u32 mut (int 0))
//       (return (path count))))

#include "rift/ast.hpp"

#include <iosfwd>
#include <string>

namespace rift::ast {

/// `with_types` appends each expression's type (after type checking): `(int 1 : u32)`.
void print(std::ostream& out, const File& file, bool with_types = false);

/// Source-like spelling of a type: `std::darray<employee>`, `mut u32&`.
std::string to_string(const TypeExpr& type);
std::string to_string(const Path& path);

/// One expression as a single-line S-expression (used in tests).
std::string to_sexpr(const Expr& expr);

} // namespace rift::ast

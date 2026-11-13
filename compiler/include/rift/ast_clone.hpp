#pragma once
// Deep copies of resolved syntax trees (spec 26, `for compile_time`).
//
// A `for compile_time` loop type-checks its body once per element, and every copy needs its own
// nodes (the checker stores types in them). The copy keeps the resolved symbols, except that
// every local the copied code declares gets a fresh symbol, and the code inside the copy refers
// to those. Types and compile-time values are not copied: the checker fills them in again.

#include "rift/ast.hpp"

#include <functional>
#include <map>

namespace rift::ast {

class Cloner {
public:
    /// Makes the fresh symbol for a local declared inside the copied code.
    using SymbolMaker = std::function<sema::Symbol*(const sema::Symbol& original)>;

    explicit Cloner(SymbolMaker make) : make_(std::move(make)) {}

    /// Uses `to` wherever the copied code refers to `from` (e.g. the loop variable).
    void map(const sema::Symbol* from, sema::Symbol* to) { map_[from] = to; }

    BlockPtr block(const BlockExpr& b);
    ExprPtr  expr(const Expr& e);
    TypePtr  type(const TypeExpr& t);

private:
    SymbolRef ref(SymbolRef s) const;
    SymbolRef fresh(SymbolRef s);
    Path      path(const Path& p);
    StmtPtr   stmt(const Stmt& s);
    PatternPtr pattern(const Pattern& p);
    template <typename T> std::unique_ptr<T> opt(const std::unique_ptr<T>& p);

    SymbolMaker                                      make_;
    std::map<const sema::Symbol*, sema::Symbol*>     map_;
};

} // namespace rift::ast

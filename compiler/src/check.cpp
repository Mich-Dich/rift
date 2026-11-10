#include "rift/check.hpp"

#include "rift/ast_printer.hpp"
#include "rift/types.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <set>

namespace rift {

using namespace ast;
using sema::Module;
using sema::Symbol;
using sema::SymbolKind;
using types::Kind;
using types::Type;

namespace {

std::string quoted(const Type* t) { return "'" + types::to_string(t) + "'"; }

/// cast<T> conversions that never lose information (spec 17).
bool lossless(const Type* from, const Type* to) {
    if (from == to) return true;
    if (from->is(Kind::bool_) && to->is_integer()) return true;
    if (from->is_integer() && to->is_integer()) {
        auto min_bits = [](const Type* t) { return t->is_size ? 32 : t->bits; };   // size types: 32..64
        auto max_bits = [](const Type* t) { return t->is_size ? 64 : t->bits; };
        if (from->is_signed == to->is_signed) return max_bits(from) <= min_bits(to);
        if (!from->is_signed && to->is_signed) return max_bits(from) < min_bits(to);
        return false;   // signed -> unsigned can lose the sign
    }
    if (from->is_integer() && to->is_float()) {
        int bits = from->is_size ? 64 : from->bits;
        return to->bits == 32 ? bits <= 16 : bits <= 32;
    }
    if (from->is_float() && to->is_float()) return from->bits <= to->bits;
    if (from->is_user(SymbolKind::enum_) && to->is_integer()) {
        // A simple enum converts to its tag type (checked by the caller against the tag).
        return true;
    }
    return false;
}

bool is_comparison(TokenKind op) {
    return op == TokenKind::less || op == TokenKind::less_eq || op == TokenKind::greater ||
           op == TokenKind::greater_eq;
}

bool is_arithmetic(TokenKind op) {
    return op == TokenKind::plus || op == TokenKind::minus || op == TokenKind::star ||
           op == TokenKind::slash || op == TokenKind::percent;
}

bool is_wrapping(TokenKind op) {
    return op == TokenKind::plus_percent || op == TokenKind::minus_percent || op == TokenKind::star_percent;
}

bool is_bitwise(TokenKind op) {
    return op == TokenKind::amp || op == TokenKind::pipe || op == TokenKind::caret;
}

bool is_shift(TokenKind op) { return op == TokenKind::shl || op == TokenKind::shr; }

/// The binary operator a compound assignment applies: `+=` -> `+`.
TokenKind compound_base(TokenKind op) {
    switch (op) {
    case TokenKind::plus_eq: return TokenKind::plus;
    case TokenKind::minus_eq: return TokenKind::minus;
    case TokenKind::star_eq: return TokenKind::star;
    case TokenKind::slash_eq: return TokenKind::slash;
    case TokenKind::percent_eq: return TokenKind::percent;
    case TokenKind::amp_eq: return TokenKind::amp;
    case TokenKind::pipe_eq: return TokenKind::pipe;
    case TokenKind::caret_eq: return TokenKind::caret;
    case TokenKind::shl_eq: return TokenKind::shl;
    case TokenKind::shr_eq: return TokenKind::shr;
    case TokenKind::plus_percent_eq: return TokenKind::plus_percent;
    case TokenKind::minus_percent_eq: return TokenKind::minus_percent;
    case TokenKind::star_percent_eq: return TokenKind::star_percent;
    default: return TokenKind::eq;
    }
}

/// Does evaluating this expression/statement always leave the function (return, panic, an
/// endless loop)? Used for "may end without returning" and for `never` block types.
bool diverges(const Expr& e);

bool diverges_block(const BlockExpr& b) {
    for (const StmtPtr& s : b.statements) {
        switch (s->kind) {
        case StmtKind::expression:
            if (diverges(*s->as<ExprStmt>().expr)) return true;
            break;
        case StmtKind::variable:
            if (s->as<VariableStmt>().init && diverges(*s->as<VariableStmt>().init)) return true;
            break;
        case StmtKind::discard:
            if (diverges(*s->as<DiscardStmt>().value)) return true;
            break;
        case StmtKind::assign: break;
        }
    }
    return b.tail && diverges(*b.tail);
}

bool contains_break(const BlockExpr& b) {
    bool found = false;
    std::function<void(const Expr&)> walk = [&](const Expr& e) {
        switch (e.kind) {
        case ExprKind::break_: found = true; break;
        case ExprKind::block:
            for (const StmtPtr& s : e.as<BlockExpr>().statements) {
                if (s->kind == StmtKind::expression) walk(*s->as<ExprStmt>().expr);
            }
            if (e.as<BlockExpr>().tail) walk(*e.as<BlockExpr>().tail);
            break;
        case ExprKind::if_:
            walk(*e.as<IfExpr>().then_block);
            if (e.as<IfExpr>().else_branch) walk(*e.as<IfExpr>().else_branch);
            break;
        case ExprKind::match:
            for (const MatchArm& a : e.as<MatchExpr>().arms) walk(*a.body);
            break;
        default: break;   // nested loops own their breaks; lambdas cannot break out
        }
    };
    walk(b);
    return found;
}

bool diverges(const Expr& e) {
    switch (e.kind) {
    case ExprKind::return_:
    case ExprKind::break_:
    case ExprKind::continue_: return true;
    case ExprKind::block: return diverges_block(e.as<BlockExpr>());
    case ExprKind::if_: {
        const auto& i = e.as<IfExpr>();
        return i.else_branch && diverges_block(*i.then_block) && diverges(*i.else_branch);
    }
    case ExprKind::match: {
        const auto& m = e.as<MatchExpr>();
        if (diverges(*m.scrutinee)) return true;
        return !m.arms.empty() && std::all_of(m.arms.begin(), m.arms.end(), [](const MatchArm& a) { return diverges(*a.body); });
    }
    case ExprKind::while_: {
        const auto& w = e.as<WhileExpr>();
        bool always = w.condition->kind == ExprKind::bool_literal && w.condition->as<LiteralExpr>().bool_value;
        return always && !contains_break(*w.body);
    }
    case ExprKind::call:
        return e.type && e.type->is(Kind::never);
    default: return e.type && e.type->is(Kind::never);
    }
}

struct FunctionState {
    const Type*         result      = nullptr;   // null: unknown (lambda without '->')
    bool                in_function = false;
    Symbol*             self        = nullptr;
    bool                has_this    = false;
    bool                this_mut    = false;
    bool                is_compile_time = false;
    std::string         name;
};

/// Where an expression's value lives, for assignment and `mut` checks.
struct Place {
    bool        is_place   = false;
    bool        is_mutable = false;
    bool        via_this   = false;
    bool        is_temporary = false;
    std::string name;
    const Symbol* local = nullptr;
};

class Checker {
public:
    Checker(sema::Program& program, DiagnosticEngine& diags)
        : program_(program), diags_(diags), tc_(*program.types) {}

    void run() {
        for (auto& [name, m] : program_.modules) {
            module_ = m.get();
            if (m->header) check_file(m->header->ast);
            if (m->source) check_file(m->source->ast);
        }
        if (program_.entry) check_main(*program_.entry);
    }

private:
    // --- diagnostics ----------------------------------------------------------------------

    Diagnostic& error(DiagCode code, Span span, std::string message, std::string label = {}) {
        return diags_.report(code, span, std::move(message), std::move(label));
    }
    static void note(Diagnostic& d, std::string message, std::optional<Span> span = std::nullopt) {
        d.notes.push_back(Note{std::move(message), span});
    }

    // --- lowering written types -----------------------------------------------------------

    const Type* lower(const TypeExpr& t, const std::map<const Symbol*, const Type*>* subst = nullptr) {
        switch (t.kind) {
        case TypeKind::auto_:
        case TypeKind::deduce:
        case TypeKind::function: return tc_.error();
        case TypeKind::reference: {
            const auto& r = t.as<ReferenceType>();
            return tc_.reference(lower(*r.inner, subst), r.is_mut);
        }
        case TypeKind::named: break;
        }
        const auto& nt = t.as<NamedType>();
        const auto& segs = nt.path.segments;
        if (segs.size() == 1 && !nt.symbol) {
            if (const Type* b = tc_.builtin(segs[0].name)) return b;
        }
        if (nt.symbol) {
            const Symbol* s = nt.symbol;
            if (s->kind == SymbolKind::type_param) {
                if (subst) {
                    if (auto it = subst->find(s); it != subst->end()) return it->second;
                }
                return tc_.type_param(s);
            }
            if (s->is_type() && s->decl && !s->decl->is_template) return tc_.user(s);
            return tc_.error();   // template types are not instantiated yet
        }
        if (!segs.empty() && segs[0].name == "std") {
            if (segs.size() == 2 && segs[1].name == "raw_ptr" && segs[1].template_args.size() == 1 &&
                segs[1].template_args[0].type) {
                const TemplateArg& arg = segs[1].template_args[0];
                return tc_.pointer(lower(*arg.type, subst), arg.is_mut);
            }
            const Type* ext = tc_.external(to_string(nt.path));
            std::vector<const Type*> args;
            for (const TemplateArg& a : segs.back().template_args) {
                args.push_back(a.type ? lower(*a.type, subst) : tc_.error());
            }
            external_args_[ext] = args;
            return ext;
        }
        return tc_.error();
    }

    const Type* type_of_symbol(const Symbol* s) {
        if (!s) return tc_.error();
        switch (s->kind) {
        case SymbolKind::local: {
            auto it = local_types_.find(s);
            return it == local_types_.end() ? tc_.error() : it->second;
        }
        case SymbolKind::global:
        case SymbolKind::field: return lower(*s->decl->as<VariableDecl>().type);
        case SymbolKind::alternative: return tc_.user(s->parent);
        default: return tc_.error();
        }
    }

    // --- assignability --------------------------------------------------------------------

    static bool is_ancestor(const Symbol* t, const Symbol* base) {
        for (const Symbol* b : t->bases) {
            if (b == base || is_ancestor(b, base)) return true;
        }
        return false;
    }

    /// 0 = exact, 1 = upcast, -1 = not assignable. `from` is the value's type, `to` the target.
    int assign_cost(const Type* from, const Type* to) {
        if (!from || !to) return 0;
        bool to_ref = to->is(Kind::reference) || to->is(Kind::pointer);
        const Type* f = types::strip_ref(from);
        const Type* t = types::strip_ref(to);
        if (f->is_unknown() || t->is_unknown()) return 0;
        if (f == t || f->is(Kind::never)) return 0;
        if (f->is(Kind::external) && t->is(Kind::external)) return 0;
        if (f->is(Kind::static_str) && t->is(Kind::external)) return 0;
        if (f->is(Kind::nullptr_) && t->is(Kind::pointer)) return 0;
        if (f->is(Kind::pointer) && t->is(Kind::pointer)) {
            if (!f->is_mut && t->is_mut) return -1;
            if (f->inner == t->inner || f->inner->is_unknown() || t->inner->is_unknown()) return 0;
            if (f->inner->is(Kind::user) && t->inner->is(Kind::user) && is_ancestor(f->inner->symbol, t->inner->symbol)) return 1;
            return -1;
        }
        if (f->is(Kind::user) && t->is(Kind::user) && to_ref && is_ancestor(f->symbol, t->symbol)) return 1;
        return -1;
    }

    void mismatch(Span span, const Type* found, const Type* expected, std::string_view what = "") {
        const Type* f = types::strip_ref(found);
        const Type* t = types::strip_ref(expected);
        std::string found_text = found == tc_.untyped_int()     ? "an integer literal"
                               : found == tc_.untyped_float()   ? "a float literal"
                               : found == tc_.untyped_default() ? "'default'"
                                                                : quoted(found);
        Diagnostic& e = error(DiagCode::E4010, span,
                              std::format("expected {}{}, found {}", quoted(expected), what, found_text),
                              std::format("this is {}", found_text));
        if (f->is_numeric() && t->is_numeric()) {
            if (lossless(f, t)) {
                note(e, std::format("there are no implicit conversions; write 'cast<{}>(...)'", types::to_string(t)));
            } else {
                note(e, std::format("this conversion can lose data: use 'cast::truncate<{0}>', 'cast::saturate<{0}>' "
                                    "or 'cast::checked<{0}>'",
                                    types::to_string(t)));
            }
        } else if (f->is_integer() && t->is(Kind::bool_)) {
            note(e, "integers convert to bool only in conditions; write 'x != 0'");
        } else if (f->is(Kind::user) && t->is(Kind::user) && is_ancestor(f->symbol, t->symbol)) {
            note(e, "copying a derived object into a base value would slice it; use a reference");
        }
    }

    // --- untyped values -------------------------------------------------------------------

    /// Gives an untyped value (literal, `bit(n)`, `default`, or arithmetic on those) the type
    /// `target`, checking that it fits.
    void settle(Expr& e, const Type* target, bool negative = false) {
        const Type* t = types::strip_ref(target);
        if (t->is_untyped()) return;   // still no context: leave it untyped
        if (t->is_unknown() || t->is(Kind::external)) {
            e.type = t;
            return;
        }
        switch (e.kind) {
        case ExprKind::int_literal: {
            const auto& lit = e.as<LiteralExpr>();
            e.type          = t;
            if (t->is_float()) {
                note(error(DiagCode::E4003, e.span, std::format("integer literal used as {}", quoted(t)),
                           "integer literal"),
                     std::format("write '{}.0'", lit.text));
            } else if (!t->is_integer()) {
                mismatch(e.span, tc_.untyped_int(), t);
            } else if (!types::literal_fits(lit.int_value, negative, t)) {
                note(error(DiagCode::E4002, e.span,
                           std::format("{}{} does not fit in {}", negative ? "-" : "", lit.text, quoted(t)),
                           "out of range"),
                     std::format("{} holds {}", quoted(t), types::integer_range(t)));
            }
            return;
        }
        case ExprKind::float_literal:
            e.type = t;
            if (!t->is_float()) mismatch(e.span, tc_.untyped_float(), t);
            return;
        case ExprKind::unary: {
            auto& u = e.as<UnaryExpr>();
            if (u.op == UnaryOp::negate) {
                if (t->is_integer() && !t->is_signed && u.operand->kind != ExprKind::int_literal) {
                    error(DiagCode::E4015, e.span, std::format("cannot negate a value of unsigned type {}", quoted(t)),
                          "unsigned");
                }
                settle(*u.operand, target, !negative);
            } else {
                settle(*u.operand, target, negative);
            }
            e.type = t;
            return;
        }
        case ExprKind::binary: {
            auto& b = e.as<BinaryExpr>();
            if (b.lhs->type && b.lhs->type->is_untyped()) settle(*b.lhs, target);
            if (b.rhs->type && b.rhs->type->is_untyped()) settle(*b.rhs, is_shift(b.op) ? target : target);
            check_operator_type(b.op, t, e.span);
            e.type = t;
            return;
        }
        case ExprKind::bit: {
            auto& b = e.as<BitExpr>();
            e.type  = t;
            if (!t->is_integer()) {
                error(DiagCode::E4013, e.span, std::format("bit(n) produces an integer, but {} is expected", quoted(t)),
                      "not an integer type");
                return;
            }
            check_bit_index(*b.operand, t);
            return;
        }
        case ExprKind::default_:
            e.type = t;
            check_has_default(t, e.span);
            return;
        case ExprKind::block: {
            auto& b = e.as<BlockExpr>();
            if (b.tail) settle(*b.tail, target, negative);
            e.type = t;
            return;
        }
        case ExprKind::if_: {
            auto& i = e.as<IfExpr>();
            coerce(*i.then_block, target);
            if (i.else_branch) coerce(*i.else_branch, target);
            e.type = t;
            return;
        }
        case ExprKind::match: {
            for (MatchArm& arm : e.as<MatchExpr>().arms) coerce(*arm.body, target);
            e.type = t;
            return;
        }
        default: e.type = t; return;
        }
    }

    /// The expression's value is used where `target` is expected: settle untyped values,
    /// otherwise check assignability.
    void coerce(Expr& e, const Type* target) {
        if (!target || !e.type) return;
        if (e.type->is_untyped()) {
            settle(e, target);
            return;
        }
        // Branches are checked one by one so the error points at the wrong branch.
        if (e.type->is(Kind::error)) {
            if (e.kind == ExprKind::if_ || e.kind == ExprKind::match || e.kind == ExprKind::block) {
                settle(e, target);
            }
            return;
        }
        if (e.kind == ExprKind::block) {
            auto& b = e.as<BlockExpr>();
            if (b.tail) {
                coerce(*b.tail, target);
                return;
            }
        }
        const Type* t = types::strip_ref(target);
        if (e.type->is(Kind::void_) && !t->is(Kind::void_) && !t->is_unknown()) {
            Span end = Span{e.span.file, e.span.end > 0 ? e.span.end - 1 : 0, e.span.end};
            error(DiagCode::E4010, e.kind == ExprKind::block ? end : e.span,
                  std::format("expected {}, found no value", quoted(target)), "this block has no value")
                ;
            return;
        }
        if (assign_cost(e.type, target) < 0) {
            mismatch(e.span, e.type, target);
            return;
        }
        if (e.type->is(Kind::static_str) && t->is(Kind::external) && t->name == "std::string") {
            note(error(DiagCode::W4002, e.span, "implicit allocation of a std::string from a string literal",
                       "allocates"),
                 "write 'std::string::from(\"...\")' to make the allocation visible");
        }
    }

    /// An untyped value in a place without any context (`auto x = 5;`, `if (3 < 4)`).
    void no_context(Expr& e, std::string_view where) {
        std::string what = e.type == tc_.untyped_default() ? "'default'"
                         : e.kind == ExprKind::bit          ? "'bit(n)'"
                         : e.type == tc_.untyped_float()    ? "this float literal"
                                                            : "this integer literal";
        Diagnostic& d = error(DiagCode::E4001, e.span, std::format("{} has no type {}", what, where), "type unknown");
        if (e.kind == ExprKind::int_literal) {
            note(d, std::format("give it a suffix like '{}u32', or a typed declaration like 'u32 x = {}'",
                                e.as<LiteralExpr>().text, e.as<LiteralExpr>().text));
        } else if (e.kind == ExprKind::bit) {
            note(d, "write e.g. 'u32 x = bit(4);' or 'bit<u32>(4)'");
        } else if (e.kind == ExprKind::float_literal) {
            note(d, std::format("give it a suffix like '{}f64'", e.as<LiteralExpr>().text));
        }
        e.type = tc_.error();
    }

    // --- declarations ---------------------------------------------------------------------

    void check_file(ast::File& file) {
        for (DeclPtr& d : file.decls) check_decl(*d, nullptr);
    }

    void check_decl(Decl& d, Symbol* self) {
        switch (d.kind) {
        case DeclKind::import: return;
        case DeclKind::function: check_function(d.as<FunctionDecl>(), self); return;
        case DeclKind::variable:
        case DeclKind::field: {
            auto& v = d.as<VariableDecl>();
            if (!v.init) return;
            FunctionState saved = fn_;
            fn_                 = FunctionState{};
            fn_.is_compile_time = v.storage != Storage::local;   // static/compile_time initializers run at compile time
            infer(*v.init);
            coerce(*v.init, lower(*v.type));
            fn_ = saved;
            return;
        }
        case DeclKind::struct_:
        case DeclKind::class_:
        case DeclKind::interface: {
            Symbol* t = symbol_for(d);
            for (DeclPtr& m : d.as<TypeDecl>().members) check_decl(*m, t);
            return;
        }
        case DeclKind::enum_: {
            auto&       e   = d.as<EnumDecl>();
            const Type* tag = lower(*e.tag_type);
            if (!tag->is_integer() && !tag->is_unknown()) {
                error(DiagCode::E4130, e.tag_type->span,
                      std::format("the tag type of enum '{}' must be an integer type, not {}", e.name, quoted(tag)),
                      "not an integer");
                return;
            }
            for (EnumAlternative& alt : e.alternatives) {
                if (alt.value) {
                    infer(*alt.value);
                    coerce(*alt.value, tag);
                }
            }
            return;
        }
        }
    }

    Symbol* symbol_for(const Decl& d) {
        auto it = module_->items.find(d.name);
        if (it == module_->items.end()) return nullptr;
        for (Symbol* s : it->second) {
            if (s->decl == &d || s->definition == &d) return s;
        }
        return it->second.front()->is_type() ? it->second.front() : nullptr;
    }

    void check_function(FunctionDecl& fn, Symbol* self) {
        if (!fn.body) return;
        FunctionState saved   = fn_;
        int           unsafe0 = unsafe_depth_;
        fn_                   = FunctionState{};
        fn_.in_function       = true;
        fn_.result            = fn.result ? lower(*fn.result) : tc_.void_();
        if (fn.result && fn.result->kind == TypeKind::deduce) fn_.result = nullptr;
        fn_.self            = self;
        fn_.has_this        = self && !fn.is_static;
        fn_.this_mut        = fn.is_mut || fn.is_destructor;
        fn_.is_compile_time = fn.is_compile_time;
        fn_.name            = fn.name;
        if (fn.is_unsafe) ++unsafe_depth_;
        for (Param& p : fn.params) {
            if (p.symbol) local_types_[p.symbol] = lower(*p.type);
        }
        infer_block(*fn.body);

        const Type* result = fn_.result;
        if (result && !result->is(Kind::void_) && !result->is_unknown()) {
            if (fn.body->tail && !(fn.body->tail->type && fn.body->tail->type->is(Kind::void_)) &&
                !diverges(*fn.body->tail)) {
                Diagnostic& e = error(DiagCode::E4043, fn.body->tail->span,
                                      std::format("'{}' returns its value without 'return'", fn.name), "missing 'return'");
                note(e, "write 'return ...;'");
            } else if (!diverges_block(*fn.body)) {
                Span end = Span{fn.body->span.file, fn.body->span.end - 1, fn.body->span.end};
                Diagnostic& e = error(DiagCode::E4042, end,
                                      std::format("'{}' may reach its end without returning a {}", fn.name, quoted(result)),
                                      "can end here without 'return'");
                note(e, "declared here", fn.name_span);
            }
        }
        unsafe_depth_ = unsafe0;
        fn_           = saved;
    }

    void check_main(Module& m) {
        if (!m.source) return;
        for (DeclPtr& d : m.source->ast.decls) {
            if (d->kind != DeclKind::function || d->name != "main") continue;
            auto&       fn = d->as<FunctionDecl>();
            const Type* r  = fn.result ? lower(*fn.result) : tc_.void_();
            bool ok_result = r->is(Kind::void_) || r->is_integer() || r->is_unknown() ||
                             (r->is(Kind::external) && r->name == "std::exit_code");
            if (!ok_result) {
                note(error(DiagCode::E4110, fn.result->span, std::format("main cannot return {}", quoted(r)),
                           "invalid return type"),
                     "main returns 'void', 'std::exit_code' or an integer type");
            }
            bool ok_params = fn.params.empty();
            if (fn.params.size() == 1) {
                const Type* p = lower(*fn.params[0].type);
                ok_params     = p->is_unknown() || (p->is(Kind::external) && p->name == "std::span<std::str>");
            }
            if (!ok_params) {
                note(error(DiagCode::E4111, fn.name_span, "invalid parameters for main", "parameters"),
                     "main takes no parameters or 'std::span<std::str> args'");
            }
        }
    }

    // --- statements -----------------------------------------------------------------------

    const Type* infer_block(BlockExpr& b) {
        if (b.is_unsafe) ++unsafe_depth_;
        for (StmtPtr& s : b.statements) check_stmt(*s);
        const Type* t;
        if (b.tail) {
            t = infer(*b.tail);
        } else {
            t = diverges_block(b) ? tc_.never() : tc_.void_();
        }
        if (b.is_unsafe) --unsafe_depth_;
        b.type = t;
        return t;
    }

    void check_stmt(Stmt& s) {
        switch (s.kind) {
        case StmtKind::variable: {
            auto& v = s.as<VariableStmt>();
            bool  is_auto = v.type->kind == TypeKind::auto_;
            const Type* declared = is_auto ? nullptr : lower(*v.type);
            if (v.init) {
                bool saved_ct = fn_.is_compile_time;
                if (v.storage == Storage::compile_time) fn_.is_compile_time = true;
                const Type* t = infer(*v.init);
                fn_.is_compile_time = saved_ct;
                if (is_auto) {
                    if (branch_conflicts_.count(v.init.get())) {
                        Diagnostic& d = error(DiagCode::E4050, v.init->span,
                                              "the branches of this value have different types", "different types");
                        note(d, "'auto' takes the type of the value; write the type to say which one is meant");
                    }
                    if (t->is_untyped()) {
                        no_context(*v.init, "('auto' needs a value whose type is known on its own)");
                        t = tc_.error();
                    }
                    declared = t->is(Kind::never) ? tc_.error() : t;
                } else {
                    coerce(*v.init, declared);
                }
            }
            if (v.symbol) local_types_[v.symbol] = declared ? declared : tc_.error();
            return;
        }
        case StmtKind::assign: check_assign(s.as<AssignStmt>()); return;
        case StmtKind::discard: infer(*s.as<DiscardStmt>().value); return;
        case StmtKind::expression: {
            Expr&       e = *s.as<ExprStmt>().expr;
            const Type* t = infer(e);
            if ((e.kind == ExprKind::call) && t->is(Kind::external) && t->external_base() == "std::result") {
                Diagnostic& d = error(DiagCode::E4150, e.span, std::format("the {} returned here is ignored", quoted(t)),
                                      "result not used");
                note(d, "handle it with 'match' or '?', or discard it explicitly with '_ = ...;'");
            }
            return;
        }
        }
    }

    void check_assign(AssignStmt& a) {
        const Type* target = infer(*a.target);
        const Type* value  = infer(*a.value);
        Place       p      = place_of(*a.target);
        require_mutable(p, *a.target, "assign to");
        if (a.op == TokenKind::eq) {
            coerce(*a.value, target);
            return;
        }
        TokenKind   op = compound_base(a.op);
        const Type* t  = types::strip_ref(target);
        if (t->is_unknown() || t->is(Kind::external)) {
            if (value->is_untyped()) settle(*a.value, t);
            return;
        }
        if (is_shift(op)) {
            check_operator_type(op, t, a.span);
            if (value->is_untyped()) settle(*a.value, t);
            check_shift_count(*a.value, t);
            return;
        }
        if (t->is(Kind::user) && find_operator(token_kind_spelling(op), t, types::strip_ref(value))) return;
        check_operator_type(op, t, a.span);
        coerce(*a.value, t);
    }

    // --- places and mutability ------------------------------------------------------------

    Place place_of(const Expr& e) {
        Place p;
        switch (e.kind) {
        case ExprKind::path: {
            const Symbol* s = e.as<PathExpr>().symbol;
            if (!s) {
                p.is_place = p.is_mutable = true;   // unknown: don't complain
                return p;
            }
            p.name = s->name;
            if (s->kind == SymbolKind::local) {
                const Type* t = type_of_symbol(s);
                p.is_place    = true;
                p.local       = s;
                p.is_mutable  = s->is_mut || (t->is(Kind::reference) && t->is_mut) || t->is_unknown();
                if (t->is(Kind::reference) && !t->is_mut) p.is_mutable = false;
                return p;
            }
            if (s->kind == SymbolKind::global) {
                p.is_place   = true;
                p.is_mutable = s->is_mut && s->decl->as<VariableDecl>().storage == Storage::static_;
                return p;
            }
            return p;
        }
        case ExprKind::this_:
            p.is_place   = true;
            p.is_mutable = fn_.this_mut;
            p.via_this   = true;
            p.name       = "this";
            return p;
        case ExprKind::member: {
            const auto& m    = e.as<MemberExpr>();
            const Type* objt = m.object->type ? m.object->type : tc_.error();
            if (m.arrow) {
                const Type* pt = types::strip_ref(objt);
                p.is_place     = true;
                p.is_mutable   = !pt->is(Kind::pointer) || pt->is_mut;
                p.name         = ast::to_string(m.name);
                return p;
            }
            Place base = place_of(*m.object);
            p          = base;
            p.name     = (base.name.empty() ? "" : base.name + ".") + ast::to_string(m.name);
            if (objt->is(Kind::reference) && base.local == nullptr) p.is_mutable = objt->is_mut;
            if (!base.is_place) {
                p.is_place     = true;   // fields of temporaries: writable but pointless; not our concern here
                p.is_mutable   = true;
                p.is_temporary = true;
            }
            return p;
        }
        case ExprKind::index: {
            Place base = place_of(*e.as<IndexExpr>().object);
            base.name += "[...]";
            if (!base.is_place) {
                base.is_place = base.is_mutable = true;
            }
            return base;
        }
        case ExprKind::unary: {
            const auto& u = e.as<UnaryExpr>();
            if (u.op == UnaryOp::deref) {
                const Type* pt = types::strip_ref(u.operand->type ? u.operand->type : tc_.error());
                p.is_place     = true;
                p.is_mutable   = !pt->is(Kind::pointer) || pt->is_mut;
                p.name         = "*" + place_of(*u.operand).name;
                return p;
            }
            if (u.op == UnaryOp::move || u.op == UnaryOp::mut_marker) return place_of(*u.operand);
            return p;
        }
        default: return p;   // temporaries
        }
    }

    void require_mutable(const Place& p, const Expr& e, std::string_view action) {
        if (!p.is_place) {
            error(DiagCode::E4025, e.span, std::format("cannot {} this expression", action), "not a variable or field");
            return;
        }
        if (p.is_mutable) return;
        if (p.via_this) {
            Diagnostic& d = error(DiagCode::E4021, e.span,
                                  std::format("cannot {} '{}' in a non-mut method", action, p.name), "'this' is immutable here");
            note(d, std::format("add 'mut' after the parameter list: '{}(...) mut'", fn_.name));
            return;
        }
        Diagnostic& d = error(DiagCode::E4020, e.span, std::format("cannot {} '{}': it is not mutable", action, p.name),
                              "immutable");
        if (p.local) {
            const Type* t = type_of_symbol(p.local);
            if (t->is(Kind::reference)) {
                note(d, std::format("'{}' is an immutable reference; declare it 'mut {}'", p.local->name,
                                    types::to_string(t)),
                     p.local->span);
            } else {
                note(d, std::format("declare it 'mut {} {}'", types::to_string(t), p.local->name), p.local->span);
            }
        }
    }

    // --- expressions ----------------------------------------------------------------------

    const Type* set(Expr& e, const Type* t) {
        e.type = t;
        return t;
    }

    const Type* infer(Expr& e) {
        switch (e.kind) {
        case ExprKind::int_literal: {
            const auto& lit = e.as<LiteralExpr>();
            if (!lit.suffix.empty()) {
                const Type* t = tc_.builtin(lit.suffix);
                if (t && t->is_integer() && !types::literal_fits(lit.int_value, false, t)) {
                    note(error(DiagCode::E4002, e.span, std::format("{} does not fit in {}", lit.text, quoted(t)),
                               "out of range"),
                         std::format("{} holds {}", quoted(t), types::integer_range(t)));
                }
                return set(e, t ? t : tc_.error());
            }
            return set(e, tc_.untyped_int());
        }
        case ExprKind::float_literal: {
            const auto& lit = e.as<LiteralExpr>();
            if (!lit.suffix.empty()) return set(e, tc_.builtin(lit.suffix));
            return set(e, tc_.untyped_float());
        }
        case ExprKind::char_literal: return set(e, tc_.char_());
        case ExprKind::bool_literal: return set(e, tc_.bool_());
        case ExprKind::nullptr_literal: return set(e, tc_.nullptr_());
        case ExprKind::string_literal: {
            const auto& lit = e.as<LiteralExpr>();
            return set(e, lit.token_kind == TokenKind::format_string_literal ? tc_.external("std::format_args")
                                                                              : tc_.static_str());
        }
        case ExprKind::this_:
            if (!fn_.self || !fn_.has_this) return set(e, tc_.error());
            return set(e, tc_.reference(tc_.user(fn_.self), fn_.this_mut));
        case ExprKind::default_: return set(e, tc_.untyped_default());
        case ExprKind::break_:
        case ExprKind::continue_: return set(e, tc_.never());
        case ExprKind::error: return set(e, tc_.error());
        case ExprKind::path: return infer_path(e.as<PathExpr>());
        case ExprKind::unary: return infer_unary(e.as<UnaryExpr>());
        case ExprKind::binary: return infer_binary(e.as<BinaryExpr>());
        case ExprKind::range: {
            auto&       r  = e.as<RangeExpr>();
            const Type* lo = types::strip_ref(infer(*r.lo));
            const Type* hi = types::strip_ref(infer(*r.hi));
            const Type* el = unify_operands(*r.lo, *r.hi, lo, hi, e.span, "range bounds");
            if (!el->is_integer() && !el->is_untyped() && !el->is_unknown() && !el->is(Kind::char_)) {
                error(DiagCode::E4013, e.span, std::format("ranges need integer bounds, found {}", quoted(el)),
                      "not an integer");
                el = tc_.error();
            }
            return set(e, tc_.range(el));
        }
        case ExprKind::call: return infer_call(e.as<CallExpr>());
        case ExprKind::index: {
            auto& i = e.as<IndexExpr>();
            infer(*i.object);
            infer(*i.index);
            return set(e, tc_.error());   // containers are library types
        }
        case ExprKind::member: return infer_member(e.as<MemberExpr>());
        case ExprKind::try_: infer(*e.as<TryExpr>().operand); return set(e, tc_.error());
        case ExprKind::cast: return infer_cast(e.as<CastExpr>());
        case ExprKind::bit: return infer_bit(e.as<BitExpr>());
        case ExprKind::struct_literal: return infer_struct_literal(e.as<StructLiteralExpr>());
        case ExprKind::lambda: return infer_lambda(e.as<LambdaExpr>());
        case ExprKind::if_: return infer_if(e.as<IfExpr>());
        case ExprKind::match: return infer_match(e.as<MatchExpr>());
        case ExprKind::block: return infer_block(e.as<BlockExpr>());
        case ExprKind::while_: {
            auto& w = e.as<WhileExpr>();
            check_condition(*w.condition, "while");
            infer_block(*w.body);
            return set(e, diverges(e) ? tc_.never() : tc_.void_());
        }
        case ExprKind::for_: return infer_for(e.as<ForExpr>());
        case ExprKind::return_: {
            auto& r = e.as<ReturnExpr>();
            check_return(r);
            return set(e, tc_.never());
        }
        }
        return set(e, tc_.error());
    }

    void check_return(ReturnExpr& r) {
        if (r.value) infer(*r.value);
        const Type* result = fn_.result;
        if (!result) return;   // lambda without '->', or deduce
        if (result->is(Kind::void_)) {
            if (r.value && !(r.value->type && r.value->type->is(Kind::void_))) {
                error(DiagCode::E4041, r.value->span, std::format("'{}' returns void but a value is returned here", fn_.name),
                      "remove this value");
            }
            return;
        }
        if (!r.value) {
            if (!result->is_unknown() && !result->is(Kind::never)) {
                error(DiagCode::E4040, r.span, std::format("'{}' must return a {}", fn_.name, quoted(result)),
                      "return without a value");
            }
            return;
        }
        coerce(*r.value, result);
    }

    void check_condition(Expr& e, std::string_view construct) {
        const Type* t = types::strip_ref(infer(e));
        if (t->is_untyped()) {
            no_context(e, std::format("in this {} condition", construct));
            return;
        }
        if (t->is(Kind::bool_) || t->is_integer() || t->is(Kind::pointer) || t->is_unknown() || t->is(Kind::external) ||
            t->is(Kind::never)) {
            return;
        }
        Diagnostic& d = error(DiagCode::E4012, e.span, std::format("{} does not convert to bool", quoted(t)),
                              "not a condition");
        if (t->is_float()) note(d, "floats are not conditions (NaN makes \"is zero\" ambiguous); write 'x != 0.0'");
        else note(d, "only bool, integers and std::raw_ptr can be conditions");
    }

    const Type* infer_path(PathExpr& pe) {
        const Symbol* s = pe.symbol;
        if (!s) return set(pe, tc_.error());
        if (s->kind == SymbolKind::global) check_static_mut_access(*s, pe.span);
        switch (s->kind) {
        case SymbolKind::local:
        case SymbolKind::global: return set(pe, type_of_symbol(s));
        case SymbolKind::alternative: return set(pe, tc_.user(s->parent));
        case SymbolKind::function:
        case SymbolKind::method:
            note(error(DiagCode::E4073, pe.span, std::format("'{}' is a function; call it", s->name), "not called"),
                 std::format("write '{}(...)'", ast::to_string(pe.path)));
            return set(pe, tc_.error());
        default: return set(pe, tc_.error());
        }
    }

    void check_static_mut_access(const Symbol& s, Span span) {
        const auto& v = s.decl->as<VariableDecl>();
        if (!v.is_mut || v.storage != Storage::static_ || unsafe_depth_ > 0) return;
        const Type* t = lower(*v.type);
        if (t->is(Kind::external) && (t->external_base() == "std::atomic" || t->external_base() == "std::mutex")) return;
        note(error(DiagCode::E4102, span, std::format("'{}' is a 'static mut' of type {}", s.name, quoted(t)),
                   "needs 'unsafe'"),
             "only thread-safe types (std::atomic, std::mutex) can be used without 'unsafe { }'");
    }

    const Type* unify_operands(Expr& lhs, Expr& rhs, const Type* l, const Type* r, Span span, std::string_view what) {
        if (l->is_unknown() || r->is_unknown()) {
            if (l->is_untyped()) settle(lhs, r);
            if (r->is_untyped()) settle(rhs, l);
            return tc_.error();
        }
        if (l->is_untyped() && r->is_untyped()) {
            if (l != r) {
                error(DiagCode::E4011, span, "integer and float literals cannot be mixed", "write e.g. '1.0' instead of '1'");
                return tc_.error();
            }
            return l;
        }
        if (l->is_untyped()) {
            settle(lhs, r);
            return r;
        }
        if (r->is_untyped()) {
            settle(rhs, l);
            return l;
        }
        if (l != r) {
            Diagnostic& d = error(DiagCode::E4011, span,
                                  std::format("{} have different types: {} and {}", what, quoted(l), quoted(r)),
                                  "mismatched types");
            if (l->is_numeric() && r->is_numeric()) {
                const Type* wide = lossless(l, r) ? r : lossless(r, l) ? l : nullptr;
                if (wide) note(d, std::format("there are no implicit conversions; convert one side with 'cast<{}>(...)'",
                                              types::to_string(wide)));
                else note(d, "there are no implicit conversions; convert one side with a cast");
            }
            return tc_.error();
        }
        return l;
    }

    /// Checks that `op` applies to values of type `t` (already unified).
    bool check_operator_type(TokenKind op, const Type* t, Span span) {
        if (t->is_unknown() || t->is_untyped() || t->is(Kind::external)) return true;
        std::string_view o = token_kind_spelling(op);
        if (is_arithmetic(op)) {
            if (t->is_numeric()) return true;
        } else if (is_wrapping(op) || is_bitwise(op) || is_shift(op)) {
            if (t->is_integer()) return true;
            if (is_wrapping(op) && t->is_float()) {
                error(DiagCode::E4013, span, std::format("'{}' is wrapping integer arithmetic; floats never wrap", o),
                      std::format("use '{}'", o.substr(0, 1)));
                return false;
            }
        } else {
            return true;
        }
        Diagnostic& d = error(DiagCode::E4013, span,
                              std::format("operator '{}' does not apply to {}", o, quoted(t)), "invalid operand type");
        if (is_bitwise(op) && t->is(Kind::bool_)) note(d, "for booleans use '&&' and '||'");
        return false;
    }

    /// A user-defined operator for `l op r`: a method `operator op` on `l` or a free function.
    const Type* find_operator(std::string_view op, const Type* l, const Type* r) {
        auto try_fn = [&](const Symbol* s, bool is_method) -> const Type* {
            const FunctionDecl* f = s->function();
            if (!f || !f->is_operator || f->name != op) return nullptr;
            std::size_t want = is_method ? 1 : 2;
            if (f->params.size() != want) return nullptr;
            const Type* first = is_method ? l : lower(*f->params[0].type);
            const Type* second = lower(*f->params[want - 1].type);
            if (!is_method && assign_cost(l, first) < 0) return nullptr;
            if (r && assign_cost(r, second) < 0) return nullptr;
            if (f->is_defaulted && op == "==") return tc_.bool_();
            return f->result ? lower(*f->result) : tc_.void_();
        };
        if (l->is(Kind::user)) {
            if (auto it = l->symbol->members.find(std::string(op)); it != l->symbol->members.end()) {
                for (Symbol* m : it->second) {
                    if (const Type* t = try_fn(m, true)) return t;
                }
            }
        }
        for (const auto* items : {&module_->items}) {
            if (auto it = items->find(std::string(op)); it != items->end()) {
                for (Symbol* s : it->second) {
                    if (const Type* t = try_fn(s, false)) return t;
                }
            }
        }
        // Operators declared in the module that owns the type.
        if (l->is(Kind::user) && l->symbol->module != module_) {
            auto& items = l->symbol->module->items;
            if (auto it = items.find(std::string(op)); it != items.end()) {
                for (Symbol* s : it->second) {
                    if (const Type* t = try_fn(s, false)) return t;
                }
            }
        }
        return nullptr;
    }

    const Type* infer_binary(BinaryExpr& b) {
        TokenKind op = b.op;
        if (op == TokenKind::amp_amp || op == TokenKind::pipe_pipe) {
            check_condition(*b.lhs, std::string(token_kind_spelling(op)));
            check_condition(*b.rhs, std::string(token_kind_spelling(op)));
            return set(b, tc_.bool_());
        }
        const Type* l = types::strip_ref(infer(*b.lhs));
        const Type* r = types::strip_ref(infer(*b.rhs));

        if (op == TokenKind::spaceship) {
            unify_operands(*b.lhs, *b.rhs, l, r, b.span, "operands of '<=>'");
            return set(b, tc_.external("std::ordering"));
        }
        // User-defined operators.
        if (l->is(Kind::user) || r->is(Kind::user)) {
            std::string_view spelling = token_kind_spelling(op);
            bool is_eq = op == TokenKind::eq_eq || op == TokenKind::bang_eq;
            if (is_eq) spelling = "==";
            if (is_comparison(op)) spelling = "<=>";
            if (const Type* res = find_operator(spelling, l, r)) {
                if (r->is_untyped()) settle(*b.rhs, l);
                return set(b, is_eq || is_comparison(op) ? tc_.bool_() : res);
            }
            if (is_eq && l == r && l->is_user(SymbolKind::enum_) && !enum_has_payload(l->symbol)) {
                return set(b, tc_.bool_());
            }
            if (is_eq && l == r) {
                Diagnostic& d = error(DiagCode::E4014, b.span, std::format("{} has no 'operator=='", quoted(l)),
                                      "cannot compare");
                note(d, std::format("'==' is opt-in: add 'bool operator==({}& other) = default;'", l->symbol->name));
                return set(b, tc_.bool_());
            }
            if (!l->is_unknown() && !r->is_unknown() && !l->is_untyped() && !r->is_untyped() && l != r) {
                unify_operands(*b.lhs, *b.rhs, l, r, b.span, "operands");
                return set(b, tc_.error());
            }
            error(DiagCode::E4013, b.span,
                  std::format("operator '{}' is not defined for {}", token_kind_spelling(op), quoted(l->is(Kind::user) ? l : r)),
                  "no such operator");
            return set(b, is_eq || is_comparison(op) ? tc_.bool_() : tc_.error());
        }
        if (l->is(Kind::external) || r->is(Kind::external)) {
            if (l->is_untyped()) settle(*b.lhs, r);
            if (r->is_untyped()) settle(*b.rhs, l);
            bool boolean = is_comparison(op) || op == TokenKind::eq_eq || op == TokenKind::bang_eq;
            return set(b, boolean ? tc_.bool_() : tc_.error());
        }
        if (is_shift(op)) {
            check_operator_type(op, l, b.span);
            if (r->is_untyped()) settle(*b.rhs, l->is_untyped() ? tc_.builtin("u32") : l);
            else if (!r->is_integer() && !r->is_unknown()) {
                error(DiagCode::E4013, b.rhs->span, std::format("shift count must be an integer, found {}", quoted(r)),
                      "not an integer");
            }
            if (!l->is_untyped()) check_shift_count(*b.rhs, l);
            return set(b, l);
        }
        if (op == TokenKind::eq_eq || op == TokenKind::bang_eq || is_comparison(op)) {
            if (l->is_untyped() && r->is_untyped()) {
                no_context(*b.lhs, "(both sides of the comparison are untyped literals)");
                return set(b, tc_.bool_());
            }
            if ((l->is(Kind::pointer) && r->is(Kind::nullptr_)) || (l->is(Kind::nullptr_) && r->is(Kind::pointer))) {
                return set(b, tc_.bool_());
            }
            const Type* t = unify_operands(*b.lhs, *b.rhs, l, r, b.span, "the compared values");
            if (is_comparison(op) && !t->is_numeric() && !t->is(Kind::char_) && !t->is_unknown() && !t->is_untyped()) {
                error(DiagCode::E4013, b.span, std::format("{} values cannot be ordered", quoted(t)), "not ordered");
            }
            return set(b, tc_.bool_());
        }
        const Type* t = unify_operands(*b.lhs, *b.rhs, l, r, b.span, "operands");
        check_operator_type(op, t, b.span);
        return set(b, t);
    }

    static bool enum_has_payload(const Symbol* e) {
        return std::any_of(e->member_order.begin(), e->member_order.end(),
                           [](const Symbol* a) { return a->alternative && a->alternative->payload; });
    }

    /// Constant shift counts must be smaller than the width (spec 16).
    void check_shift_count(const Expr& count, const Type* t) {
        if (!t->is_integer()) return;
        const Expr* c = &count;
        if (c->kind != ExprKind::int_literal) return;
        u128 n = c->as<LiteralExpr>().int_value;
        if (n >= static_cast<u128>(t->bits)) {
            note(error(DiagCode::E4080, count.span,
                       std::format("shift by {} is not smaller than the width of {} ({} bits)", rift::to_string(n),
                                   quoted(t), t->bits),
                       "out of range"),
                 "a shift by the full width or more would panic at run time");
        }
    }

    void check_bit_index(Expr& operand, const Type* t) {
        const Type* n = operand.type ? operand.type : infer(operand);
        if (n->is_untyped()) settle(operand, tc_.builtin("u32"));
        if (operand.kind == ExprKind::int_literal && t->is_integer()) {
            u128 v = operand.as<LiteralExpr>().int_value;
            if (v >= static_cast<u128>(t->bits)) {
                note(error(DiagCode::E4080, operand.span,
                           std::format("bit({}) does not exist in {} ({} bits)", rift::to_string(v), quoted(t), t->bits),
                           "out of range"),
                     std::format("bit indices of {} are 0 to {}", quoted(t), t->bits - 1));
            }
        }
    }

    const Type* infer_unary(UnaryExpr& u) {
        const Type* t = infer(*u.operand);
        const Type* s = types::strip_ref(t);
        switch (u.op) {
        case UnaryOp::negate:
            if (s->is_untyped()) return set(u, s);
            if (s->is_integer() && !s->is_signed) {
                error(DiagCode::E4015, u.span, std::format("cannot negate a value of unsigned type {}", quoted(s)),
                      "unsigned");
            } else if (!s->is_numeric() && !s->is_unknown() && !s->is(Kind::external) && !s->is(Kind::user)) {
                error(DiagCode::E4013, u.span, std::format("operator '-' does not apply to {}", quoted(s)), "not a number");
            }
            return set(u, s);
        case UnaryOp::logical_not:
            check_condition_type(*u.operand, s);
            return set(u, tc_.bool_());
        case UnaryOp::bit_not:
            if (s->is_untyped()) return set(u, s);
            check_operator_type(TokenKind::amp, s, u.span);
            return set(u, s);
        case UnaryOp::deref:
            if (s->is(Kind::pointer)) {
                require_unsafe(u.span, "dereferencing a raw pointer");
                return set(u, s->inner);
            }
            if (s->is_unknown() || s->is(Kind::external)) return set(u, tc_.error());
            error(DiagCode::E4013, u.span, std::format("cannot dereference a value of type {}", quoted(s)), "not a pointer");
            return set(u, tc_.error());
        case UnaryOp::address_of: return set(u, tc_.pointer(s, false));
        case UnaryOp::address_of_mut:
            require_mutable(place_of(*u.operand), *u.operand, "take a mutable pointer to");
            return set(u, tc_.pointer(s, true));
        case UnaryOp::move:
        case UnaryOp::mut_marker: return set(u, t);
        }
        return set(u, tc_.error());
    }

    void check_condition_type(Expr& e, const Type* t) {
        if (t->is_untyped()) {
            no_context(e, "in this condition");
            return;
        }
        if (t->is(Kind::bool_) || t->is_integer() || t->is(Kind::pointer) || t->is_unknown() || t->is(Kind::external)) return;
        Diagnostic& d = error(DiagCode::E4012, e.span, std::format("{} does not convert to bool", quoted(t)), "not a condition");
        if (t->is_float()) note(d, "write 'x != 0.0'");
    }

    void require_unsafe(Span span, std::string_view what) {
        if (unsafe_depth_ > 0) return;
        note(error(DiagCode::E4100, span, std::format("{} requires 'unsafe'", what), "outside 'unsafe'"),
             "wrap it in 'unsafe { ... }'");
    }

    // --- members --------------------------------------------------------------------------

    Symbol* find_member(const Symbol* t, const std::string& name, bool& opaque) {
        opaque = opaque || t->has_opaque_base;
        if (auto it = t->members.find(name); it != t->members.end() && !it->second.empty()) return it->second.front();
        for (const Symbol* b : t->bases) {
            if (Symbol* s = find_member(b, name, opaque)) return s;
        }
        return nullptr;
    }

    std::vector<Symbol*> find_methods(const Symbol* t, const std::string& name) {
        std::vector<Symbol*> out;
        std::set<const Symbol*> seen;
        std::function<void(const Symbol*)> walk = [&](const Symbol* s) {
            if (!seen.insert(s).second) return;
            if (auto it = s->members.find(name); it != s->members.end()) {
                for (Symbol* m : it->second) {
                    if (m->kind == SymbolKind::method) out.push_back(m);
                }
            }
            if (out.empty()) {
                for (const Symbol* b : s->bases) walk(b);
            }
        };
        walk(t);
        return out;
    }

    bool member_visible(const Symbol* m) const {
        return m->access != Access::private_ || !m->parent || m->parent->module == module_;
    }

    const Type* infer_member(MemberExpr& m) {
        const Type* objt = infer(*m.object);
        const Type* t    = types::strip_ref(objt);
        std::string name = m.name.segments.back().name;
        if (m.arrow) {
            if (t->is(Kind::pointer)) {
                require_unsafe(m.span, "dereferencing a raw pointer with '->'");
                t = t->inner;
            } else {
                return set(m, tc_.error());   // smart pointers are library types
            }
        } else if (t->is(Kind::pointer)) {
            if (name != "offset") {
                note(error(DiagCode::E4074, m.span, "use '->' to access members through a raw pointer", "use '->'"),
                     "or dereference it first: '(*p).member', inside 'unsafe'");
            }
            return set(m, tc_.error());
        }
        if (!t->is(Kind::user)) return set(m, tc_.error());
        bool    opaque = false;
        Symbol* mem    = m.symbol ? m.symbol : find_member(t->symbol, name, opaque);
        if (m.name.segments.size() > 1) {
            mem = m.symbol;   // this.base::method, resolved in M3
        }
        if (!mem) {
            if (!opaque) {
                error(DiagCode::E4070, m.name.span, std::format("{} has no field or method '{}'", quoted(t), name), "unknown member");
            }
            return set(m, tc_.error());
        }
        m.symbol = mem;
        if (!member_visible(mem)) {
            note(error(DiagCode::E4071, m.name.span,
                       std::format("'{}::{}' is private to module '{}'", t->symbol->name, name, mem->parent->module->name),
                       "private"),
                 "declared here", mem->span);
        }
        if (mem->kind == SymbolKind::method) {
            note(error(DiagCode::E4073, m.name.span, std::format("'{}' is a method; call it", name), "not called"),
                 std::format("write '.{}(...)'", name));
            return set(m, tc_.error());
        }
        if (mem->kind != SymbolKind::field) return set(m, tc_.error());
        return set(m, lower(*mem->decl->as<VariableDecl>().type));
    }

    // --- calls ----------------------------------------------------------------------------

    struct Candidate {
        Symbol*                                 symbol;
        const FunctionDecl*                     decl;
        std::map<const Symbol*, const Type*>    subst;
        std::vector<const Type*>                params;
        const Type*                             result = nullptr;
        int                                     cost   = 0;
        bool                                    undeducible = false;
        std::string                             undeduced_name;
    };

    /// The template parameter symbol a parameter type is spelled as (`T` or `T&`), if any.
    static const Symbol* param_template_symbol(const TypeExpr& t) {
        const TypeExpr* inner = t.kind == TypeKind::reference ? t.as<ReferenceType>().inner.get() : &t;
        if (inner->kind != TypeKind::named) return nullptr;
        const Symbol* s = inner->as<NamedType>().symbol;
        return s && s->kind == SymbolKind::type_param ? s : nullptr;
    }

    /// Template parameter symbols of a function, by matching the names used in its signature.
    std::vector<const Symbol*> template_symbols(const FunctionDecl& f) {
        std::vector<const Symbol*> out(f.template_params.size(), nullptr);
        std::function<void(const TypeExpr&)> scan = [&](const TypeExpr& t) {
            if (t.kind == TypeKind::reference) {
                scan(*t.as<ReferenceType>().inner);
                return;
            }
            if (t.kind != TypeKind::named) return;
            const auto& nt = t.as<NamedType>();
            if (nt.symbol && nt.symbol->kind == SymbolKind::type_param) {
                for (std::size_t i = 0; i < f.template_params.size(); ++i) {
                    if (f.template_params[i].name == nt.symbol->name && !out[i]) out[i] = nt.symbol;
                }
            }
            for (const PathSegment& seg : nt.path.segments) {
                for (const TemplateArg& a : seg.template_args) {
                    if (a.type) scan(*a.type);
                }
            }
        };
        for (const Param& p : f.params) scan(*p.type);
        if (f.result) scan(*f.result);
        return out;
    }

    Candidate make_candidate(Symbol* s, const std::vector<Expr*>& args, const PathSegment* explicit_args) {
        Candidate c{s, s->function(), {}, {}, nullptr, 0, false, {}};
        const FunctionDecl& f = *c.decl;
        if (f.is_template) {
            std::vector<const Symbol*> syms = template_symbols(f);
            if (explicit_args) {
                for (std::size_t i = 0; i < explicit_args->template_args.size() && i < syms.size(); ++i) {
                    const TemplateArg& a = explicit_args->template_args[i];
                    if (syms[i] && a.type) c.subst[syms[i]] = lower(*a.type);
                }
            }
            for (std::size_t i = 0; i < f.params.size() && i < args.size(); ++i) {
                const Symbol* tp = param_template_symbol(*f.params[i].type);
                if (!tp || c.subst.count(tp)) continue;
                const Type* at = types::strip_ref(args[i]->type);
                if (at->is_untyped()) {
                    c.undeducible    = true;
                    c.undeduced_name = tp->name;
                } else if (!at->is(Kind::never)) {
                    c.subst[tp] = at;
                }
            }
            if (c.undeducible && c.subst.count(syms.empty() ? nullptr : syms[0])) c.undeducible = false;
        }
        for (const Param& p : f.params) c.params.push_back(lower(*p.type, &c.subst));
        c.result = f.result ? (f.result->kind == TypeKind::deduce ? tc_.error() : lower(*f.result, &c.subst)) : tc_.void_();
        return c;
    }

    /// Cost of passing `arg` to a parameter of type `param`, or -1.
    int arg_cost(const Expr& arg, const Type* param) {
        const Type* at = arg.type;
        const Type* p  = types::strip_ref(param);
        if (at->is_untyped()) {
            if (p->is_unknown() || p->is(Kind::external)) return 0;
            if (at == tc_.untyped_default()) return 0;
            if (at == tc_.untyped_int() && p->is_integer()) {
                const Expr* lit = &arg;
                bool        neg = false;
                while (lit->kind == ExprKind::unary && lit->as<UnaryExpr>().op == UnaryOp::negate) {
                    neg = !neg;
                    lit = lit->as<UnaryExpr>().operand.get();
                }
                if (lit->kind == ExprKind::int_literal && !types::literal_fits(lit->as<LiteralExpr>().int_value, neg, p)) {
                    return -1;
                }
                return 0;
            }
            if (at == tc_.untyped_float() && p->is_float()) return 0;
            return -1;
        }
        return assign_cost(at, param);
    }

    static const Expr& strip_marker(const Expr& e) {
        if (e.kind == ExprKind::unary && e.as<UnaryExpr>().op == UnaryOp::mut_marker) return *e.as<UnaryExpr>().operand;
        return e;
    }

    /// Picks an overload, reports problems, coerces the arguments. Returns the chosen symbol.
    const Candidate* resolve_overload(std::vector<Candidate>& cands, std::vector<Expr*>& args, Span span,
                                      std::string_view display) {
        std::vector<Candidate*> arity;
        for (Candidate& c : cands) {
            if (c.params.size() == args.size()) arity.push_back(&c);
        }
        if (arity.empty()) {
            if (cands.size() == 1) {
                Diagnostic& d = error(DiagCode::E4030, span,
                                      std::format("'{}' takes {} argument{}, but {} {} given", display,
                                                  cands[0].params.size(), cands[0].params.size() == 1 ? "" : "s",
                                                  args.size(), args.size() == 1 ? "is" : "are"),
                                      "wrong number of arguments");
                note(d, "declared here", cands[0].symbol->span);
            } else {
                error(DiagCode::E4032, span, std::format("no overload of '{}' takes {} arguments", display, args.size()),
                      "no matching overload");
            }
            return nullptr;
        }
        std::vector<Candidate*> viable;
        for (Candidate* c : arity) {
            c->cost = 0;
            bool ok = true;
            for (std::size_t i = 0; i < args.size() && ok; ++i) {
                int cost = arg_cost(strip_marker(*args[i]), c->params[i]);
                if (cost < 0) ok = false;
                c->cost += std::max(cost, 0);
            }
            if (ok) viable.push_back(c);
        }
        Candidate* chosen = nullptr;
        if (viable.empty()) {
            if (arity.size() > 1) {
                std::string list;
                for (std::size_t i = 0; i < args.size(); ++i) {
                    list += (i ? ", " : "") + types::to_string(strip_marker(*args[i]).type);
                }
                Diagnostic& d = error(DiagCode::E4032, span, std::format("no overload of '{}' accepts ({})", display, list),
                                      "no matching overload");
                for (Candidate* c : arity) note(d, "candidate", c->symbol->span);
                return nullptr;
            }
            chosen = arity.front();   // report argument by argument below
        } else {
            int best = std::min_element(viable.begin(), viable.end(), [](Candidate* a, Candidate* b) {
                           return a->cost < b->cost;
                       })[0]->cost;
            std::vector<Candidate*> top;
            for (Candidate* c : viable) {
                if (c->cost == best) top.push_back(c);
            }
            if (top.size() > 1) {
                Diagnostic& d = error(DiagCode::E4031, span, std::format("call to '{}' is ambiguous", display),
                                      "matches several overloads");
                for (Candidate* c : top) note(d, "candidate", c->symbol->span);
                note(d, "give literals a suffix, e.g. '5u32', to pick one");
                return nullptr;
            }
            chosen = top.front();
        }
        if (chosen->undeducible) {
            Diagnostic& d = error(DiagCode::E4160, span,
                                  std::format("cannot deduce template parameter '{}' of '{}' from untyped literals",
                                              chosen->undeduced_name, display),
                                  "type unknown");
            note(d, std::format("write '{}<u32>(...)' or give a literal a suffix", display));
            return nullptr;
        }
        for (std::size_t i = 0; i < args.size(); ++i) check_argument(*args[i], chosen->params[i]);
        return chosen;
    }

    /// Call-site `mut` (spec 04) and the argument's type.
    void check_argument(Expr& arg, const Type* param) {
        bool marked  = arg.kind == ExprKind::unary && arg.as<UnaryExpr>().op == UnaryOp::mut_marker;
        bool mut_ref = param->is(Kind::reference) && param->is_mut;
        Expr& value  = marked ? *arg.as<UnaryExpr>().operand : arg;
        if (mut_ref && !marked) {
            Place p = place_of(value);
            Diagnostic& d = error(DiagCode::E4023, arg.span, "a mutable reference argument is marked with 'mut'",
                                  std::format("write 'mut {}'", p.name.empty() ? "..." : p.name));
            note(d, "the parameter is 'mut T&': the call can change the argument, and the call site says so");
        } else if (marked && !mut_ref && !param->is_unknown()) {
            error(DiagCode::E4024, arg.span,
                  std::format("'mut' marks a mutable reference argument, but the parameter is {}", quoted(param)),
                  "remove 'mut'");
        }
        if (marked || mut_ref) require_mutable(place_of(value), value, "pass as a mutable reference");
        coerce(value, param);
        if (marked) arg.type = value.type;
    }

    void check_compile_time_call(const Symbol* callee, Span span) {
        if (!fn_.is_compile_time || !callee) return;
        const FunctionDecl* f = callee->function();
        if (!f || f->is_compile_time) return;
        Diagnostic& d = error(DiagCode::E4120, span,
                              std::format("compile-time code calls '{}', which is not a compile_time function", callee->name),
                              "run-time function");
        note(d, "declared here", callee->span);
    }

    void check_unsafe_call(const Symbol* callee, Span span) {
        if (!callee || unsafe_depth_ > 0) return;
        const FunctionDecl* f = callee->function();
        if (!f || (!f->is_unsafe && !f->is_extern)) return;
        note(error(DiagCode::E4101, span,
                   std::format("'{}' is {} and can only be called inside 'unsafe'", callee->name,
                               f->is_extern ? "an extern function" : "an unsafe function"),
                   "needs 'unsafe'"),
             "wrap the call in 'unsafe { ... }'");
    }

    const Type* infer_call(CallExpr& c) {
        std::vector<Expr*> args;
        for (ExprPtr& a : c.args) {
            infer(*a);
            args.push_back(a.get());
        }
        Expr& callee = *c.callee;

        // Free functions, static methods, enum alternatives.
        if (callee.kind == ExprKind::path) {
            auto&   pe = callee.as<PathExpr>();
            Symbol* s  = pe.symbol;
            if (!s) {
                if (!pe.path.segments.empty() && pe.path.segments[0].name == "std" &&
                    pe.path.segments.back().name == "panic") {
                    return set(c, tc_.never());
                }
                for (Expr* a : args) {
                    if (a->type && a->type->is_untyped() && a->type != tc_.untyped_default()) settle(*a, tc_.error());
                }
                return set(c, tc_.error());   // std::... and unresolved names
            }
            if (s->kind == SymbolKind::alternative) {
                const Type* payload = s->alternative->payload ? lower(*s->alternative->payload) : nullptr;
                if (payload) {
                    if (args.size() != 1) {
                        error(DiagCode::E4030, c.span,
                              std::format("'{}' carries exactly one value, but {} are given", ast::to_string(pe.path), args.size()),
                              "wrong number of values");
                    } else {
                        check_argument(*args[0], payload);
                    }
                }
                callee.type = tc_.user(s->parent);
                return set(c, tc_.user(s->parent));
            }
            if (s->kind == SymbolKind::function || s->kind == SymbolKind::method) {
                std::vector<Symbol*> overloads;
                auto&                table = s->kind == SymbolKind::function ? s->module->items : s->parent->members;
                for (Symbol* o : table[s->name]) {
                    if (o->kind == s->kind) overloads.push_back(o);
                }
                std::vector<Candidate> cands;
                for (Symbol* o : overloads) cands.push_back(make_candidate(o, args, &pe.path.segments.back()));
                const Candidate* chosen = resolve_overload(cands, args, c.span, ast::to_string(pe.path));
                if (!chosen) return set(c, tc_.error());
                pe.symbol   = chosen->symbol;
                callee.type = nullptr;   // functions are not values
                check_compile_time_call(chosen->symbol, c.span);
                check_unsafe_call(chosen->symbol, c.span);
                return set(c, chosen->result);
            }
            if (s->kind == SymbolKind::local || s->kind == SymbolKind::global) {
                infer(callee);
                for (Expr* a : args) {
                    if (a->type && a->type->is_untyped()) settle(*a, tc_.error());
                }
                return set(c, tc_.error());   // lambdas and function pointers
            }
            return set(c, tc_.error());
        }

        // Method calls: obj.method(args).
        if (callee.kind == ExprKind::member) {
            auto&       me   = callee.as<MemberExpr>();
            const Type* objt = infer(*me.object);
            const Type* t    = types::strip_ref(objt);
            std::string name = me.name.segments.back().name;
            if (t->is(Kind::pointer) && !me.arrow) {
                if (name == "offset") require_unsafe(c.span, "pointer arithmetic");
                for (Expr* a : args) {
                    if (a->type && a->type->is_untyped()) settle(*a, tc_.builtin("isize"));
                }
                callee.type = tc_.error();
                return set(c, name == "offset" ? t : tc_.error());
            }
            if (me.arrow && t->is(Kind::pointer)) {
                require_unsafe(c.span, "calling a method through a raw pointer");
                t = t->inner;
            }
            if (!t->is(Kind::user)) {
                callee.type = tc_.error();
                for (Expr* a : args) {
                    if (a->type && a->type->is_untyped() && a->type != tc_.untyped_default()) settle(*a, tc_.error());
                }
                return set(c, tc_.error());
            }
            std::vector<Symbol*> methods;
            if (me.name.segments.size() > 1 && me.symbol) {
                methods.push_back(me.symbol);   // this.base::method()
            } else {
                methods = find_methods(t->symbol, name);
            }
            callee.type = nullptr;
            if (methods.empty()) {
                bool    opaque = false;
                Symbol* other  = find_member(t->symbol, name, opaque);
                if (other) {
                    error(DiagCode::E4033, me.name.span, std::format("'{}' is a field, not a method", name), "not callable");
                } else if (!opaque) {
                    error(DiagCode::E4070, me.name.span, std::format("{} has no method '{}'", quoted(t), name),
                          "unknown method");
                }
                return set(c, tc_.error());
            }
            std::vector<Candidate> cands;
            for (Symbol* m : methods) cands.push_back(make_candidate(m, args, &me.name.segments.back()));
            const Candidate* chosen = resolve_overload(cands, args, c.span, t->symbol->name + "::" + name);
            if (!chosen) return set(c, tc_.error());
            me.symbol = chosen->symbol;
            if (!member_visible(chosen->symbol)) {
                note(error(DiagCode::E4071, me.name.span,
                           std::format("'{}::{}' is private to module '{}'", t->symbol->name, name,
                                       chosen->symbol->parent->module->name),
                           "private"),
                     "declared here", chosen->symbol->span);
            }
            if (chosen->decl->is_mut) {
                Place p = place_of(*me.object);
                if (p.is_place && !p.is_mutable && !p.is_temporary) {
                    if (p.via_this) {
                        Diagnostic& d = error(DiagCode::E4021, c.span,
                                              std::format("'{}' is a mut method; it cannot be called on 'this' in a non-mut method", name),
                                              "'this' is immutable here");
                        note(d, std::format("add 'mut' after the parameter list: '{}(...) mut'", fn_.name));
                    } else {
                        Diagnostic& d = error(DiagCode::E4022, c.span,
                                              std::format("'{}' is a mut method, but '{}' is not mutable", name, p.name),
                                              "immutable receiver");
                        if (p.local) note(d, std::format("declare it 'mut {} {}'", types::to_string(type_of_symbol(p.local)),
                                                         p.local->name),
                                          p.local->span);
                    }
                }
            }
            check_compile_time_call(chosen->symbol, c.span);
            check_unsafe_call(chosen->symbol, c.span);
            return set(c, chosen->result);
        }

        infer(callee);
        for (Expr* a : args) {
            if (a->type && a->type->is_untyped()) settle(*a, tc_.error());
        }
        return set(c, tc_.error());
    }

    // --- other expressions ----------------------------------------------------------------

    const Type* infer_cast(CastExpr& c) {
        const Type* to   = lower(*c.target);
        const Type* from = types::strip_ref(infer(*c.operand));
        set(c, to);
        const std::string& v = c.variant;
        if (v == "reinterpret") {
            if (unsafe_depth_ == 0) {
                note(error(DiagCode::E4103, c.span, "cast::reinterpret requires 'unsafe'", "outside 'unsafe'"),
                     "wrap it in 'unsafe { ... }'");
            }
            if (from->is_untyped()) settle(*c.operand, to);
            return to;
        }
        if (v == "checked" || v == "dynamic") {
            set(c, tc_.external("std::option<" + types::to_string(to) + ">"));
        } else if (!v.empty() && v != "truncate" && v != "saturate" && v != "round") {
            error(DiagCode::E4092, c.span, std::format("unknown cast 'cast::{}'", v), "unknown cast")
                ;
            return c.type;
        }
        if (from->is_untyped()) {
            if (to->is_numeric() && (from == tc_.untyped_float() ? true : to->is_integer())) {
                settle(*c.operand, from == tc_.untyped_float() ? tc_.builtin("f64") : to);
            } else {
                settle(*c.operand, from == tc_.untyped_float() ? tc_.builtin("f64") : tc_.builtin("i64"));
            }
            from = types::strip_ref(c.operand->type);
        }
        if (from->is_unknown() || to->is_unknown() || from->is(Kind::external) || to->is(Kind::external)) return c.type;
        auto invalid = [&](std::string_view why) {
            error(DiagCode::E4091, c.span,
                  std::format("{} cannot convert {} to {}", v.empty() ? "cast" : "cast::" + v, quoted(from), quoted(to)),
                  std::string(why));
        };
        if (v.empty()) {
            if (from->is_user(SymbolKind::enum_)) {
                const Type* tag = lower(*from->symbol->decl->as<EnumDecl>().tag_type);
                if (enum_has_payload(from->symbol)) invalid("enums with payloads have no tag value");
                else if (tag != to && !lossless(tag, to)) invalid(std::format("the tag type is {}", quoted(tag)));
                return c.type;
            }
            if (from->is_numeric() || from->is(Kind::bool_)) {
                if (!to->is_numeric()) {
                    invalid("not a number type");
                } else if (!lossless(from, to)) {
                    Diagnostic& d = error(DiagCode::E4090, c.span,
                                          std::format("cast<{}> from {} may lose data", types::to_string(to), quoted(from)),
                                          "lossy conversion");
                    note(d, std::format("say how: 'cast::truncate<{0}>', 'cast::saturate<{0}>', 'cast::round<{0}>' or "
                                        "'cast::checked<{0}>'",
                                        types::to_string(to)));
                }
                return c.type;
            }
            if ((from->is(Kind::reference) || from->is(Kind::user)) && assign_cost(from, to) >= 0) return c.type;
            invalid("cast<T> converts numbers, bools and simple enums");
            return c.type;
        }
        if (v == "truncate" && !(from->is_integer() && to->is_integer())) invalid("truncate converts integers to integers");
        if (v == "saturate" && !(from->is_numeric() && to->is_integer())) invalid("saturate converts numbers to integers");
        if (v == "round" && !(from->is_numeric() && to->is_float())) invalid("round converts numbers to floats");
        if (v == "checked" && !(from->is_numeric() && to->is_numeric())) invalid("checked converts numbers to numbers");
        return c.type;
    }

    const Type* infer_bit(BitExpr& b) {
        if (b.type) {
            const Type* t = lower(*b.type);
            infer(*b.operand);
            if (!t->is_integer() && !t->is_unknown()) {
                error(DiagCode::E4013, b.type->span, std::format("bit<T> needs an integer type, not {}", quoted(t)),
                      "not an integer type");
                return set(b, tc_.error());
            }
            check_bit_index(*b.operand, t);
            return set(b, t);
        }
        infer(*b.operand);
        return set(b, tc_.untyped_int());
    }

    void check_has_default(const Type* t, Span span) {
        const Type* s = t;
        if (s->is(Kind::reference)) {
            error(DiagCode::E4140, span, "references have no default value", "no default");
            return;
        }
        if (!s->is(Kind::user)) return;
        const Symbol* sym = s->symbol;
        if (sym->kind == SymbolKind::enum_ || sym->kind == SymbolKind::interface) {
            error(DiagCode::E4140, span, std::format("{} '{}' has no default value", sema::to_string(sym->kind), sym->name),
                  "no default");
            return;
        }
        std::vector<std::string> missing;
        std::function<void(const Symbol*)> walk = [&](const Symbol* ts) {
            for (const Symbol* b : ts->bases) {
                if (b->kind == SymbolKind::class_) walk(b);
            }
            for (const Symbol* m : ts->member_order) {
                if (m->kind == SymbolKind::field && !m->is_static && !m->decl->as<VariableDecl>().init) {
                    missing.push_back("'" + m->name + "'");
                }
            }
        };
        walk(sym);
        if (missing.empty() || sym->has_opaque_base) return;
        std::string list;
        for (std::size_t i = 0; i < missing.size(); ++i) list += (i ? ", " : "") + missing[i];
        Diagnostic& d = error(DiagCode::E4140, span, std::format("'{}' has no default value", sym->name), "no default");
        note(d, std::format("{} {} no default", missing.size() == 1 ? "field" : "fields", list + (missing.size() == 1 ? " has" : " have")));
    }

    const Type* infer_struct_literal(StructLiteralExpr& sl) {
        const Type* t = lower(*sl.type);
        for (FieldInit& f : sl.fields) {
            infer(*f.value);
            if (!t->is(Kind::user)) {
                if (f.value->type && f.value->type->is_untyped() && f.value->type != tc_.untyped_default()) {
                    settle(*f.value, tc_.error());
                }
                continue;
            }
            bool    opaque = false;
            Symbol* field  = find_member(t->symbol, f.name, opaque);
            if (field && field->kind == SymbolKind::field) coerce(*f.value, lower(*field->decl->as<VariableDecl>().type));
        }
        return set(sl, t);
    }

    const Type* infer_lambda(LambdaExpr& l) {
        FunctionState saved = fn_;
        fn_.result          = l.result ? lower(*l.result) : nullptr;
        fn_.in_function     = true;
        fn_.name            = "lambda";
        for (Capture& cap : l.captures) {
            if (cap.init) {
                const Type* t = infer(*cap.init);
                if (t->is_untyped()) {
                    no_context(*cap.init, "(an init-capture needs a typed value)");
                    t = tc_.error();
                }
                if (cap.symbol) local_types_[cap.symbol] = t;
            } else if (cap.symbol) {
                // Same type as the captured local; reference captures are references.
                const Type* outer = tc_.error();
                for (const auto& [sym, ty] : local_types_) {
                    if (sym->name == cap.name && sym != cap.symbol && sym->span.begin < cap.span.begin) outer = ty;
                }
                bool ref = cap.kind == CaptureKind::ref || cap.kind == CaptureKind::mut_ref;
                local_types_[cap.symbol] = ref ? tc_.reference(types::strip_ref(outer), cap.kind == CaptureKind::mut_ref)
                                               : types::strip_ref(outer);
            }
        }
        for (Param& p : l.params) {
            if (p.symbol) local_types_[p.symbol] = lower(*p.type);
        }
        infer_block(*l.body);
        fn_ = saved;
        return set(l, tc_.error());   // lambda types cannot be named
    }

    const Type* unify_branches(std::vector<Expr*> branches) {
        const Type* result = nullptr;
        for (Expr* b : branches) {
            const Type* t = b->type;
            if (!t || t->is(Kind::never)) continue;
            if (!result) {
                result = t;
                continue;
            }
            const Type* rs = types::strip_ref(result);
            const Type* ts = types::strip_ref(t);
            if (rs == ts) continue;
            if (rs->is_unknown() || ts->is_unknown()) return tc_.error();
            if (rs->is_untyped() && ts->is_untyped()) {
                if (rs != ts) return tc_.error();
                continue;
            }
            if (rs->is_untyped()) {
                result = ts;
                continue;
            }
            if (ts->is_untyped()) continue;
            return tc_.error();   // different types: reported where a value is needed
        }
        return result ? result : tc_.never();
    }

    const Type* infer_if(IfExpr& i) {
        check_condition(*i.condition, "if");
        infer_block(*i.then_block);
        if (!i.else_branch) return set(i, tc_.void_());
        infer(*i.else_branch);
        const Type* t = unify_branches({i.then_block.get(), i.else_branch.get()});
        // A value whose branches settle on one concrete type: settle untyped branches to it.
        if (!t->is_untyped() && !t->is_unknown() && !t->is(Kind::never)) {
            for (Expr* b : {static_cast<Expr*>(i.then_block.get()), i.else_branch.get()}) {
                if (b->type && b->type->is_untyped()) settle(*b, t);
            }
        }
        if (t->is(Kind::error) && branch_types_conflict({i.then_block.get(), i.else_branch.get()})) {
            branch_conflicts_.insert(&i);
        }
        return set(i, t);
    }

    bool branch_types_conflict(std::vector<Expr*> branches) {
        std::set<const Type*> seen;
        for (Expr* b : branches) {
            const Type* t = b->type ? types::strip_ref(b->type) : nullptr;
            if (!t || t->is(Kind::never) || t->is_unknown()) continue;
            seen.insert(t->is_untyped() ? t : t);
        }
        return seen.size() > 1;
    }

    const Type* infer_for(ForExpr& f) {
        const Type* it   = infer(*f.iterable);
        const Type* var  = lower(*f.type);
        const Type* sit  = types::strip_ref(it);
        if (f.symbol) local_types_[f.symbol] = var;
        if (sit->is(Kind::range)) {
            const Type* el = sit->inner;
            if (el->is_untyped()) {
                auto& r = f.iterable->as<RangeExpr>();
                settle(*r.lo, var);
                settle(*r.hi, var);
                f.iterable->type = tc_.range(types::strip_ref(var));
            } else if (!el->is_unknown() && el != types::strip_ref(var) && !var->is_unknown()) {
                mismatch(f.type->span, var, el, " (the range's element type)");
            }
            if (var->is(Kind::reference)) {
                error(DiagCode::E4010, f.type->span, "range elements are values, not references",
                      std::format("write '{}'", types::to_string(types::strip_ref(var))));
            }
        }
        infer_block(*f.body);
        return set(f, tc_.void_());
    }

    // --- match ----------------------------------------------------------------------------

    const Type* infer_match(MatchExpr& m) {
        const Type* scrut   = infer(*m.scrutinee);
        const Type* s       = types::strip_ref(scrut);
        bool        mut_ref = m.scrutinee->kind == ExprKind::unary && m.scrutinee->as<UnaryExpr>().op == UnaryOp::mut_marker;
        if (s->is_untyped()) {
            no_context(*m.scrutinee, "in this match subject");
            s = tc_.error();
        }
        std::vector<Expr*> bodies;
        for (MatchArm& arm : m.arms) {
            check_pattern(*arm.pattern, s, mut_ref);
            if (arm.guard) check_condition(*arm.guard, "match guard");
            infer(*arm.body);
            bodies.push_back(arm.body.get());
        }
        check_exhaustive(m, s);
        const Type* t = unify_branches(bodies);
        if (!t->is_untyped() && !t->is_unknown() && !t->is(Kind::never)) {
            for (Expr* b : bodies) {
                if (b->type && b->type->is_untyped()) settle(*b, t);
            }
        }
        if (t->is(Kind::error) && branch_types_conflict(bodies)) branch_conflicts_.insert(&m);
        return set(m, t);
    }

    /// The payload type of std::ok/error/some patterns, from the subject's template arguments.
    const Type* std_payload(const Type* s, const std::string& alt) {
        auto it = external_args_.find(s);
        if (it == external_args_.end()) return tc_.error();
        const auto& args = it->second;
        std::string_view base = s->external_base();
        if (base == "std::result" && args.size() == 2) return alt == "ok" ? args[0] : alt == "error" ? args[1] : tc_.error();
        if (base == "std::option" && args.size() == 1 && alt == "some") return args[0];
        return tc_.error();
    }

    void check_pattern(Pattern& p, const Type* s, bool mut_ref) {
        switch (p.kind) {
        case PatternKind::wildcard:
        case PatternKind::error: return;
        case PatternKind::binding: {
            auto& b = p.as<BindingPattern>();
            if (b.symbol) {
                b.symbol->is_mut = mut_ref;
                local_types_[b.symbol] = s;
            }
            return;
        }
        case PatternKind::literal: {
            Expr& lit = *p.as<LiteralPattern>().literal;
            const Type* t = infer(lit);
            if (s->is_unknown() || s->is(Kind::external)) {
                if (t->is_untyped()) settle(lit, tc_.error());
                return;
            }
            if (t->is_untyped()) {
                settle(lit, s);
                return;
            }
            const Type* ts = types::strip_ref(t);
            if (ts != s && !(ts->is(Kind::static_str) && s->is(Kind::external))) {
                error(DiagCode::E4061, lit.span,
                      std::format("this pattern is {}, but the match subject is {}", quoted(ts), quoted(s)),
                      "wrong type");
            }
            return;
        }
        case PatternKind::range: {
            auto& r = p.as<RangePattern>();
            for (Expr* e : {r.lo.get(), r.hi.get()}) {
                const Type* t = infer(*e);
                if (s->is_unknown() || s->is(Kind::external)) continue;
                if (!s->is_integer() && !s->is(Kind::char_)) {
                    error(DiagCode::E4061, p.span, std::format("range patterns need an integer or char subject, not {}", quoted(s)),
                          "wrong type");
                    return;
                }
                if (t->is_untyped()) settle(*e, s);
                else if (types::strip_ref(t) != s) {
                    error(DiagCode::E4061, e->span, std::format("this bound is {}, but the subject is {}", quoted(t), quoted(s)),
                          "wrong type");
                }
            }
            return;
        }
        case PatternKind::enum_alternative: {
            auto& ep  = p.as<EnumPattern>();
            const Symbol* alt = ep.symbol;
            const Type*   payload = tc_.error();
            if (alt && alt->kind == SymbolKind::alternative) {
                const Type* et = tc_.user(alt->parent);
                if (!s->is_unknown() && s != et) {
                    error(DiagCode::E4061, ep.path.span,
                          std::format("'{}' is an alternative of {}, but the match subject is {}", ast::to_string(ep.path),
                                      quoted(et), quoted(s)),
                          "wrong enum");
                }
                if (alt->alternative->payload) payload = lower(*alt->alternative->payload);
            } else if (!alt && s->is(Kind::external)) {
                payload = std_payload(s, ep.path.segments.back().name);
            }
            if (ep.payload) check_pattern(*ep.payload, payload, mut_ref);
            return;
        }
        case PatternKind::struct_: {
            auto& sp = p.as<StructPattern>();
            const Symbol* type = nullptr;
            auto it = module_->items.find(sp.path.segments.back().name);
            if (s->is(Kind::user)) type = s->symbol;
            for (FieldPattern& f : sp.fields) {
                const Type* ft = tc_.error();
                if (type) {
                    bool    opaque = false;
                    Symbol* field  = find_member(type, f.name, opaque);
                    if (field && field->kind == SymbolKind::field) ft = lower(*field->decl->as<VariableDecl>().type);
                }
                check_pattern(*f.pattern, ft, mut_ref);
            }
            (void)it;
            return;
        }
        }
    }

    /// Does this arm match every value on its own (no guard, `_` or a binding)?
    static bool is_catch_all(const MatchArm& a) {
        return !a.guard && (a.pattern->kind == PatternKind::wildcard || a.pattern->kind == PatternKind::binding);
    }

    void check_exhaustive(MatchExpr& m, const Type* s) {
        // Unreachable arms after a catch-all.
        bool covered = false;
        for (MatchArm& arm : m.arms) {
            if (covered) {
                note(error(DiagCode::W4001, arm.pattern->span, "this arm can never match", "unreachable"),
                     "an earlier arm already matches every value");
                break;
            }
            covered = is_catch_all(arm);
        }
        if (covered || s->is_unknown()) return;

        std::vector<std::string> all;   // alternative names that must be covered
        std::string              prefix;
        if (s->is_user(SymbolKind::enum_)) {
            for (const Symbol* a : s->symbol->member_order) all.push_back(a->name);
            prefix = s->symbol->name + "::";
        } else if (s->is(Kind::bool_)) {
            all = {"true", "false"};
        } else if (s->is(Kind::external) && s->external_base() == "std::result") {
            all    = {"ok", "error"};
            prefix = "std::";
        } else if (s->is(Kind::external) && s->external_base() == "std::option") {
            all    = {"some", "none"};
            prefix = "std::";
        } else if (s->is(Kind::external)) {
            return;   // library types: unknown shape
        }

        std::set<std::string> seen;
        for (const MatchArm& arm : m.arms) {
            if (arm.guard) continue;
            const Pattern& p = *arm.pattern;
            if (p.kind == PatternKind::enum_alternative) {
                const auto& ep = p.as<EnumPattern>();
                bool full_payload = !ep.payload || ep.payload->kind == PatternKind::wildcard ||
                                    ep.payload->kind == PatternKind::binding;
                if (full_payload) seen.insert(ep.path.segments.back().name);
            } else if (p.kind == PatternKind::literal && s->is(Kind::bool_)) {
                const Expr& lit = *p.as<LiteralPattern>().literal;
                if (lit.kind == ExprKind::bool_literal) seen.insert(lit.as<LiteralExpr>().bool_value ? "true" : "false");
            } else if (p.kind == PatternKind::struct_) {
                const auto& sp = p.as<StructPattern>();
                bool all_free = std::all_of(sp.fields.begin(), sp.fields.end(), [](const FieldPattern& f) {
                    return f.pattern->kind == PatternKind::wildcard || f.pattern->kind == PatternKind::binding;
                });
                if (all_free) return;   // a struct pattern with only bindings matches everything
            }
        }
        if (all.empty()) {
            Diagnostic& d = error(DiagCode::E4060, m.span,
                                  std::format("match on {} does not cover every value", quoted(s)), "not exhaustive");
            note(d, "add a final '_ => ...,' arm");
            return;
        }
        std::vector<std::string> missing;
        for (const std::string& a : all) {
            if (!seen.count(a)) missing.push_back(prefix + a);
        }
        if (missing.empty()) return;
        std::string list;
        for (std::size_t i = 0; i < missing.size(); ++i) list += (i ? ", " : "") + missing[i];
        Diagnostic& d = error(DiagCode::E4060, m.span, std::format("match on {} does not cover {}", quoted(s), list),
                              "not exhaustive");
        note(d, "add the missing arms, or a final '_ => ...,' arm");
    }

    // --- state ----------------------------------------------------------------------------

    sema::Program&                                      program_;
    DiagnosticEngine&                                   diags_;
    types::TypeContext&                                 tc_;
    Module*                                             module_ = nullptr;
    FunctionState                                       fn_;
    int                                                 unsafe_depth_ = 0;
    std::map<const Symbol*, const Type*>                local_types_;
    std::map<const Type*, std::vector<const Type*>>     external_args_;
    std::set<const Expr*>                               branch_conflicts_;
};

} // namespace

void check_types(sema::Program& program, DiagnosticEngine& diags) {
    if (!program.types) program.types = std::make_shared<types::TypeContext>();
    Checker(program, diags).run();
}

} // namespace rift

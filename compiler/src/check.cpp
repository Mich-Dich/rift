#include "rift/check.hpp"

#include "rift/ast_clone.hpp"
#include "rift/ast_printer.hpp"
#include "rift/compile_time.hpp"
#include "rift/lexer.hpp"
#include "rift/parser.hpp"
#include "rift/resolve.hpp"
#include "rift/types.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <memory>
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

class Instantiator;

class Checker {
public:
    Checker(sema::Program& program, DiagnosticEngine& diags, Instantiator* instantiator = nullptr)
        : program_(program), diags_(diags), tc_(*program.types), instantiator_(instantiator) {}

    void run() {
        for (auto& [name, m] : program_.modules) {
            module_ = m.get();
            if (m->header) check_file(m->header->ast);
            if (m->source) check_file(m->source->ast);
        }
        if (program_.entry) check_main(*program_.entry);
    }

    /// Checks one function on demand (a compile_time function that compile-time code calls
    /// before the normal pass reaches it).
    void check_function_symbol(const Symbol* s) {
        const Decl* d = s->definition ? s->definition : s->decl;
        if (!d || d->kind != DeclKind::function) return;
        module_ = s->module;
        if (const auto* subst = sema::substitution_of(s)) subst_ = *subst;
        Symbol* self = s->parent && s->parent->is_type() ? s->parent : nullptr;
        check_function(const_cast<FunctionDecl&>(d->as<FunctionDecl>()), self);
    }

    /// Checks (and evaluates) a global compile_time constant on demand.
    void check_global_symbol(const Symbol* g) {
        if (!g->decl) return;
        module_ = g->module;
        check_decl(const_cast<Decl&>(*g->decl), nullptr);
    }

    /// Checks the members of a template instance (spec 14): `copy` is the instance's own copy of
    /// the template's declaration, checked with the parameters replaced by their arguments.
    void check_instance(Symbol* inst, Decl& copy) {
        module_ = inst->module;
        subst_  = inst->template_subst;
        if (copy.kind == DeclKind::enum_) {
            check_decl(copy, inst);
            return;
        }
        if (copy.kind == DeclKind::function) {
            check_decl(copy, nullptr);
            return;
        }
        for (DeclPtr& m : copy.as<TypeDecl>().members) check_decl(*m, inst);
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

    /// A type written in the code being checked: template parameters are those of the instance
    /// being checked (`subst_`), if any.
    const Type* lower(const TypeExpr& t, const std::map<const Symbol*, const Type*>* subst = nullptr) {
        return tc_.lower(t, subst ? subst : &subst_);
    }

    /// The parameter substitution of the template instance that declares `s` (a member, payload
    /// or alternative), or null outside instances.
    static const std::map<const Symbol*, const Type*>* substitution_of(const Symbol* s) {
        for (; s; s = s->parent) {
            if (!s->template_subst.empty()) return &s->template_subst;
        }
        return nullptr;
    }

    /// The alternative that `alt` stands for in the enum type `enum_type`: an instance of a template
    /// enum has its own alternatives (spec 14). `alt` itself otherwise.
    static Symbol* instance_alternative(Symbol* alt, const Type* enum_type) {
        const Symbol* templ = alt->parent;
        if (!templ || !templ->decl || !templ->decl->is_template || templ->template_of) return alt;
        if (!enum_type || !enum_type->is(Kind::user) || enum_type->symbol->template_of != templ) return alt;
        auto it = enum_type->symbol->members.find(alt->name);
        return it == enum_type->symbol->members.end() ? alt : it->second.front();
    }

    /// The enum type that an alternative path names: `maybe<u32>::some` names `maybe<u32>`
    /// (spec 14). A plain enum's alternative names the enum.
    const Type* enum_type_of(const Symbol* alt, const Path& path) {
        const Symbol* templ = alt->parent;
        if (!templ || !templ->decl || !templ->decl->is_template || templ->template_of || path.segments.size() < 2) {
            return tc_.user(templ);
        }
        const PathSegment& seg = path.segments[path.segments.size() - 2];
        std::vector<const Type*> args;
        for (const TemplateArg& a : seg.template_args) args.push_back(a.type ? lower(*a.type) : tc_.error());
        if (args.size() != templ->decl->template_params.size()) {
            error(DiagCode::E4180, path.span,
                  std::format("'{}' takes {} template argument{}, but {} {} given", templ->name,
                              templ->decl->template_params.size(), templ->decl->template_params.size() == 1 ? "" : "s",
                              args.size(), args.size() == 1 ? "is" : "are"),
                  "wrong number of template arguments");
            return tc_.error();
        }
        return tc_.instance(templ, args);
    }

    /// A type written in the declaration of `owner` (a field, payload, method signature, ...),
    /// which may belong to a template instance other than the code being checked (spec 14).
    const Type* lower_owned(const Symbol* owner, const TypeExpr& t) { return tc_.lower(t, substitution_of(owner)); }

    void set_local(sema::Symbol* sym, const Type* t) {
        local_types_[sym] = t;
        sym->type         = t;
    }

    const Type* type_of_symbol(const Symbol* s) {
        if (!s) return tc_.error();
        switch (s->kind) {
        case SymbolKind::local: {
            auto it = local_types_.find(s);
            return it == local_types_.end() ? tc_.error() : it->second;
        }
        case SymbolKind::global: return lower(*s->decl->as<VariableDecl>().type);
        case SymbolKind::field: return lower_owned(s, *s->decl->as<VariableDecl>().type);
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
        if (f && f->is(Kind::type_value) && t && !t->is_unknown()) {
            Diagnostic& d = error(DiagCode::E3022, span, std::format("expected {}{}, found a type", quoted(expected), what),
                                  "this is a type, not a value");
            if (t->is(Kind::user) && (t->symbol->kind == SymbolKind::struct_ || t->symbol->kind == SymbolKind::class_)) {
                note(d, std::format("construct a value with '{}{{ ... }}'", types::to_string(t)));
            }
            return;
        }
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

    // --- the standard library's core types (spec 08, 11) ------------------------------------

    /// An item of the built-in std module (`option`, `result`, `exit_code`), or null when no
    /// file imports std.
    const Symbol* std_item(std::string_view name) const {
        const Module* m = program_.std_module;
        if (!m) return nullptr;
        auto it = m->items.find(std::string(name));
        return it == m->items.end() ? nullptr : it->second.front();
    }

    /// Whether `t` (references removed) is an instance of the std template `name`: `std::option<u32>`.
    static bool is_std_instance(const Type* t, std::string_view name) { return types::is_std_instance(t, name); }

    /// `r?` (spec 11): unwraps `ok`/`some`, or returns the error / `none` from the function.
    const Type* infer_try(TryExpr& t) {
        const Type* o = infer(*t.operand);
        const Type* s = types::strip_ref(o);
        if (s->is(Kind::pending)) {
            no_context(*t.operand, "before '?'");
            return set(t, tc_.error());
        }
        if (s->is_unknown() || s->is(Kind::external) || s->is(Kind::never)) return set(t, tc_.error());
        bool is_result = is_std_instance(s, "result");
        if (!is_result && !is_std_instance(s, "option")) {
            if (s->is_untyped()) settle(*t.operand, tc_.error());
            note(error(DiagCode::E4190, t.span, std::format("'?' needs a std::result or std::option, but this is {}", quoted(o)),
                       "not a result or option"),
                 "'?' unwraps 'std::ok(x)' / 'std::some(x)' and returns anything else from the function");
            return set(t, tc_.error());
        }
        const Type* r = fn_.result;
        if (fn_.in_function && r && !r->is_unknown()) {
            std::string_view kind = is_result ? "result" : "option";
            if (!is_std_instance(r, kind)) {
                Diagnostic& d = error(DiagCode::E4191, t.span,
                                      std::format("'?' would return from '{}', which returns {}, not a std::{}", fn_.name,
                                                  quoted(r), kind),
                                      std::format("returns the {} early", is_result ? "error" : "'none'"));
                note(d, std::format("'?' on a std::{0} works only in functions that return std::{0}; use 'match' or "
                                    "'.value_or(...)' here",
                                    kind));
            } else if (is_result && types::strip_ref(r)->symbol->template_args[1] != s->symbol->template_args[1]) {
                const Type* want = types::strip_ref(r)->symbol->template_args[1];
                const Type* have = s->symbol->template_args[1];
                Diagnostic& d = error(DiagCode::E4191, t.span,
                                      std::format("'?' would return the error {}, but '{}' returns errors of type {}",
                                                  quoted(have), fn_.name, quoted(want)),
                                      "different error type");
                note(d, std::format("there is no implicit error conversion; convert it with '.or_error(...)' or "
                                    "'.map_error(...)' before '?'"));
            }
        }
        return set(t, s->symbol->template_args[0]);
    }

    /// `value_or`, `expect`, `or_error`, `map_error` on std::option / std::result (spec 11). They
    /// consume the receiver.
    const Type* infer_std_method(CallExpr& c, const Type* t, const std::string& name, std::vector<Expr*>& args) {
        c.callee->type     = tc_.error();
        const Type* value  = t->symbol->template_args[0];
        if (args.size() != 1) {
            error(DiagCode::E4030, c.span, std::format("'{}' takes 1 argument, but {} {} given", name, args.size(),
                                                       args.size() == 1 ? "is" : "are"),
                  "wrong number of arguments");
            for (Expr* a : args) {
                if (a->type && a->type->is_untyped()) settle(*a, tc_.error());
            }
            return set(c, name == "or_error" || name == "map_error" ? tc_.error() : value);
        }
        Expr& a = *args[0];
        if (name == "value_or") {
            coerce(a, value);
            return set(c, value);
        }
        if (name == "expect") {
            const Type* m = types::strip_ref(a.type);
            if (m->is_untyped()) {
                mismatch(a.span, a.type, tc_.static_str(), " (the message)");
                settle(a, tc_.error());
            } else if (!m->is_unknown() && !m->is(Kind::static_str) &&
                       !(m->is(Kind::external) && (m->name == "std::str" || m->name == "std::format_args"))) {
                mismatch(a.span, a.type, tc_.static_str(), " (the message)");
            }
            return set(c, value);
        }
        if (name == "or_error") {
            const Type* e = types::strip_ref(a.type);
            if (e->is_untyped()) {
                no_context(a, "as the new error ('or_error' takes the error type from its argument)");
                return set(c, tc_.error());
            }
            if (e->is_unknown() || e->is(Kind::never) || e->is(Kind::external)) return set(c, tc_.error());
            const Symbol* result = std_item("result");
            return set(c, result ? tc_.instance(result, {value, e}) : tc_.error());
        }
        return set(c, tc_.error());   // map_error: the new error type comes from a function value (not typed yet)
    }

    /// `std::some`, `std::none`, `std::ok`, `std::error` written without template arguments: the
    /// alternatives std exports directly, whose arguments come from context (spec 08).
    static bool is_std_export(const PathExpr& pe) {
        const Symbol* s = pe.symbol;
        return s && s->kind == SymbolKind::alternative && pe.path.segments.size() == 2 &&
               pe.path.segments[0].name == "std" && pe.path.segments[1].template_args.empty() && s->parent &&
               s->parent->decl && s->parent->decl->is_template && !s->parent->template_of;
    }

    /// Index of the template parameter that the payload of `alt` is (`some(T)` -> T), or -1.
    static int payload_param(const Symbol* alt) {
        const TypeExpr* p = alt->alternative->payload.get();
        if (!p || p->kind != TypeKind::named) return -1;
        const auto& nt = p->as<NamedType>();
        if (nt.path.segments.size() != 1) return -1;
        const auto& params = alt->parent->decl->template_params;
        for (std::size_t i = 0; i < params.size(); ++i) {
            if (params[i].name == nt.path.segments[0].name) return static_cast<int>(i);
        }
        return -1;
    }

    /// The type of `std::ok(x)` / `std::none`: the instance when the payload fixes every
    /// template argument, otherwise a pending type that context completes later.
    const Type* std_alternative_type(PathExpr& pe, Expr* payload) {
        const Symbol*            alt   = pe.symbol;
        const Symbol*            templ = alt->parent;
        std::vector<const Type*> args(templ->decl->template_params.size(), nullptr);
        if (payload) {
            const Type* at = types::strip_ref(payload->type);
            if (at->is_unknown()) return tc_.error();
            int i = payload_param(alt);
            if (i >= 0 && !at->is_untyped() && !at->is(Kind::never)) args[static_cast<std::size_t>(i)] = at;
        }
        for (const Type* a : args) {
            if (!a) return tc_.pending(templ, args);
        }
        const Type* et = tc_.instance(templ, args);
        pe.symbol      = instance_alternative(pe.symbol, et);
        return et;
    }

    /// A pending `std::ok(x)` / `std::none` meets the type `t` it is used as.
    void settle_pending(Expr& e, const Type* t) {
        const Type* p       = e.type;
        auto&       pe      = e.kind == ExprKind::call ? e.as<CallExpr>().callee->as<PathExpr>() : e.as<PathExpr>();
        Expr*       payload = e.kind == ExprKind::call && e.as<CallExpr>().args.size() == 1
                                  ? e.as<CallExpr>().args[0].get() : nullptr;
        if (t->is_unknown() || t->is(Kind::external)) {
            e.type = pe.type = t;
            if (payload && payload->type && payload->type->is_untyped()) settle(*payload, tc_.error());
            return;
        }
        bool fits = t->is(Kind::user) && t->symbol->template_of == p->symbol &&
                    t->symbol->template_args.size() == p->args.size();
        for (std::size_t i = 0; fits && i < p->args.size(); ++i) {
            fits = !p->args[i] || p->args[i] == t->symbol->template_args[i];
        }
        if (!fits) {
            mismatch(e.span, p, t);
            e.type = pe.type = tc_.error();
            if (payload && payload->type && payload->type->is_untyped()) settle(*payload, tc_.error());
            return;
        }
        pe.symbol = instance_alternative(pe.symbol, t);
        e.type = pe.type = t;
        if (payload && pe.symbol->alternative->payload) {
            coerce(*payload, lower_owned(pe.symbol, *pe.symbol->alternative->payload));
        }
    }

    void no_context_pending(Expr& e, std::string_view where) {
        const Type*  p     = e.type;
        const auto&  tps   = p->symbol->decl->template_params;
        std::string  unknown;
        int          count = 0;
        for (std::size_t i = 0; i < tps.size(); ++i) {
            if (p->args[i]) continue;
            unknown += (unknown.empty() ? "'" : ", '") + tps[i].name + "'";
            ++count;
        }
        const Expr& callee = e.kind == ExprKind::call ? *e.as<CallExpr>().callee : e;
        std::string what   = ast::to_string(callee.as<PathExpr>().path) + (e.kind == ExprKind::call ? "(...)" : "");
        Diagnostic& d = error(DiagCode::E4001, e.span, std::format("'{}' has no type {}", what, where), "type unknown");
        note(d, std::format("the template argument{} {} of 'std::{}' must come from context; write the type, as in "
                            "'std::{}<{}> x = {};'",
                            count == 1 ? "" : "s", unknown, p->symbol->name, p->symbol->name,
                            p->symbol->name == "result" ? "T, E" : "T", what));
        e.type = tc_.error();
    }

    // --- untyped values -------------------------------------------------------------------

    /// Gives an untyped value (literal, `bit(n)`, `default`, or arithmetic on those) the type
    /// `target`, checking that it fits.
    void settle(Expr& e, const Type* target, bool negative = false) {
        const Type* t = types::strip_ref(target);
        if (t->is_untyped()) return;   // still no context: leave it untyped
        if (e.type && e.type->is(Kind::pending) && (e.kind == ExprKind::call || e.kind == ExprKind::path)) {
            settle_pending(e, t);
            return;
        }
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
        if (e.type && e.type->is(Kind::pending)) {
            no_context_pending(e, where);
            return;
        }
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

    /// `{expr:spec}` in an f-string: the value needs a type, and the spec must fit it (spec 19).
    void check_format_hole(FormatPart& part) {
        Expr&       e = *part.expr;
        const Type* t = types::strip_ref(infer(e));
        if (t->is_untyped()) {
            no_context(e, "in a format string");
            return;
        }
        const FormatSpec& s = part.spec;
        if (!s.present || t->is_unknown() || t->is(Kind::external) || t->is(Kind::user)) return;
        auto bad = [&](std::string_view what, std::string_view fix) {
            note(error(DiagCode::E4170, s.span, std::format("{} cannot be used with {}", what, quoted(t)),
                       "does not fit the value"),
                 std::string(fix));
        };
        if (s.radix && !t->is_integer() && !t->is(Kind::char_)) {
            bad(std::format("'{}' (hex/binary/octal)", s.radix), "only integers can be written in another base");
        } else if (s.precision >= 0 && !t->is_float()) {
            bad("a precision", "precision applies to floats only");
        } else if (s.plus && !t->is_numeric()) {
            bad("'+'", "'+' applies to numbers only");
        }
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
            if (!v.init) {
                if (d.kind == DeclKind::field && compile_time_only(lower(*v.type))) {
                    run_time_use(v.type->span, lower(*v.type), std::format("'{}' is a field", v.name),
                                 "values of a struct or class exist at run time");
                }
                return;
            }
            if (!begin_check(&d)) return;
            std::size_t   errors = diags_.error_count();
            FunctionState saved = fn_;
            fn_                 = FunctionState{};
            fn_.is_compile_time = v.storage != Storage::local;   // static/compile_time initializers run at compile time
            infer(*v.init);
            const Type* declared = lower(*v.type);
            coerce(*v.init, declared);
            fn_ = saved;
            if (d.kind == DeclKind::variable && v.storage == Storage::compile_time) {
                if (Symbol* g = symbol_for(d); g && diags_.error_count() == errors) {
                    g->ct_value = evaluate(*v.init, std::format("the value of compile_time constant '{}' is computed at compile time", v.name));
                }
            } else if (compile_time_only(declared)) {
                run_time_use(v.type->span, declared,
                             std::format("'{}' is {}", v.name, d.kind == DeclKind::field ? "a field" : "a static variable"),
                             "declare it 'compile_time'");
            }
            end_check(&d);
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
        if (!begin_check(&fn)) return;   // checked already (on demand, for compile-time code)
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
            if (p.symbol) set_local(p.symbol, lower(*p.type));
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
        end_check(&fn);
        if (!fn.is_compile_time) {
            for (Param& p : fn.params) {
                const Type* pt = p.symbol ? p.symbol->type : nullptr;
                if (pt && compile_time_only(pt)) {
                    run_time_use(p.type->span, pt, std::format("'{}' is a run-time function", fn.name),
                                 "make the function 'compile_time', or pass the type as a template parameter");
                }
            }
            if (fn.result && compile_time_only(lower(*fn.result))) {
                run_time_use(fn.result->span, lower(*fn.result), std::format("'{}' is a run-time function", fn.name),
                             "make the function 'compile_time'");
            }
        }
    }

    // --- compile time (spec 15, 26) -------------------------------------------------------

    /// Types whose values exist only at compile time: `type`, std::meta data, lists of them.
    bool compile_time_only(const Type* t) const {
        t = types::strip_ref(t);
        if (!t) return false;
        if (t->is_compile_time_only()) return true;
        if (t->is(Kind::external)) {
            if (const auto* args = tc_.external_args(t)) {
                for (const Type* a : *args) {
                    if (a && compile_time_only(a)) return true;
                }
            }
        }
        return false;
    }

    void run_time_use(Span span, const Type* t, std::string where, std::string fix) {
        Diagnostic& d = error(DiagCode::E4202, span, std::format("{} exists only at compile time, but {}", quoted(t), where),
                              "compile-time only");
        note(d, std::move(fix));
    }

    ct::Evaluator& evaluator() {
        if (!evaluator_) {
            ct::Hooks h;
            h.check_function = [this](const Symbol* f) { return ensure_function(f); };
            h.constant_value = [this](const Symbol* c) -> std::shared_ptr<const ct::Value> {
                if (c->ct_value) return c->ct_value;
                return c->kind == SymbolKind::global ? ensure_constant(c) : nullptr;
            };
            h.lower      = [this](const TypeExpr& t) { return lower(t); };
            h.depends_on_template = [this](const Symbol* s) { return unknown_values_.count(s) > 0; };
            h.substitute = [this](const Symbol* param) -> const Type* {
                auto it = subst_.find(param);
                return it == subst_.end() ? nullptr : it->second;
            };
            h.visible_fields = [this](const Type* t) {
                std::vector<const Symbol*> out;
                for (const Symbol* f : t->symbol->member_order) {
                    if (f->kind == SymbolKind::field && !f->is_static && member_visible(f)) out.push_back(f);
                }
                return out;
            };
            evaluator_ = std::make_unique<ct::Evaluator>(program_, diags_, std::move(h));
        }
        return *evaluator_;
    }

    /// Evaluates a checked expression at compile time; null when it cannot be (reported, or it
    /// depends on a template parameter, see `evaluator().unknown()`).
    std::shared_ptr<const ct::Value> evaluate(Expr& e, std::string_view why) {
        if (!e.type || e.type->is_unknown()) return nullptr;
        return evaluator().evaluate(e, why);
    }

    /// The type a type value denotes, when the expression is one (`u32`, `point`, `T`).
    const Type* denoted_type(const Expr& e) const {
        if (!e.constant || e.constant->kind != ct::Value::Kind::type) return nullptr;
        return e.constant->type_value;
    }

    void check_main(Module& m) {
        if (!m.source) return;
        for (DeclPtr& d : m.source->ast.decls) {
            if (d->kind != DeclKind::function || d->name != "main") continue;
            auto&       fn = d->as<FunctionDecl>();
            const Type* r  = fn.result ? lower(*fn.result) : tc_.void_();
            bool ok_result = r->is(Kind::void_) || r->is_integer() || r->is_unknown() ||
                             (r->is(Kind::external) && r->name == "std::exit_code") ||
                             (r->is(Kind::user) && r->symbol == std_item("exit_code"));
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
            std::size_t errors   = diags_.error_count();
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
            if (v.symbol) set_local(v.symbol, declared ? declared : tc_.error());
            if (v.storage == Storage::compile_time && v.init && v.symbol && diags_.error_count() == errors) {
                v.symbol->ct_value =
                    evaluate(*v.init, std::format("the value of compile_time constant '{}' is computed at compile time", v.name));
                if (!v.symbol->ct_value && evaluator().unknown()) unknown_values_.insert(v.symbol);
            } else if (declared && compile_time_only(declared) && !fn_.is_compile_time) {
                run_time_use(v.type->span, declared, std::format("'{}' is a run-time variable", v.name),
                             std::format("declare it 'compile_time {} {} = ...;'", types::to_string(declared), v.name));
            }
            return;
        }
        case StmtKind::assign: check_assign(s.as<AssignStmt>()); return;
        case StmtKind::discard: {
            Expr& v = *s.as<DiscardStmt>().value;
            if (infer(v)->is(Kind::pending)) no_context(v, "here ('_ =' needs a value whose type is known)");
            return;
        }
        case StmtKind::expression: {
            Expr&       e = *s.as<ExprStmt>().expr;
            const Type* t = infer(e);
            if (t->is(Kind::pending)) {
                no_context(e, "here");
                return;
            }
            if ((e.kind == ExprKind::call) && (is_std_instance(t, "result") ||
                                                (t->is(Kind::external) && t->external_base() == "std::result"))) {
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
        case ExprKind::call: {   // `f.get(mut value)`: a reference to a field (spec 26)
            const Type* t = e.type;
            if (!t || t->is_unknown()) {
                p.is_place = p.is_mutable = true;   // unknown (the generic check of a template): don't complain
            } else if (t->is(Kind::reference)) {
                p.is_place   = true;
                p.is_mutable = t->is_mut;
                p.name       = "the field";
            }
            return p;
        }
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
                bool negative = negated_literal_;
                negated_literal_ = false;
                if (t && t->is_integer() && !types::literal_fits(lit.int_value, negative, t)) {
                    note(error(DiagCode::E4002, e.span,
                               std::format("{}{} does not fit in {}", negative ? "-" : "", lit.text, quoted(t)),
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
            auto& lit = e.as<LiteralExpr>();
            for (FormatPart& part : lit.format_parts) {
                if (part.expr) check_format_hole(part);
            }
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
            return set(e, infer_index(i));
        }
        case ExprKind::member: return infer_member(e.as<MemberExpr>());
        case ExprKind::try_: return infer_try(e.as<TryExpr>());
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
        if (!s) {
            // `u32` used as a type value (spec 26).
            if (pe.path.segments.size() == 1) {
                if (const Type* b = tc_.builtin(pe.path.segments[0].name)) {
                    pe.constant = std::make_shared<const ct::Value>(ct::Value::of_type(b, tc_.type_value()));
                    return set(pe, tc_.type_value());
                }
            }
            return set(pe, tc_.error());
        }
        if (s->kind == SymbolKind::global) check_static_mut_access(*s, pe.span);
        if (s->is_type()) {   // a type used as a value: `point`, `T`, `box<u32>` (spec 26)
            pe.constant = evaluate(pe, "");
            return set(pe, tc_.type_value());
        }
        switch (s->kind) {
        case SymbolKind::local:
        case SymbolKind::global: {
            const Type* t = type_of_symbol(s);
            if (s->kind == SymbolKind::global && s->decl && s->decl->kind == DeclKind::variable &&
                s->decl->as<VariableDecl>().storage == Storage::compile_time) {
                pe.constant = s->ct_value ? s->ct_value : ensure_constant(s);
            } else if (s->ct_value) {
                pe.constant = s->ct_value;   // a compile_time local or a `for compile_time` loop variable
            }
            return set(pe, t);
        }
        case SymbolKind::alternative: {
            if (is_std_export(pe)) return set(pe, std_alternative_type(pe, nullptr));
            const Type* et = enum_type_of(s, pe.path);
            pe.symbol      = instance_alternative(pe.symbol, et);
            return set(pe, et);
        }
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
            const Type* first = is_method ? l : lower_owned(s, *f->params[0].type);
            const Type* second = lower_owned(s, *f->params[want - 1].type);
            if (!is_method && assign_cost(l, first) < 0) return nullptr;
            if (r && assign_cost(r, second) < 0) return nullptr;
            if (f->is_defaulted && op == "==") return tc_.bool_();
            return f->result ? lower_owned(s, *f->result) : tc_.void_();
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
        // `-128i8`: a suffixed literal under '-' is range-checked with its sign.
        bool negated_literal = u.op == UnaryOp::negate && u.operand->kind == ExprKind::int_literal &&
                               !u.operand->as<LiteralExpr>().suffix.empty();
        negated_literal_      = negated_literal;
        const Type* t         = infer(*u.operand);
        negated_literal_      = false;
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
        } else if (t->is(Kind::meta)) {
            return infer_meta_member(m, t, name);
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
        return set(m, lower_owned(mem, *mem->decl->as<VariableDecl>().type));
    }

    // --- reflection (spec 26) -------------------------------------------------------------

    /// `std::is_integer<T>()`, `std::size_of<T>()`, `std::meta::fields<T>()`, ...: compile-time
    /// functions of the compiler. Their value is computed here, so run-time code gets a constant.
    const Type* infer_builtin(CallExpr& c, const std::string& name) {
        auto&       seg   = c.callee->as<PathExpr>().path.segments.back();
        std::size_t wants = name == "is_same" ? 2 : 1;
        if (seg.template_args.size() != wants || !c.args.empty()) {
            error(DiagCode::E4030, c.span,
                  std::format("'{}' takes {} template argument{} and no arguments: '{}<{}>()'", ast::to_string(c.callee->as<PathExpr>().path),
                              wants, wants == 1 ? "" : "s", ast::to_string(c.callee->as<PathExpr>().path), wants == 1 ? "T" : "A, B"),
                  "wrong arguments");
            return set(c, tc_.error());
        }
        for (const TemplateArg& a : seg.template_args) {
            if (a.type) lower(*a.type);
        }
        const Type* t = name == "size_of" || name == "align_of"   ? tc_.builtin("usize")
                      : name == "type_name"                       ? tc_.static_str()
                      : name == "meta::fields"                    ? list_of(tc_.meta_field())
                      : name == "meta::alternatives"              ? list_of(tc_.meta_alternative())
                                                                  : tc_.bool_();
        set(c, t);
        c.constant = evaluate(c, "");
        return t;
    }

    /// `std::span<T>` of compile-time data: the result of `std::meta::fields<T>()`.
    const Type* list_of(const Type* element) {
        std::string spelling = "std::span<" + types::to_string(element) + ">";
        const Type* t        = tc_.external(spelling);
        if (!tc_.external_args(t)) tc_.set_external_args(t, {element});
        return t;
    }

    /// `f.name`, `a.payload`, ...: the parts of std::meta::field / std::meta::alternative.
    const Type* infer_meta_member(MemberExpr& m, const Type* t, const std::string& name) {
        bool        field = t == tc_.meta_field();
        const Type* r     = name == "name"                                  ? tc_.static_str()
                          : name == "index" || (field && name == "offset") ? tc_.builtin("usize")
                          : field && name == "type"                         ? tc_.type_value()
                          : !field && name == "payload"                     ? tc_.type_value()
                          : !field && name == "has_payload"                 ? tc_.bool_()
                                                                            : nullptr;
        if (!r) {
            error(DiagCode::E4070, m.name.span, std::format("{} has no field '{}'", quoted(t), name), "unknown member");
            return set(m, tc_.error());
        }
        set(m, r);
        if (m.object->constant) m.constant = evaluate(m, "");
        return r;
    }

    /// `f.get(value)`, `a.is(value)`, `a.get(value)`, `a.make(...)` (spec 26). The handle must be
    /// a compile-time value: what the call does depends on which field or alternative it is.
    const Type* infer_meta_method(CallExpr& c, MemberExpr& me, const Type* t, const std::string& name, std::vector<Expr*>& args) {
        c.callee->type = tc_.error();
        bool field     = t == tc_.meta_field();
        bool known     = field ? name == "get" : name == "is" || name == "get" || name == "make";
        if (!known) {
            error(DiagCode::E4070, me.name.span, std::format("{} has no method '{}'", quoted(t), name), "unknown method");
            return set(c, tc_.error());
        }
        const ct::Value* handle = me.object->constant.get();
        if (!handle) {
            if (evaluator().unknown() || types::strip_ref(me.object->type)->is_unknown()) return set(c, tc_.error());
            Diagnostic& d = error(DiagCode::E4200, me.object->span,
                                  std::format("'.{}' needs a {} that is known at compile time", name, quoted(t)),
                                  "not known at compile time");
            note(d, "use the loop variable of a 'for compile_time' loop, or a 'compile_time' constant");
            return set(c, tc_.error());
        }
        const Type* owner = handle->type_value;
        if (name == "make") {
            const Symbol* alt = handle->symbol;
            if (alt->alternative->payload) {
                if (args.size() != 1) {
                    error(DiagCode::E4030, c.span, std::format("'{}' carries one value: 'a.make(value)'", alt->name), "wrong number of values");
                    return set(c, owner);
                }
                coerce(*args[0], lower_owned(alt, *alt->alternative->payload));
            } else if (!args.empty()) {
                error(DiagCode::E4030, c.span, std::format("'{}' carries no value: 'a.make()'", alt->name), "wrong number of values");
            }
            return set(c, owner);
        }
        if (args.size() != 1) {
            error(DiagCode::E4030, c.span, std::format("'{}' takes the value as its one argument", name), "wrong number of arguments");
            return set(c, tc_.error());
        }
        Expr& arg    = *args[0];
        bool  marked = arg.kind == ExprKind::unary && arg.as<UnaryExpr>().op == UnaryOp::mut_marker;
        const Type* at = types::strip_ref(arg.type);
        if (at->is_untyped()) settle(arg, owner);
        else if (!at->is_unknown() && at != owner) mismatch(arg.span, arg.type, owner, " (the reflected value)");
        if (field) {
            if (marked) require_mutable(place_of(*arg.as<UnaryExpr>().operand), *arg.as<UnaryExpr>().operand, "change a field of");
            const Symbol* f = handle->symbol;
            return set(c, tc_.reference(lower_owned(f, *f->decl->as<VariableDecl>().type), marked));
        }
        if (name == "is") return set(c, tc_.bool_());
        const Symbol* alt = handle->symbol;   // a.get(value): std::option<P&>
        if (!alt->alternative->payload) {
            error(DiagCode::E4070, me.name.span, std::format("'{}' has no payload; use 'a.is(value)'", alt->name), "no payload");
            return set(c, tc_.error());
        }
        const Symbol* option = std_item("option");
        if (!option) return set(c, tc_.error());
        return set(c, tc_.instance(option, {tc_.reference(lower_owned(alt, *alt->alternative->payload), false)}));
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
        // A parameter that only the body uses (`std::is_integer<T>()`): its own symbol.
        for (std::size_t i = 0; i < out.size(); ++i) {
            if (!out[i] && i < f.template_param_symbols.size()) out[i] = f.template_param_symbols[i];
        }
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
        // A member of a template instance uses its parameters (spec 14); a function template's are deduced.
        const std::map<const Symbol*, const Type*>* owner = f.is_template ? nullptr : substitution_of(s);
        for (const Param& p : f.params) c.params.push_back(owner ? tc_.lower(*p.type, owner) : lower(*p.type, &c.subst));
        c.result = f.result ? (f.result->kind == TypeKind::deduce ? tc_.error()
                               : owner ? tc_.lower(*f.result, owner) : lower(*f.result, &c.subst))
                            : tc_.void_();
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

    /// The function a call to `chosen` runs (spec 14): for a function template, its instance for the
    /// deduced arguments. Arguments that are still unknown (inside a template that is not instantiated)
    /// keep the template itself. Null, after reporting, when a parameter could not be deduced.
    Symbol* call_target(const Candidate& chosen, Span span, std::string_view display) {
        if (!chosen.decl->is_template) return chosen.symbol;
        std::vector<const Symbol*> syms = template_symbols(*chosen.decl);
        std::vector<const Type*>   args;
        for (const Symbol* p : syms) {
            auto it = p ? chosen.subst.find(p) : chosen.subst.end();
            if (it == chosen.subst.end()) {
                std::string name = p ? p->name : std::string("?");
                error(DiagCode::E4160, span, std::format("cannot deduce template parameter '{}' of '{}'", name, display),
                      "cannot deduce");
                return nullptr;
            }
            args.push_back(it->second);
        }
        for (const Type* a : args) {
            if (a->is_unknown()) return chosen.symbol;
        }
        return instantiate_function(chosen.symbol, args);
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
            if (std::string b = ct::builtin_name(c); !b.empty()) return infer_builtin(c, b);
            if (s && s->is_type()) {
                Diagnostic& d = error(DiagCode::E3022, callee.span,
                                      std::format("'{}' is a {}, not a function", ast::to_string(pe.path), sema::to_string(s->kind)),
                                      "not a function");
                if (s->kind == SymbolKind::struct_ || s->kind == SymbolKind::class_) {
                    note(d, std::format("construct a value with '{}{{ ... }}'", ast::to_string(pe.path)));
                }
                return set(c, tc_.error());
            }
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
            if (s->kind == SymbolKind::alternative && is_std_export(pe)) {
                if (!s->alternative->payload || args.size() != 1) {
                    error(DiagCode::E4030, c.span,
                          s->alternative->payload
                              ? std::format("'{}' carries exactly one value, but {} are given", ast::to_string(pe.path), args.size())
                              : std::format("'{}' carries no value", ast::to_string(pe.path)),
                          "wrong number of values");
                    for (Expr* a : args) {
                        if (a->type && a->type->is_untyped()) settle(*a, tc_.error());
                    }
                    return set(c, tc_.error());
                }
                const Type* t = std_alternative_type(pe, args[0]);
                callee.type   = t;
                if (t->is(Kind::user)) check_argument(*args[0], lower_owned(pe.symbol, *pe.symbol->alternative->payload));
                return set(c, t);
            }
            if (s->kind == SymbolKind::alternative) {
                const Type* et      = enum_type_of(s, pe.path);
                Symbol*     alt     = instance_alternative(pe.symbol, et);
                pe.symbol           = alt;
                const Type* payload = alt->alternative->payload ? lower_owned(alt, *alt->alternative->payload) : nullptr;
                if (payload) {
                    if (args.size() != 1) {
                        error(DiagCode::E4030, c.span,
                              std::format("'{}' carries exactly one value, but {} are given", ast::to_string(pe.path), args.size()),
                              "wrong number of values");
                    } else {
                        check_argument(*args[0], payload);
                    }
                }
                callee.type = et;
                return set(c, et);
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
                Symbol* target = call_target(*chosen, c.span, ast::to_string(pe.path));
                if (!target) return set(c, tc_.error());
                pe.symbol   = target;
                callee.type = nullptr;   // functions are not values
                check_compile_time_call(target, c.span);
                check_unsafe_call(target, c.span);
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
            if (t->is(Kind::pending)) {
                no_context(*me.object, std::format("before '.{}'", name));
                for (Expr* a : args) {
                    if (a->type && a->type->is_untyped()) settle(*a, tc_.error());
                }
                return set(c, tc_.error());
            }
            if (types::is_std_builtin_method(t, name)) return infer_std_method(c, t, name, args);
            if (t->is(Kind::meta)) return infer_meta_method(c, me, t, name, args);
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
                if (types::is_mutating_library_method(t, name)) {
                    Place p = place_of(*me.object);
                    if (p.is_place && !p.is_mutable && !p.is_temporary) {
                        if (p.via_this) {
                            note(error(DiagCode::E4021, c.span,
                                       std::format("'{}' changes the container; 'this' is immutable in a non-mut method", name),
                                       "'this' is immutable here"),
                                 std::format("add 'mut' after the parameter list: '{}(...) mut'", fn_.name));
                        } else {
                            Diagnostic& d = error(DiagCode::E4022, c.span,
                                                  std::format("'{}' changes {}, but '{}' is not mutable", name, quoted(t), p.name),
                                                  "immutable receiver");
                            if (p.local) note(d, std::format("declare it 'mut {} {}'", types::to_string(type_of_symbol(p.local)),
                                                             p.local->name),
                                              p.local->span);
                        }
                    }
                }
                for (Expr* a : args) {
                    if (a->type && a->type->is_untyped() && a->type != tc_.untyped_default()) settle(*a, tc_.error());
                }
                // A few string methods are known before the library exists (the backend implements them).
                if (args.empty() && (t->is(Kind::static_str) || (t->is(Kind::external) && t->name == "std::str"))) {
                    if (name == "len") return set(c, tc_.builtin("usize"));
                    if (name == "is_empty") return set(c, tc_.bool_());
                }
                // Lists of compile-time data (`std::meta::fields<T>().len()`).
                if (args.empty() && t->is(Kind::external) && t->external_base() == "std::span" && compile_time_only(t)) {
                    const Type* r = name == "len" ? tc_.builtin("usize") : name == "is_empty" ? tc_.bool_() : nullptr;
                    if (r) {
                        set(c, r);
                        if (me.object->constant) c.constant = evaluate(c, "");
                        return r;
                    }
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
            if (methods.empty() && name == "clone" && args.empty() &&
                (t->is_user(SymbolKind::struct_) || t->is_user(SymbolKind::enum_))) {
                return set(c, t);   // provided automatically (spec 05); cloneability is checked in M5
            }
            if (methods.empty()) {
                bool    opaque = false;
                Symbol* other  = find_member(t->symbol, name, opaque);
                if (other) {
                    error(DiagCode::E4033, me.name.span, std::format("'{}' is a field, not a method", name), "not callable");
                } else if (!opaque) {
                    Diagnostic& d = error(DiagCode::E4070, me.name.span, std::format("{} has no method '{}'", quoted(t), name),
                                          "unknown method");
                    if (name == "clone" && t->is_user(SymbolKind::class_)) {
                        note(d, "classes implement 'clone()' themselves (interface std::cloneable); structs get it automatically");
                    }
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
        if (v == "checked") {
            const Symbol* option = std_item("option");
            set(c, option ? tc_.instance(option, {to}) : tc_.external("std::option<" + types::to_string(to) + ">"));
        } else if (v == "dynamic") {
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
                const Type* tag = lower_owned(from->symbol, *from->symbol->decl->as<EnumDecl>().tag_type);
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
        if (is_std_instance(s, "option")) return;   // std::none (spec 03)
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
            if (field && field->kind == SymbolKind::field) coerce(*f.value, lower_owned(field, *field->decl->as<VariableDecl>().type));
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
                if (cap.symbol) set_local(cap.symbol, t);
            } else if (cap.symbol) {
                // Same type as the captured local; reference captures are references.
                const Type* outer = tc_.error();
                for (const auto& [sym, ty] : local_types_) {
                    if (sym->name == cap.name && sym != cap.symbol && sym->span.begin < cap.span.begin) outer = ty;
                }
                bool ref = cap.kind == CaptureKind::ref || cap.kind == CaptureKind::mut_ref;
                set_local(cap.symbol, ref ? tc_.reference(types::strip_ref(outer), cap.kind == CaptureKind::mut_ref)
                                               : types::strip_ref(outer));
            }
        }
        for (Param& p : l.params) {
            if (p.symbol) set_local(p.symbol, lower(*p.type));
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
            if (rs->is(Kind::pending) && ts->is(Kind::pending) && rs->symbol == ts->symbol) {
                result = merge_pending(rs, ts);   // `if (c) { std::ok(1u32) } else { std::error(e) }`
                if (result->is(Kind::error)) return result;
                continue;
            }
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

    /// `if compile_time (c)` (spec 15): the branch that is not taken is not checked. Nullopt when
    /// the condition depends on a template parameter (the generic check): then both are checked.
    std::optional<const Type*> infer_if_compile_time(IfExpr& i) {
        const Type* ct = types::strip_ref(infer(*i.condition));
        if (ct->is_untyped()) settle(*i.condition, tc_.bool_());
        else if (!ct->is(Kind::bool_) && !ct->is_unknown()) mismatch(i.condition->span, i.condition->type, tc_.bool_(), " (the condition)");
        std::shared_ptr<const ct::Value> v = evaluate(*i.condition, "the condition of 'if compile_time' must be known at compile time");
        if (!v || v->kind != ct::Value::Kind::boolean) {
            if (!v && !evaluator().unknown() && !ct->is_unknown()) {
                set(i, tc_.error());
                return tc_.error();   // reported
            }
            return std::nullopt;
        }
        if (v->boolean) {
            i.taken = 0;
            return set(i, i.else_branch ? infer_block(*i.then_block) : (infer_block(*i.then_block), tc_.void_()));
        }
        if (!i.else_branch) {
            i.taken = 2;
            return set(i, tc_.void_());
        }
        i.taken = 1;
        return set(i, infer(*i.else_branch));
    }

    /// Two pending values of the same template: the arguments either knows. Error on a conflict.
    const Type* merge_pending(const Type* a, const Type* b) {
        std::vector<const Type*> args = a->args;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (!args[i]) args[i] = b->args[i];
            else if (b->args[i] && b->args[i] != args[i]) return tc_.error();
        }
        for (const Type* x : args) {
            if (!x) return tc_.pending(a->symbol, args);
        }
        return tc_.instance(a->symbol, args);
    }

    const Type* infer_if(IfExpr& i) {
        if (i.is_compile_time) {
            if (std::optional<const Type*> t = infer_if_compile_time(i)) return *t;
        }
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

    /// `c[i]` on the std containers: sequences take a 'usize' index, maps their key type.
    /// Everything else is a library type the checker does not know (unknown, never an error).
    const Type* infer_index(IndexExpr& i) {
        const Type* obj = types::strip_ref(infer(*i.object));
        const Type* idx = infer(*i.index);
        const Type* key = nullptr;
        const Type* el  = tc_.error();
        if (obj->is(Kind::external)) {
            std::string_view                 base = obj->external_base();
            const std::vector<const Type*>* args = tc_.external_args(obj);
            if (args && !args->empty() && (base == "std::darray" || base == "std::array" || base == "std::span")) {
                key = tc_.builtin("usize");
                el  = (*args)[0];
            } else if (args && args->size() >= 2 && (base == "std::hash_map" || base == "std::tree_map")) {
                key = (*args)[0];
                el  = (*args)[1];
            }
        }
        if (!key) {
            if (idx->is_untyped()) settle(*i.index, tc_.error());
            return tc_.error();
        }
        if (idx->is_untyped()) {
            settle(*i.index, key);
        } else if (!idx->is_unknown() && !key->is_unknown() && types::strip_ref(idx) != key) {
            mismatch(i.index->span, idx, key, " (the index)");
        }
        return el;
    }

    const Type* infer_for(ForExpr& f) {
        if (f.is_compile_time) {
            if (infer_for_compile_time(f)) return set(f, tc_.void_());
        }
        const Type* it   = infer(*f.iterable);
        const Type* var  = lower(*f.type);
        const Type* sit  = types::strip_ref(it);
        if (f.symbol) set_local(f.symbol, var);
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

    /// `for compile_time` (spec 26): the body is copied once per element and each copy is
    /// checked with the loop variable as a compile-time constant. False when the elements depend
    /// on a template parameter (the generic check): then the body is checked once as usual.
    bool infer_for_compile_time(ForExpr& f) {
        std::size_t errors = diags_.error_count();
        const Type* it     = infer(*f.iterable);
        if (diags_.error_count() != errors) return true;   // reported (e.g. std::meta::fields of a non-struct)
        const Type* var = lower(*f.type);
        if (f.iterable->kind == ExprKind::range) {
            auto& r = f.iterable->as<RangeExpr>();
            if (r.lo->type && r.lo->type->is_untyped()) settle(*r.lo, var);
            if (r.hi->type && r.hi->type->is_untyped()) settle(*r.hi, var);
            f.iterable->type = tc_.range(types::strip_ref(var));
        } else if (types::strip_ref(it)->is_unknown()) {
            if (f.symbol) unknown_values_.insert(f.symbol);
            return false;
        }
        std::optional<std::vector<ct::Value>> items = evaluator().elements(*f.iterable);
        if (!items) {
            if (evaluator().unknown()) {
                if (f.symbol) unknown_values_.insert(f.symbol);   // the generic check of a template
                return false;
            }
            return true;   // reported
        }
        for (const ct::Value& item : *items) {
            if (item.type && !var->is_unknown() && types::strip_ref(item.type) != types::strip_ref(var)) {
                mismatch(f.type->span, var, item.type, " (the loop's elements)");
                return true;
            }
        }
        Module* home = module_;
        f.unrolled   = true;
        for (const ct::Value& item : *items) {
            Cloner cloner([home](const Symbol& original) {
                Symbol* copy = home->make(original.kind, original.name, original.span);
                Module* owner = copy->module;
                *copy         = original;
                copy->module  = owner;
                copy->ct_value.reset();
                copy->type = nullptr;
                return copy;
            });
            Symbol* loop_var = home->make(SymbolKind::local, f.name, f.name_span);
            if (f.symbol) {
                Module* owner = loop_var->module;
                *loop_var     = *f.symbol;
                loop_var->module = owner;
                cloner.map(f.symbol, loop_var);
            }
            loop_var->ct_value = std::make_shared<const ct::Value>(item);
            set_local(loop_var, var);
            f.copies.push_back(cloner.block(*f.body));
            f.copy_symbols.push_back(loop_var);
            infer_block(*f.copies.back());
        }
        return true;
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
        const std::vector<const Type*>* found = tc_.external_args(s);
        if (!found) return tc_.error();
        const auto& args = *found;
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
                set_local(b.symbol, s);
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
                ep.symbol      = instance_alternative(ep.symbol, s);
                alt            = ep.symbol;
                const Type* et = tc_.user(alt->parent);
                if (!s->is_unknown() && s != et) {
                    error(DiagCode::E4061, ep.path.span,
                          std::format("'{}' is an alternative of {}, but the match subject is {}", ast::to_string(ep.path),
                                      quoted(et), quoted(s)),
                          "wrong enum");
                }
                if (alt->alternative->payload) payload = lower_owned(alt, *alt->alternative->payload);
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
                    if (field && field->kind == SymbolKind::field) ft = lower_owned(field, *field->decl->as<VariableDecl>().type);
                }
                check_pattern(*f.pattern, ft, mut_ref);
            }
            (void)it;
            return;
        }
        }
    }

    /// Matches every value: `_`, a binding, or a struct pattern made only of such patterns.
    static bool irrefutable(const Pattern& p) {
        if (p.kind == PatternKind::wildcard || p.kind == PatternKind::binding) return true;
        if (p.kind != PatternKind::struct_) return false;
        const auto& sp = p.as<StructPattern>();
        return std::all_of(sp.fields.begin(), sp.fields.end(),
                           [](const FieldPattern& f) { return !f.pattern || irrefutable(*f.pattern); });
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
            // option and result: their alternatives are written `std::ok` (spec 08).
            prefix = is_std_instance(s, "option") || is_std_instance(s, "result") ? "std::" : s->symbol->name + "::";
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
                bool full_payload = !ep.payload || irrefutable(*ep.payload);
                if (full_payload) seen.insert(ep.path.segments.back().name);
            } else if (p.kind == PatternKind::literal && s->is(Kind::bool_)) {
                const Expr& lit = *p.as<LiteralPattern>().literal;
                if (lit.kind == ExprKind::bool_literal) seen.insert(lit.as<LiteralExpr>().bool_value ? "true" : "false");
            } else if (p.kind == PatternKind::struct_) {
                if (irrefutable(p)) return;   // a struct pattern with only bindings matches everything
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
    bool                                                negated_literal_ = false;   // the next int literal sits under a unary minus
    std::map<const Symbol*, const Type*>                local_types_;
    std::set<const Expr*>                               branch_conflicts_;
    std::map<const Symbol*, const Type*>                subst_;   // template parameters of the instance being checked
    Instantiator*                                       instantiator_ = nullptr;   // makes function instances
    std::unique_ptr<ct::Evaluator>                      evaluator_;   // compile-time evaluation (spec 15, 26)
    std::set<const Symbol*>                             unknown_values_;   // compile-time values that depend on a template parameter

    // On-demand checking for compile-time code; defined after Instantiator, which keeps the state.
    bool begin_check(const Decl* d);
    void end_check(const Decl* d);
    bool ensure_function(const Symbol* s);
    std::shared_ptr<const ct::Value> ensure_constant(const Symbol* g);

    /// The instance of the function template `templ` for `args` (spec 14), defined after Instantiator.
    Symbol* instantiate_function(const Symbol* templ, const std::vector<const Type*>& args);
};

/// Makes the instances of template types (spec 14). An instance is a copy of the template's
/// declaration, re-parsed from its source text so that it has an AST of its own; it is resolved
/// in the template's module and checked with the parameters replaced by the arguments. Instances
/// are created on demand, wherever a type `box<u32>` is lowered.
class Instantiator {
public:
    Instantiator(sema::Program& program, const SourceManager& sources, DiagnosticEngine& diags)
        : program_(program), sources_(sources), diags_(diags) {}

    const Type* instantiate(const Symbol* templ, const std::vector<const Type*>& args) {
        auto key = std::make_pair(templ, args);
        if (auto it = memo_.find(key); it != memo_.end()) return it->second;
        Decl* copy = copy_of(*templ);
        if (!copy) return program_.types->error();
        std::vector<Symbol*> params;
        Symbol*              inst = resolve_instance(program_, diags_, templ, *copy, params);
        if (params.size() != args.size()) return program_.types->error();
        inst->template_of    = templ;
        inst->instance_index = ++count_[templ];
        for (std::size_t i = 0; i < params.size(); ++i) inst->template_subst[params[i]] = args[i];
        inst->template_args = args;
        const Type* t = program_.types->user(inst);
        memo_[key]    = t;
        program_.instances.push_back(inst);
        program_.instance_copies.push_back(copy);
        Checker(program_, diags_, this).check_instance(inst, *copy);
        return t;
    }

    /// The instance of the function template `templ` for `args`, made once per argument list.
    Symbol* instantiate_function(const Symbol* templ, const std::vector<const Type*>& args) {
        auto key = std::make_pair(templ, args);
        if (auto it = fn_memo_.find(key); it != fn_memo_.end()) return it->second;
        Decl* copy = copy_of(*templ);
        if (!copy || copy->kind != DeclKind::function) return nullptr;
        std::vector<Symbol*> params;
        Symbol*              inst = resolve_function_instance(program_, diags_, templ, copy->as<FunctionDecl>(), params);
        if (params.size() != args.size()) return nullptr;
        inst->template_of    = templ;
        inst->instance_index = ++count_[templ];
        for (std::size_t i = 0; i < params.size(); ++i) inst->template_subst[params[i]] = args[i];
        inst->template_args = args;
        fn_memo_[key] = inst;   // before checking the body, so that recursive calls find it
        program_.instances.push_back(inst);
        program_.instance_copies.push_back(copy);
        Checker(program_, diags_, this).check_instance(inst, *copy);
        return inst;
    }

private:
    /// A fresh copy of the template's declaration, re-parsed from its source text, so that the
    /// instance has an AST of its own. It lives as long as the program: symbols refer to it.
    /// Null when the source does not hold exactly one declaration.
    Decl* copy_of(const Symbol& templ) {
        const Decl& src           = templ.definition ? *templ.definition : *templ.decl;
        const SourceFile& file    = sources_.file(src.span.file);
        std::vector<Token> tokens = tokenize_range(file, src.span.file, diags_, src.span.begin, src.span.end);
        program_.instance_files.push_back(parse(file, src.span.file, tokens, diags_));
        ast::File& parsed = program_.instance_files.back();
        if (parsed.decls.size() != 1) return nullptr;
        return parsed.decls.front().get();
    }

public:
    /// Which declarations are being checked or are done (functions and compile_time constants
    /// can be checked early, on demand, when compile-time code needs them).
    enum class State { checking, done };
    std::map<const Decl*, State> states;

private:
    sema::Program&                                                        program_;
    const SourceManager&                                                  sources_;
    DiagnosticEngine&                                                     diags_;
    std::map<std::pair<const Symbol*, std::vector<const Type*>>, const Type*> memo_;
    std::map<std::pair<const Symbol*, std::vector<const Type*>>, Symbol*>     fn_memo_;
    std::map<const Symbol*, unsigned>                                     count_;
};

bool Checker::begin_check(const Decl* d) {
    if (!instantiator_) return true;
    if (instantiator_->states.count(d)) return false;
    instantiator_->states[d] = Instantiator::State::checking;
    return true;
}

void Checker::end_check(const Decl* d) {
    if (instantiator_) instantiator_->states[d] = Instantiator::State::done;
}

bool Checker::ensure_function(const Symbol* s) {
    const Decl* d = s->definition ? s->definition : s->decl;
    if (!instantiator_ || !d) return false;
    auto it = instantiator_->states.find(d);
    if (it != instantiator_->states.end()) return it->second == Instantiator::State::done;
    Checker(program_, diags_, instantiator_).check_function_symbol(s);
    it = instantiator_->states.find(d);
    return it != instantiator_->states.end() && it->second == Instantiator::State::done;
}

std::shared_ptr<const ct::Value> Checker::ensure_constant(const Symbol* g) {
    if (g->ct_value || !instantiator_ || !g->decl) return g->ct_value;
    if (instantiator_->states.count(g->decl)) return g->ct_value;   // in progress (a cycle) or done without a value
    Checker(program_, diags_, instantiator_).check_global_symbol(g);
    return g->ct_value;
}

// Null only when the template's source did not parse (already reported) or no instantiator is set.
Symbol* Checker::instantiate_function(const Symbol* templ, const std::vector<const Type*>& args) {
    return instantiator_ ? instantiator_->instantiate_function(templ, args) : nullptr;
}

} // namespace

void check_types(sema::Program& program, const SourceManager& sources, DiagnosticEngine& diags) {
    if (!program.types) program.types = std::make_shared<types::TypeContext>();
    Instantiator instantiator(program, sources, diags);
    program.types->set_instantiator(
        [&](const Symbol* templ, const std::vector<const Type*>& args) { return instantiator.instantiate(templ, args); });
    // A compile_time `type` constant used as a type (spec 26): evaluated on demand while checking.
    program.types->set_type_value_resolver([&](const Symbol* s) -> const Type* {
        if (!s->ct_value && s->kind == SymbolKind::global && s->decl && !instantiator.states.count(s->decl)) {
            Checker(program, diags, &instantiator).check_global_symbol(s);
        }
        if (!s->ct_value || s->ct_value->kind != ct::Value::Kind::type) return nullptr;
        return s->ct_value->type_value;
    });
    Checker(program, diags, &instantiator).run();
    // Instances are only made while checking. Later phases see the ones that exist.
    program.types->set_instantiator(nullptr);
    program.types->set_type_value_resolver([](const Symbol* s) -> const Type* {
        if (!s->ct_value || s->ct_value->kind != ct::Value::Kind::type) return nullptr;
        return s->ct_value->type_value;
    });
}

} // namespace rift

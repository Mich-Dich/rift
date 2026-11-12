#include "rift/ownership.hpp"

#include "rift/ast_printer.hpp"
#include "rift/types.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <set>

namespace rift {

using namespace ast;
using sema::LocalKind;
using sema::Module;
using sema::Symbol;
using sema::SymbolKind;
using types::Kind;
using types::Type;

namespace {

enum class Copy { yes, no, unknown };

std::string quoted(const Type* t) { return "'" + types::to_string(t) + "'"; }

/// Moved locals at one program point. Unreachable states (after return/break) merge as identity.
struct State {
    bool                           reachable = true;
    std::map<const Symbol*, Span>  moved;   // local -> where it was moved

    static State unreachable() {
        State s;
        s.reachable = false;
        return s;
    }
    bool operator==(const State& o) const {
        if (reachable != o.reachable) return false;
        if (moved.size() != o.moved.size()) return false;
        for (const auto& [sym, span] : moved) {
            if (!o.moved.count(sym)) return false;
        }
        return true;
    }
};

State merge(const State& a, const State& b) {
    if (!a.reachable) return b;
    if (!b.reachable) return a;
    State out = a;
    for (const auto& [sym, span] : b.moved) out.moved.emplace(sym, span);
    return out;
}

struct LoopContext {
    State         breaks    = State::unreachable();
    State         continues = State::unreachable();
    const Symbol* borrowed  = nullptr;   // container the loop iterates by reference
    bool          borrow_mut = false;
    Span          span;
    std::string   var;
};

/// A local reference `T& r = x...;` keeps `x` borrowed until r's last use.
struct Borrow {
    const Symbol* ref;
    const Symbol* root;
    Span          decl;
    std::uint32_t live_end;
};

class Ownership {
public:
    Ownership(sema::Program& program, DiagnosticEngine& diags)
        : program_(program), diags_(diags), tc_(*program.types) {}

    void run() {
        for (auto& [name, m] : program_.modules) {
            module_ = m.get();
            check_types_and_signatures(*m);
            if (m->header) walk_file(m->header->ast);
            if (m->source) walk_file(m->source->ast);
        }
        for (std::size_t i = 0; i < program_.instances.size(); ++i) {
            walk_instance(*program_.instances[i], *program_.instance_copies[i]);
        }
    }

    /// The members of a template instance (spec 14), walked with its parameters substituted.
    void walk_instance(Symbol& inst, ast::Decl& copy) {
        if (copy.kind == DeclKind::enum_) return;
        module_    = inst.module;
        cur_subst_ = &inst.template_subst;
        if (copy.kind == DeclKind::function) {
            walk_function(copy.as<FunctionDecl>());
        } else {
            for (DeclPtr& m : copy.as<TypeDecl>().members) walk_decl(*m);
        }
        cur_subst_ = nullptr;
    }

    /// Lowers a type written in the code being checked (`owner` null) or in the declaration of
    /// `owner`, which may belong to a template instance (spec 14).
    const Type* lower(const TypeExpr& t, const Symbol* owner = nullptr) {
        return tc_.lower(t, owner ? substitution_of(owner) : cur_subst_);
    }

private:
    // --- diagnostics ----------------------------------------------------------------------

    Diagnostic& error(DiagCode code, Span span, std::string message, std::string label = {}) {
        if (silent_ > 0) {
            scratch_ = Diagnostic{code, Severity::error, {}, span, {}, {}};
            return scratch_;
        }
        return diags_.report(code, span, std::move(message), std::move(label));
    }
    static void note(Diagnostic& d, std::string message, std::optional<Span> span = std::nullopt) {
        d.notes.push_back(Note{std::move(message), span});
    }

    // --- classification -------------------------------------------------------------------

    Copy copy_kind(const Type* t) {
        if (!t) return Copy::unknown;
        switch (t->kind) {
        case Kind::error:
        case Kind::type_param:
        case Kind::untyped_int:
        case Kind::untyped_float:
        case Kind::untyped_default: return Copy::unknown;
        case Kind::user: return user_copy(t->symbol);
        case Kind::external: return external_copy(t);
        default: return Copy::yes;   // built-ins, references, raw pointers, ranges, string literals
        }
    }

    Copy combine(Copy a, Copy b) {
        if (a == Copy::no || b == Copy::no) return Copy::no;
        if (a == Copy::unknown || b == Copy::unknown) return Copy::unknown;
        return Copy::yes;
    }

    Copy external_copy(const Type* t) {
        static const std::set<std::string_view> copy = {"std::str",       "std::span",     "std::format_args",
                                                        "std::exit_code", "std::ordering", "std::sort_order",
                                                        "std::weak_ptr_never"};
        static const std::set<std::string_view> owning = {
            "std::string",   "std::darray", "std::unique_ptr", "std::shared_ptr", "std::weak_ptr",
            "std::file",     "std::hash_map", "std::tree_map", "std::function",  "std::mutex",
            "std::atomic",   "std::filesystem::path"};
        std::string_view base = t->external_base();
        if (base.starts_with("std::exit_code") || base.starts_with("std::ordering")) return Copy::yes;
        if (copy.count(base)) return Copy::yes;
        if (owning.count(base)) return Copy::no;
        if (base == "std::result" || base == "std::option" || base == "std::array") {
            const std::vector<const Type*>* args = tc_.external_args(t);
            if (!args) return Copy::unknown;
            Copy c = Copy::yes;
            for (const Type* a : *args) c = combine(c, copy_kind(a));
            return c;
        }
        return Copy::unknown;
    }

    Copy user_copy(const Symbol* s) {
        if (auto it = copy_memo_.find(s); it != copy_memo_.end()) return it->second;
        copy_memo_[s] = Copy::unknown;   // recursion guard
        Copy c = Copy::yes;
        if (s->kind == SymbolKind::interface) {
            c = Copy::no;
        } else if (s->kind == SymbolKind::enum_) {
            for (const Symbol* a : s->member_order) {
                if (a->alternative && a->alternative->payload) c = combine(c, copy_kind(lower(*a->alternative->payload, a)));
            }
        } else {
            std::function<void(const Symbol*)> walk = [&](const Symbol* t) {
                for (const Symbol* b : t->bases) walk(b);
                if (t->has_opaque_base) c = combine(c, Copy::unknown);
                for (const Symbol* m : t->member_order) {
                    if (m->kind == SymbolKind::method && m->function() && m->function()->is_destructor) c = Copy::no;
                    if (m->kind == SymbolKind::field && !m->is_static) {
                        c = combine(c, copy_kind(lower(*m->decl->as<VariableDecl>().type, m)));
                    }
                }
            };
            walk(s);
        }
        copy_memo_[s] = c;
        return c;
    }

    bool is_view(const Type* t) {
        if (!t) return false;
        switch (t->kind) {
        case Kind::reference: return true;
        case Kind::external: {
            std::string_view base = t->external_base();
            if (base == "std::str" || base == "std::span" || base == "std::format_args") return true;
            if (const std::vector<const Type*>* args = tc_.external_args(t)) {
                for (const Type* a : *args) {
                    if (is_view(a)) return true;
                }
            }
            return false;
        }
        case Kind::user: {
            const Symbol* s = t->symbol;
            if (s->decl && (s->kind == SymbolKind::struct_ || s->kind == SymbolKind::class_)) {
                return s->decl->as<TypeDecl>().is_view;
            }
            if (s->kind == SymbolKind::enum_) {
                if (view_memo_.count(s)) return view_memo_[s];
                view_memo_[s] = false;
                bool v = false;
                for (const Symbol* a : s->member_order) {
                    if (a->alternative && a->alternative->payload && is_view(lower(*a->alternative->payload, a))) v = true;
                }
                return view_memo_[s] = v;
            }
            return false;
        }
        default: return false;
        }
    }

    /// Why a type cannot be cloned (empty if it can, or if unknown).
    std::string clone_blocker(const Type* t, int depth = 0) {
        if (!t || depth > 8 || copy_kind(t) == Copy::yes) return "";
        if (t->is(Kind::external)) {
            std::string_view base = t->external_base();
            if (base == "std::unique_ptr" || base == "std::file" || base == "std::mutex" || base == "std::atomic") {
                return std::format("{} cannot be cloned", quoted(t));
            }
            return "";
        }
        if (!t->is(Kind::user)) return "";
        const Symbol* s = t->symbol;
        if (s->kind == SymbolKind::class_) {
            return s->members.count("clone") ? "" : std::format("class {} has no 'clone()' method", quoted(t));
        }
        if (s->kind == SymbolKind::struct_) {
            for (const Symbol* m : s->member_order) {
                if (m->kind != SymbolKind::field || m->is_static) continue;
                const Type* ft = lower(*m->decl->as<VariableDecl>().type, m);
                std::string why = clone_blocker(ft, depth + 1);
                if (!why.empty()) return std::format("field '{}': {}", m->name, why);
            }
        }
        if (s->kind == SymbolKind::enum_) {
            for (const Symbol* a : s->member_order) {
                if (!a->alternative || !a->alternative->payload) continue;
                std::string why = clone_blocker(lower(*a->alternative->payload, a), depth + 1);
                if (!why.empty()) return std::format("alternative '{}': {}", a->name, why);
            }
        }
        return "";
    }

    // --- type and signature rules ---------------------------------------------------------

    void check_types_and_signatures(Module& m) {
        for (Symbol* s : m.item_order) {
            if (s->kind == SymbolKind::struct_ || s->kind == SymbolKind::class_) {
                bool view = s->decl->as<TypeDecl>().is_view;
                for (Symbol* f : s->member_order) {
                    if (f->kind == SymbolKind::method) {
                        check_signature(*f, /*is_method=*/!f->is_static);
                        continue;
                    }
                    if (f->kind != SymbolKind::field || view) continue;
                    const auto& v  = f->decl->as<VariableDecl>();
                    const Type* ft = lower(*v.type);
                    if (ft->is(Kind::reference)) {
                        Diagnostic& d = error(DiagCode::E5010, v.type->span,
                                              std::format("field '{}::{}' is a reference", s->name, f->name),
                                              "reference field");
                        note(d, "references only live in parameters, locals and loops; store a value, a smart "
                                "pointer, or declare the type 'view'");
                    } else if (is_view(ft)) {
                        Diagnostic& d = error(DiagCode::E5011, v.type->span,
                                              std::format("field '{}::{}' has view type {}, but '{}' is not a view type",
                                                          s->name, f->name, quoted(ft), s->name),
                                              "view field");
                        note(d, std::format("declare it 'view {} {}', or store an owning type such as std::string",
                                            sema::to_string(s->kind), s->name));
                    }
                }
            } else if (s->kind == SymbolKind::function) {
                check_signature(*s, false);
            }
        }
    }

    /// A function returning a reference or view must have one place to borrow from (spec 12).
    void check_signature(const Symbol& s, bool is_method) {
        const FunctionDecl* f = s.function();
        if (!f || !f->result || f->is_destructor) return;
        const Type* r = lower(*f->result, &s);
        if (!is_view(r) || is_method) return;
        std::vector<const Param*> sources;
        for (const Param& p : f->params) {
            if (is_view(lower(*p.type, &s))) sources.push_back(&p);
        }
        if (sources.size() == 1) return;
        if (sources.empty()) {
            Diagnostic& d = error(DiagCode::E5012, f->result->span,
                                  std::format("'{}' returns {} but has no reference parameter to borrow from", f->name,
                                              quoted(r)),
                                  "nothing to borrow from");
            note(d, "return an owning value (e.g. std::string) instead");
        } else {
            Diagnostic& d = error(DiagCode::E5012, f->result->span,
                                  std::format("'{}' returns {}, which could borrow from '{}' or '{}'", f->name, quoted(r),
                                              sources[0]->name, sources[1]->name),
                                  "ambiguous source");
            note(d, "a returned reference must borrow from 'this' or from the only reference parameter");
        }
    }

    // --- function walk --------------------------------------------------------------------

    void walk_file(ast::File& file) {
        for (DeclPtr& d : file.decls) walk_decl(*d);
    }

    void walk_decl(Decl& d) {
        switch (d.kind) {
        case DeclKind::function: walk_function(d.as<FunctionDecl>()); break;
        case DeclKind::struct_:
        case DeclKind::class_:
        case DeclKind::interface:
            for (DeclPtr& m : d.as<TypeDecl>().members) walk_decl(*m);
            break;
        default: break;
        }
    }

    struct FunctionFrame {
        State                          state;
        std::vector<LoopContext>       loops;
        std::vector<Borrow>            borrows;
        std::map<const Symbol*, std::uint32_t> live_end;
        bool                           returns_view = false;
        std::string                    name;
    };

    void walk_function(FunctionDecl& fn) {
        if (!fn.body) return;
        FunctionFrame saved = std::move(frame_);
        frame_              = FunctionFrame{};
        frame_.name         = fn.name;
        frame_.returns_view = fn.result && is_view(lower(*fn.result));
        compute_live_ranges(*fn.body);
        walk_block(*fn.body);
        frame_ = std::move(saved);
    }

    /// For every local: the position up to which it is used. A use inside a loop that the
    /// local was declared outside of keeps it alive until the end of that loop.
    void compute_live_ranges(BlockExpr& body) {
        std::vector<Span> loop_spans;
        std::function<void(Expr&)> visit = [&](Expr& e) {
            if (e.kind == ExprKind::path) {
                const Symbol* s = e.as<PathExpr>().symbol;
                if (s && s->kind == SymbolKind::local) {
                    std::uint32_t end = e.span.end;
                    for (const Span& l : loop_spans) {
                        if (s->span.begin < l.begin && e.span.begin >= l.begin) end = std::max(end, l.end);
                    }
                    frame_.live_end[s] = std::max(frame_.live_end[s], end);
                }
            }
            bool is_loop = e.kind == ExprKind::while_ || e.kind == ExprKind::for_;
            if (is_loop) loop_spans.push_back(e.span);
            for_each_child(e, visit);
            if (is_loop) loop_spans.pop_back();
        };
        visit(body);
    }

    /// Calls `f` on every direct sub-expression (statements included).
    static void for_each_child(Expr& e, const std::function<void(Expr&)>& f) {
        auto stmt = [&](Stmt& s) {
            switch (s.kind) {
            case StmtKind::variable:
                if (s.as<VariableStmt>().init) f(*s.as<VariableStmt>().init);
                break;
            case StmtKind::assign:
                f(*s.as<AssignStmt>().target);
                f(*s.as<AssignStmt>().value);
                break;
            case StmtKind::discard: f(*s.as<DiscardStmt>().value); break;
            case StmtKind::expression: f(*s.as<ExprStmt>().expr); break;
            }
        };
        switch (e.kind) {
        case ExprKind::unary: f(*e.as<UnaryExpr>().operand); break;
        case ExprKind::binary:
            f(*e.as<BinaryExpr>().lhs);
            f(*e.as<BinaryExpr>().rhs);
            break;
        case ExprKind::range:
            f(*e.as<RangeExpr>().lo);
            f(*e.as<RangeExpr>().hi);
            break;
        case ExprKind::call:
            f(*e.as<CallExpr>().callee);
            for (ExprPtr& a : e.as<CallExpr>().args) f(*a);
            break;
        case ExprKind::index:
            f(*e.as<IndexExpr>().object);
            f(*e.as<IndexExpr>().index);
            break;
        case ExprKind::member: f(*e.as<MemberExpr>().object); break;
        case ExprKind::string_literal:
            for (FormatPart& part : e.as<LiteralExpr>().format_parts) {
                if (part.expr) f(*part.expr);
            }
            break;
        case ExprKind::try_: f(*e.as<TryExpr>().operand); break;
        case ExprKind::cast: f(*e.as<CastExpr>().operand); break;
        case ExprKind::bit: f(*e.as<BitExpr>().operand); break;
        case ExprKind::struct_literal:
            for (FieldInit& fi : e.as<StructLiteralExpr>().fields) f(*fi.value);
            break;
        case ExprKind::lambda:
            for (Capture& c : e.as<LambdaExpr>().captures) {
                if (c.init) f(*c.init);
            }
            f(*e.as<LambdaExpr>().body);
            break;
        case ExprKind::if_: {
            auto& i = e.as<IfExpr>();
            f(*i.condition);
            f(*i.then_block);
            if (i.else_branch) f(*i.else_branch);
            break;
        }
        case ExprKind::match:
            f(*e.as<MatchExpr>().scrutinee);
            for (MatchArm& a : e.as<MatchExpr>().arms) {
                if (a.guard) f(*a.guard);
                f(*a.body);
            }
            break;
        case ExprKind::block:
            for (StmtPtr& s : e.as<BlockExpr>().statements) stmt(*s);
            if (e.as<BlockExpr>().tail) f(*e.as<BlockExpr>().tail);
            break;
        case ExprKind::while_:
            f(*e.as<WhileExpr>().condition);
            f(*e.as<WhileExpr>().body);
            break;
        case ExprKind::for_:
            f(*e.as<ForExpr>().iterable);
            f(*e.as<ForExpr>().body);
            break;
        case ExprKind::return_:
            if (e.as<ReturnExpr>().value) f(*e.as<ReturnExpr>().value);
            break;
        default: break;
        }
    }

    // --- uses, moves, mutations -----------------------------------------------------------

    static const Symbol* local_of(const Expr& e) {
        if (e.kind != ExprKind::path) return nullptr;
        const Symbol* s = e.as<PathExpr>().symbol;
        return s && s->kind == SymbolKind::local ? s : nullptr;
    }

    /// The local an expression's place is rooted in: `a.b[i].c` -> a. `this` has its own root.
    const Symbol* root_of(const Expr& e) {
        switch (e.kind) {
        case ExprKind::path: return local_of(e);
        case ExprKind::this_: return &this_root_;
        case ExprKind::member: return e.as<MemberExpr>().arrow ? nullptr : root_of(*e.as<MemberExpr>().object);
        case ExprKind::index: return root_of(*e.as<IndexExpr>().object);
        case ExprKind::unary: {
            UnaryOp op = e.as<UnaryExpr>().op;
            return op == UnaryOp::move || op == UnaryOp::mut_marker ? root_of(*e.as<UnaryExpr>().operand) : nullptr;
        }
        default: return nullptr;
        }
    }

    void use(const Symbol* s, Span span) {
        if (!frame_.state.reachable) return;
        if (auto it = frame_.state.moved.find(s); it != frame_.state.moved.end()) {
            if (silent_ == 0 && reported_.insert({s, span.begin}).second) {
                bool earlier_iteration = it->second.begin > span.begin;
                Diagnostic& d = error(DiagCode::E5002, span, std::format("'{}' is used after it was moved", s->name),
                                      "used after move");
                note(d, earlier_iteration ? "moved here, in an earlier iteration of the loop" : "moved here", it->second);
                note(d, std::format("assign a new value to '{}' first, or move a '.clone()' instead", s->name));
            }
        }
        for (const LoopContext& l : frame_.loops) {
            if (l.borrowed == s && l.borrow_mut) {
                Diagnostic& d = error(DiagCode::E5021, span,
                                      std::format("'{}' is used while the loop borrows it mutably through '{}'", s->name, l.var),
                                      "conflicting use");
                note(d, "a 'mut T&' is the only way to reach the object while it is alive", l.span);
            }
        }
    }

    void mutation(const Symbol* root, Span span, std::string_view action) {
        if (!root) return;
        for (const LoopContext& l : frame_.loops) {
            if (l.borrowed == root) {
                Diagnostic& d = error(DiagCode::E5021, span,
                                      std::format("cannot {} '{}' while the loop borrows it", action, root->name),
                                      "the loop holds a reference");
                note(d, std::format("'{}' refers into '{}' for the whole loop", l.var, root->name), l.span);
            }
        }
        for (const Borrow& b : frame_.borrows) {
            if (b.root == root && span.begin > b.decl.end && span.begin < b.live_end) {
                Diagnostic& d = error(DiagCode::E5022, span,
                                      std::format("cannot {} '{}' while '{}' refers to it", action, root->name, b.ref->name),
                                      "still borrowed");
                note(d, std::format("'{}' borrows from '{}' here and is used later", b.ref->name, root->name), b.decl);
            }
        }
    }

    // --- consumption: copies and implicit moves -------------------------------------------

    /// `value` is stored into a place of type `target` (or returned / passed by value).
    void consume(Expr& value, const Type* target, bool is_return = false) {
        if (target && is_view(target)) return;   // binds a reference or view (std::string -> std::str): no copy
        check_copy(value, is_return);
    }

    void check_copy(Expr& e, bool is_return) {
        switch (e.kind) {
        case ExprKind::unary: {
            UnaryOp op = e.as<UnaryExpr>().op;
            if (op == UnaryOp::move || op == UnaryOp::mut_marker) return;
            if (op != UnaryOp::deref) return;
            break;   // *p copies the pointee
        }
        case ExprKind::block:
            if (e.as<BlockExpr>().tail) check_copy(*e.as<BlockExpr>().tail, is_return);
            return;
        case ExprKind::if_: {
            auto& i = e.as<IfExpr>();
            check_copy(*i.then_block, is_return);
            if (i.else_branch) check_copy(*i.else_branch, is_return);
            return;
        }
        case ExprKind::match:
            for (MatchArm& a : e.as<MatchExpr>().arms) check_copy(*a.body, is_return);
            return;
        case ExprKind::path:
        case ExprKind::member:
        case ExprKind::index: break;
        default: return;   // temporaries are moved
        }
        const Type* t = types::strip_ref(e.type);
        if (copy_kind(t) != Copy::no) return;
        const Symbol* local = local_of(e);
        if (local && frame_.state.moved.count(local)) return;   // already reported as E5002
        if (is_return && local && !local->is_reference &&
            (local->local_kind == LocalKind::variable || local->local_kind == LocalKind::value_param ||
             local->local_kind == LocalKind::binding || local->local_kind == LocalKind::capture)) {
            return;   // `return x;` moves a local implicitly (spec 12)
        }
        std::string what = e.kind == ExprKind::path ? ast::to_string(e.as<PathExpr>().path) : "this value";
        if (e.kind == ExprKind::member) what = "this field";
        Diagnostic& d = error(DiagCode::E5001, e.span,
                              std::format("{} has type {}, which is not a copy type",
                                          e.kind == ExprKind::path ? "'" + what + "'" : what, quoted(t)),
                              "would be copied");
        if (local && !local->is_reference) {
            note(d, std::format("write 'move {0}' to transfer ownership, or '{0}.clone()' for a visible copy", what));
        } else if (e.kind == ExprKind::member) {
            note(d, "a field cannot be moved out: use '.clone()', 'std::take(mut obj.field)' or 'std::replace(...)'");
        } else {
            note(d, "use '.clone()' to make a visible copy");
        }
    }

    // --- statements and expressions -------------------------------------------------------

    void walk_block(BlockExpr& b) {
        std::size_t borrows = frame_.borrows.size();
        for (StmtPtr& s : b.statements) walk_stmt(*s);
        if (b.tail) walk(*b.tail);
        frame_.borrows.resize(borrows);   // references declared in the block are gone
    }

    void walk_stmt(Stmt& s) {
        switch (s.kind) {
        case StmtKind::variable: {
            auto& v = s.as<VariableStmt>();
            if (v.init) {
                walk(*v.init);
                consume(*v.init, v.symbol ? v.symbol->type : nullptr);
            }
            if (v.symbol) {
                frame_.state.moved.erase(v.symbol);
                const Type* t = v.symbol->type;
                if (t && t->is(Kind::reference) && v.init) {
                    if (const Symbol* root = root_of(*v.init); root && root != &this_root_) {
                        frame_.borrows.push_back(Borrow{v.symbol, root, v.span, frame_.live_end[v.symbol]});
                    }
                }
            }
            return;
        }
        case StmtKind::assign: {
            auto& a = s.as<AssignStmt>();
            walk(*a.value);
            const Symbol* target_local = local_of(*a.target);
            if (a.op == TokenKind::eq && target_local) {
                mutation(target_local, a.target->span, "assign to");
                frame_.state.moved.erase(target_local);   // a moved-from variable is alive again
            } else {
                walk(*a.target);
                mutation(root_of(*a.target), a.target->span, "change");
            }
            if (a.op == TokenKind::eq) consume(*a.value, a.target->type);
            return;
        }
        case StmtKind::discard: walk(*s.as<DiscardStmt>().value); return;
        case StmtKind::expression: walk(*s.as<ExprStmt>().expr); return;
        }
    }

    void walk(Expr& e) {
        switch (e.kind) {
        case ExprKind::path:
            if (const Symbol* s = local_of(e)) use(s, e.span);
            return;
        case ExprKind::unary: {
            auto& u = e.as<UnaryExpr>();
            walk(*u.operand);
            if (u.op == UnaryOp::move) {
                if (const Symbol* s = local_of(*u.operand)) {
                    mutation(s, e.span, "move");
                    if (frame_.state.reachable) frame_.state.moved[s] = e.span;
                }
            } else if (u.op == UnaryOp::address_of_mut) {
                mutation(root_of(*u.operand), e.span, "take a mutable pointer to");
            }
            return;
        }
        case ExprKind::call: walk_call(e.as<CallExpr>()); return;
        case ExprKind::struct_literal: {
            auto&       sl = e.as<StructLiteralExpr>();
            const Type* t  = types::strip_ref(e.type);
            for (FieldInit& f : sl.fields) {
                walk(*f.value);
                const Type* ft = nullptr;
                if (t && t->is(Kind::user)) {
                    if (auto it = t->symbol->members.find(f.name); it != t->symbol->members.end()) {
                        ft = lower(*it->second.front()->decl->as<VariableDecl>().type, it->second.front());
                    }
                }
                consume(*f.value, ft);
            }
            return;
        }
        case ExprKind::lambda: walk_lambda(e.as<LambdaExpr>()); return;
        case ExprKind::if_: {
            auto& i = e.as<IfExpr>();
            walk(*i.condition);
            State before = frame_.state;
            walk_block(*i.then_block);
            State then_state = frame_.state;
            frame_.state     = before;
            if (i.else_branch) walk(*i.else_branch);
            frame_.state = merge(then_state, frame_.state);
            return;
        }
        case ExprKind::match: {
            auto& m = e.as<MatchExpr>();
            walk(*m.scrutinee);
            State before = frame_.state;
            State after  = State::unreachable();
            for (MatchArm& arm : m.arms) {
                frame_.state = before;
                forget_bindings(*arm.pattern);   // fresh bindings in every arm (and iteration)
                if (arm.guard) walk(*arm.guard);
                walk(*arm.body);
                after = merge(after, frame_.state);
            }
            if (!m.arms.empty()) frame_.state = after;
            return;
        }
        case ExprKind::block: walk_block(e.as<BlockExpr>()); return;
        case ExprKind::while_: {
            auto& w = e.as<WhileExpr>();
            bool  forever = w.condition->kind == ExprKind::bool_literal && w.condition->as<LiteralExpr>().bool_value;
            walk_loop(w.condition.get(), *w.body, nullptr, nullptr, false, e.span, "", forever);
            return;
        }
        case ExprKind::for_: {
            auto& f = e.as<ForExpr>();
            walk(*f.iterable);
            const Symbol* borrowed = nullptr;
            bool          by_ref   = f.type->kind == TypeKind::reference;
            if (by_ref) borrowed = root_of(*f.iterable);
            if (borrowed == &this_root_) borrowed = nullptr;
            bool is_mut = by_ref && f.type->as<ReferenceType>().is_mut;
            walk_loop(nullptr, *f.body, f.symbol, borrowed, is_mut, e.span, f.name, false);
            return;
        }
        case ExprKind::return_: {
            auto& r = e.as<ReturnExpr>();
            if (r.value) {
                walk(*r.value);
                consume(*r.value, nullptr, /*is_return=*/true);
                if (frame_.returns_view) check_returned_reference(*r.value);
            }
            frame_.state = State::unreachable();
            return;
        }
        case ExprKind::break_:
            if (!frame_.loops.empty()) frame_.loops.back().breaks = merge(frame_.loops.back().breaks, frame_.state);
            frame_.state = State::unreachable();
            return;
        case ExprKind::continue_:
            if (!frame_.loops.empty()) frame_.loops.back().continues = merge(frame_.loops.back().continues, frame_.state);
            frame_.state = State::unreachable();
            return;
        default: for_each_child(e, [&](Expr& c) { walk(c); }); return;
        }
    }

    void forget_bindings(const Pattern& p) {
        switch (p.kind) {
        case PatternKind::binding:
            if (p.as<BindingPattern>().symbol) frame_.state.moved.erase(p.as<BindingPattern>().symbol);
            return;
        case PatternKind::enum_alternative:
            if (p.as<EnumPattern>().payload) forget_bindings(*p.as<EnumPattern>().payload);
            return;
        case PatternKind::struct_:
            for (const FieldPattern& f : p.as<StructPattern>().fields) forget_bindings(*f.pattern);
            return;
        default: return;
        }
    }

    void walk_loop(Expr* cond, BlockExpr& body, const Symbol* loop_var, const Symbol* borrowed, bool borrow_mut,
                   Span span, std::string var, bool forever) {
        State pre   = frame_.state;
        State entry = pre;
        // Find the state at the loop head by iterating to a fixpoint without reporting,
        // then walk once more reporting errors.
        for (int pass = 0; pass < 4; ++pass) {
            ++silent_;
            LoopContext ctx;
            frame_.loops.push_back(ctx);
            frame_.state = entry;
            if (loop_var) frame_.state.moved.erase(loop_var);
            if (cond) walk(*cond);
            walk_block(body);
            State end = merge(frame_.state, frame_.loops.back().continues);
            frame_.loops.pop_back();
            --silent_;
            State next = merge(pre, end);
            if (next == entry) break;
            entry = next;
        }
        LoopContext ctx;
        ctx.borrowed   = borrowed;
        ctx.borrow_mut = borrow_mut;
        ctx.span       = span;
        ctx.var        = std::move(var);
        frame_.loops.push_back(ctx);
        frame_.state = entry;
        if (loop_var) frame_.state.moved.erase(loop_var);
        if (cond) walk(*cond);
        State on_exit = forever ? State::unreachable() : frame_.state;
        walk_block(body);
        State end    = merge(frame_.state, frame_.loops.back().continues);
        State breaks = frame_.loops.back().breaks;
        frame_.loops.pop_back();
        if (!cond && !forever) on_exit = merge(entry, end);   // for: leaves when the range is done
        frame_.state = merge(on_exit, breaks);
    }

    void walk_call(CallExpr& c) {
        Expr&         callee = *c.callee;
        const FunctionDecl* fn = nullptr;
        const Symbol*       owner_sym = nullptr;
        const Symbol* receiver = nullptr;
        bool          mut_receiver = false;
        const EnumAlternative* alt = nullptr;
        if (callee.kind == ExprKind::member) {
            auto& me = callee.as<MemberExpr>();
            walk(*me.object);
            if (me.symbol) fn = me.symbol->function();
            if (!me.symbol && types::is_mutating_library_method(types::strip_ref(me.object->type), me.name.segments.back().name)) {
                receiver     = root_of(*me.object);
                mut_receiver = true;
                mutation(receiver, me.object->span, std::format("call '{}' on", me.name.segments.back().name));
            }
            if (fn && fn->is_mut) {
                receiver     = root_of(*me.object);
                mut_receiver = true;
                mutation(receiver, me.object->span, std::format("call mut method '{}' on", fn->name));
            }
            check_clone(c, me);
        } else if (callee.kind == ExprKind::path) {
            const Symbol* s = callee.as<PathExpr>().symbol;
            if (s && s->kind == SymbolKind::local) use(s, callee.span);
            if (s && (s->kind == SymbolKind::function || s->kind == SymbolKind::method)) fn = s->function(), owner_sym = s;
            if (s && s->kind == SymbolKind::alternative) alt = s->alternative, owner_sym = s;
        } else {
            walk(callee);
        }
        for (std::size_t i = 0; i < c.args.size(); ++i) {
            Expr& arg = *c.args[i];
            walk(arg);
            bool marked = arg.kind == ExprKind::unary && arg.as<UnaryExpr>().op == UnaryOp::mut_marker;
            if (marked) mutation(root_of(arg), arg.span, "pass as a mutable reference");
            const Type* param = nullptr;
            if (fn && i < fn->params.size() && !fn->is_template) param = lower(*fn->params[i].type, owner_sym);
            if (alt && alt->payload) param = lower(*alt->payload, owner_sym);
            if (param && !marked) consume(arg, param);
        }
        check_aliasing(c, receiver, mut_receiver);
    }

    /// One mutable reference at a time: `f(mut a, a)`, `f(mut a, mut a)`, `a.push(mut a)`.
    void check_aliasing(CallExpr& c, const Symbol* receiver, bool mut_receiver) {
        struct Arg {
            const Symbol* root;
            bool          is_mut;
            Span          span;
        };
        std::vector<Arg> args;
        if (receiver) args.push_back(Arg{receiver, mut_receiver, c.callee->span});
        for (ExprPtr& a : c.args) {
            bool marked = a->kind == ExprKind::unary && a->as<UnaryExpr>().op == UnaryOp::mut_marker;
            const Symbol* root = root_of(*a);
            if (a->kind == ExprKind::unary && a->as<UnaryExpr>().op == UnaryOp::move) root = nullptr;
            if (root) args.push_back(Arg{root, marked, a->span});
        }
        for (std::size_t i = 0; i < args.size(); ++i) {
            for (std::size_t j = i + 1; j < args.size(); ++j) {
                if (args[i].root != args[j].root || !(args[i].is_mut || args[j].is_mut)) continue;
                const Arg& m = args[i].is_mut ? args[i] : args[j];
                const Arg& o = args[i].is_mut ? args[j] : args[i];
                std::string name = args[i].root == &this_root_ ? "this" : args[i].root->name;
                Diagnostic& d = error(DiagCode::E5020, o.span,
                                      std::format("'{}' is passed as a mutable reference and also as another argument", name),
                                      "second reference");
                note(d, "the mutable reference is here; while it exists it must be the only way to reach the value", m.span);
                return;
            }
        }
    }

    void check_clone(CallExpr& c, MemberExpr& me) {
        if (me.symbol || !c.args.empty() || me.name.segments.back().name != "clone") return;
        const Type* t = types::strip_ref(me.object->type);
        if (!t || !t->is(Kind::user)) return;
        std::string why = clone_blocker(t);
        if (why.empty()) return;
        Diagnostic& d = error(DiagCode::E5030, c.span, std::format("{} cannot be cloned", quoted(t)), "not cloneable");
        note(d, why);
    }

    void walk_lambda(LambdaExpr& l) {
        for (Capture& cap : l.captures) {
            if (cap.init) {
                walk(*cap.init);
                consume(*cap.init, nullptr);
                continue;
            }
            const Symbol* outer = cap.captured;
            if (!outer) continue;
            use(outer, cap.span);
            if (cap.kind == CaptureKind::move) {
                mutation(outer, cap.span, "move");
                if (frame_.state.reachable) frame_.state.moved[outer] = cap.span;
            } else if (cap.kind == CaptureKind::copy && copy_kind(types::strip_ref(outer->type)) == Copy::no) {
                Diagnostic& d = error(DiagCode::E5001, cap.span,
                                      std::format("capturing '{}' by copy would copy a {}", outer->name,
                                                  quoted(types::strip_ref(outer->type))),
                                      "copy capture");
                note(d, std::format("capture it as '[move {0}]', '[&{0}]', or '[{0} = {0}.clone()]'", outer->name));
            } else if (cap.kind == CaptureKind::mut_ref) {
                mutation(outer, cap.span, "capture a mutable reference to");
            }
        }
        // The body is its own function: fresh moves, no enclosing loops.
        FunctionFrame saved  = std::move(frame_);
        frame_               = FunctionFrame{};
        frame_.name          = "lambda";
        frame_.live_end      = saved.live_end;
        walk_block(*l.body);
        frame_ = std::move(saved);
    }

    /// A returned reference must point into `this` or into a reference parameter.
    void check_returned_reference(Expr& value) {
        const Symbol* root = root_of(value);
        if (root == &this_root_) return;
        if (!root) {
            if (value.kind == ExprKind::struct_literal) {
                error(DiagCode::E5013, value.span, "returns a reference to a temporary", "destroyed at the end of the statement");
            }
            return;
        }
        if (root->is_reference || is_view(root->type) || root->local_kind == LocalKind::ref_param) return;
        Diagnostic& d = error(DiagCode::E5013, value.span,
                              std::format("returns a reference into {} '{}', which is destroyed when '{}' returns",
                                          root->local_kind == LocalKind::value_param ? "by-value parameter" : "local",
                                          root->name, frame_.name),
                              "does not outlive the function");
        note(d, "return an owning value instead, or borrow from a reference parameter");
    }

    sema::Program&                         program_;
    DiagnosticEngine&                      diags_;
    types::TypeContext&                    tc_;
    Module*                                module_ = nullptr;
    const std::map<const Symbol*, const types::Type*>* cur_subst_ = nullptr;   // of the instance being walked
    FunctionFrame                          frame_;
    int                                    silent_ = 0;
    Diagnostic                             scratch_{DiagCode::E5001, Severity::error, {}, {}, {}, {}};
    std::set<std::pair<const Symbol*, std::uint32_t>> reported_;
    std::map<const Symbol*, Copy>          copy_memo_;
    std::map<const Symbol*, bool>          view_memo_;
    Symbol                                 this_root_;   // stands for `this` in root_of()
};

} // namespace

void check_ownership(sema::Program& program, DiagnosticEngine& diags) {
    if (!program.types) return;
    Ownership(program, diags).run();
}

} // namespace rift

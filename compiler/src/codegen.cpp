#include "rift/codegen.hpp"

#include "rift/ast_printer.hpp"
#include "rift/types.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <format>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace rift {

namespace {

using namespace ast;
using sema::LocalKind;
using sema::Module;
using sema::Program;
using sema::Symbol;
using sema::SymbolKind;
using types::Kind;
using types::Type;

// --- small helpers ------------------------------------------------------------------------

bool is_void_like(const Type* t) { return !t || t->is(Kind::void_) || t->is(Kind::never); }

/// Every direct child expression of `e` (statements included), for scans like "has a call".
void visit_children(Expr& e, const std::function<void(Expr&)>& f) {
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
    case ExprKind::string_literal:
        for (FormatPart& p : e.as<LiteralExpr>().format_parts) {
            if (p.expr) f(*p.expr);
        }
        break;
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
    case ExprKind::try_: f(*e.as<TryExpr>().operand); break;
    case ExprKind::cast: f(*e.as<CastExpr>().operand); break;
    case ExprKind::bit: f(*e.as<BitExpr>().operand); break;
    case ExprKind::struct_literal:
        for (FieldInit& fi : e.as<StructLiteralExpr>().fields) f(*fi.value);
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

/// What evaluating an operand can do besides producing a value. C leaves the order of operands
/// unspecified, so operands are put into temporaries (left to right) when the order shows.
enum class Effect { none, may_panic, side_effects };

/// Checked integer operations (spec 16) panic on overflow, division by zero or a bad shift.
bool may_panic_here(Expr& e) {
    auto integer = [](const Expr& x) {
        const types::Type* t = types::strip_ref(x.type);
        return t && t->is_integer();
    };
    switch (e.kind) {
    case ExprKind::binary: {
        TokenKind op = e.as<BinaryExpr>().op;
        bool checked = op == TokenKind::plus || op == TokenKind::minus || op == TokenKind::star || op == TokenKind::slash ||
                       op == TokenKind::percent || op == TokenKind::shl || op == TokenKind::shr;
        return checked && integer(*e.as<BinaryExpr>().lhs);
    }
    case ExprKind::unary:
        return e.as<UnaryExpr>().op == UnaryOp::negate && integer(e) &&
               e.as<UnaryExpr>().operand->kind != ExprKind::int_literal;
    case ExprKind::bit:
    case ExprKind::index: return true;
    default: return false;
    }
}

Effect effect_of(Expr& e) {
    switch (e.kind) {
    case ExprKind::call:
    case ExprKind::if_:
    case ExprKind::match:
    case ExprKind::block:
    case ExprKind::while_:
    case ExprKind::for_:
    case ExprKind::return_:
    case ExprKind::break_:
    case ExprKind::continue_:
    case ExprKind::try_: return Effect::side_effects;
    default: break;
    }
    Effect worst = may_panic_here(e) ? Effect::may_panic : Effect::none;
    visit_children(e, [&](Expr& c) { worst = std::max(worst, effect_of(c)); });
    return worst;
}

/// A C string literal for arbitrary bytes.
std::string c_string(std::string_view bytes) {
    std::string out = "\"";
    for (char ch : bytes) {
        auto c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c >= 0x20 && c < 0x7F && c != '?') {
            out += static_cast<char>(c);
        } else {
            out += std::format("\\{:03o}", c);
        }
    }
    return out + "\"";
}

/// Length-prefixed name parts: ["main", "add"] -> "4main3add". Unambiguous for any names.
std::string mangle_parts(const std::vector<std::string>& parts) {
    std::string out;
    for (const std::string& p : parts) out += std::to_string(p.size()) + p;
    return out;
}

std::vector<std::string> split_module(const std::string& name) {
    std::vector<std::string> out;
    std::size_t              start = 0;
    while (true) {
        std::size_t pos = name.find("::", start);
        out.push_back(name.substr(start, pos - start));
        if (pos == std::string::npos) break;
        start = pos + 2;
    }
    return out;
}

/// Integer C types and limits, by Rift spelling.
struct IntInfo {
    const char* ctype;
    const char* unsigned_ctype;
    const char* min;
    const char* max;
};

const IntInfo* int_info(const std::string& name) {
    static const std::map<std::string, IntInfo> table = {
        {"i8", {"int8_t", "uint8_t", "INT8_MIN", "INT8_MAX"}},
        {"i16", {"int16_t", "uint16_t", "INT16_MIN", "INT16_MAX"}},
        {"i32", {"int32_t", "uint32_t", "INT32_MIN", "INT32_MAX"}},
        {"i64", {"int64_t", "uint64_t", "INT64_MIN", "INT64_MAX"}},
        {"i128", {"rift_i128", "rift_u128", "((rift_i128)1 << 127)", "((rift_i128)(~(rift_u128)0 >> 1))"}},
        {"isize", {"int64_t", "uint64_t", "INT64_MIN", "INT64_MAX"}},
        {"u8", {"uint8_t", "uint8_t", "0", "UINT8_MAX"}},
        {"u16", {"uint16_t", "uint16_t", "0", "UINT16_MAX"}},
        {"u32", {"uint32_t", "uint32_t", "0", "UINT32_MAX"}},
        {"u64", {"uint64_t", "uint64_t", "0", "UINT64_MAX"}},
        {"u128", {"rift_u128", "rift_u128", "0", "(~(rift_u128)0)"}},
        {"usize", {"uint64_t", "uint64_t", "0", "UINT64_MAX"}},
    };
    auto it = table.find(name);
    return it == table.end() ? nullptr : &it->second;
}

/// A C constant expression for an unsigned 128-bit magnitude, converted to `ctype`.
std::string int_constant(u128 v, const std::string& ctype) {
    auto hi = static_cast<std::uint64_t>(v >> 64);
    auto lo = static_cast<std::uint64_t>(v);
    if (hi != 0) return std::format("(({})(((rift_u128){}ULL << 64) | {}ULL))", ctype, hi, lo);
    if (lo <= 0x7FFFFFFF) return std::format("(({}){})", ctype, lo);
    return std::format("(({}){}ULL)", ctype, lo);
}

// --- the generator ------------------------------------------------------------------------

class Generator {
public:
    Generator(Program& program, const SourceManager& sources, DiagnosticEngine& diags, CodegenOptions options)
        : program_(program), tc_(*program.types), sources_(sources), diags_(diags), options_(options) {}

    std::string run() {
        Symbol* main = options_.all_functions ? find_main_quietly() : find_main();
        if (!main && !options_.all_functions) return {};
        if (main) main_name_ = function_name(main);
        if (options_.all_functions) {
            for (const Module* m : module_order()) {
                for (const Symbol* s : m->item_order) {
                    if (s->kind == SymbolKind::function && !s->function()->is_template) function_name(s);
                    if (!s->is_type() || (s->decl && s->decl->is_template)) continue;
                    for (const auto& [name, members] : s->members) {
                        for (const Symbol* mem : members) {
                            const FunctionDecl* f = mem->function();
                            if (mem->kind == SymbolKind::method && f && !f->is_template && !f->is_abstract && !f->is_virtual &&
                                !f->is_destructor && body_decl(mem)->body) {
                                function_name(mem);
                            }
                        }
                    }
                }
            }
        }
        // Functions, globals and types are discovered while generating; keep going until
        // nothing new turns up.
        while (!fn_queue_.empty() || !global_queue_.empty()) {
            while (!fn_queue_.empty()) {
                const Symbol* f = fn_queue_.front();
                fn_queue_.pop_front();
                generate_function(f);
            }
            while (!global_queue_.empty()) {
                const Symbol* g = global_queue_.front();
                global_queue_.pop_front();
                generate_global(g);
            }
        }
        generate_types();
        return assemble(main);
    }

private:
    // --- diagnostics ----------------------------------------------------------------------

    void unsupported(Span span, const std::string& what, const std::string& why = {}) {
        if (!reported_.insert({span.file, span.begin, what}).second) return;
        Diagnostic& d = diags_.report(DiagCode::E6001, span, std::format("code generation does not support {} yet", what),
                                      "not supported yet");
        if (!why.empty()) d.notes.push_back(Note{why, std::nullopt});
    }

    static constexpr const char* no_std = "the standard library is not written yet";

    // --- program structure ----------------------------------------------------------------

    Symbol* find_main_quietly() {
        if (!program_.entry) return nullptr;
        auto it = program_.entry->items.find("main");
        if (it == program_.entry->items.end()) return nullptr;
        for (Symbol* s : it->second) {
            const FunctionDecl* f = s->kind == SymbolKind::function ? body_decl(s) : nullptr;
            if (f && f->params.empty()) return s;
        }
        return nullptr;
    }

    Symbol* find_main() {
        Module* entry = program_.entry;
        if (entry) {
            if (auto it = entry->items.find("main"); it != entry->items.end()) {
                for (Symbol* s : it->second) {
                    if (s->kind == SymbolKind::function) {
                        const FunctionDecl* f = body_decl(s);
                        if (f && !f->params.empty()) {
                            unsupported(f->params.front().span, "parameters of 'main'");
                        }
                        return s;
                    }
                }
            }
        }
        Span where{};
        if (entry && entry->source) where = Span{entry->source->id, 0, 0};
        else if (entry && entry->header) where = Span{entry->header->id, 0, 0};
        diags_.report(DiagCode::E6002, where, "the program has no 'main' function", "no 'main'")
            .notes.push_back(Note{"a program starts at 'void main()' or 'u32 main()' in the entry file", std::nullopt});
        return nullptr;
    }

    /// The declaration that carries the body: the `.rifts` definition when there is a header.
    static const FunctionDecl* body_decl(const Symbol* s) {
        if (s->definition && s->definition->kind == DeclKind::function) return &s->definition->as<FunctionDecl>();
        return s->function();
    }

    /// Modules in dependency order (imports first), for global initialization.
    std::vector<const Module*> module_order() {
        std::vector<const Module*> out;
        std::set<const Module*>    seen;
        std::function<void(const Module*)> visit = [&](const Module* m) {
            if (!m || !seen.insert(m).second) return;
            for (const Module* i : m->header_imports) visit(i);
            for (const Module* i : m->source_imports) visit(i);
            out.push_back(m);
        };
        visit(program_.entry);
        for (auto& [name, m] : program_.modules) visit(m.get());
        return out;
    }

    // --- names ----------------------------------------------------------------------------

    std::string function_name(const Symbol* s) {
        if (auto it = fn_names_.find(s); it != fn_names_.end()) return it->second;
        std::vector<std::string> parts = split_module(s->module ? s->module->name : "");
        if (s->parent) parts.push_back(s->parent->name);
        parts.push_back(s->name == "~" + (s->parent ? s->parent->name : "") ? "drop" : s->name);
        std::string name = "r_" + mangle_parts(parts);
        // Overloads share a name; number them in declaration order.
        const auto& table = s->parent ? s->parent->members : s->module->items;
        if (auto it = table.find(s->name); it != table.end() && it->second.size() > 1) {
            auto pos = std::find(it->second.begin(), it->second.end(), s);
            name += std::format("_{}", pos - it->second.begin());
        }
        fn_names_[s] = name;
        fn_queue_.push_back(s);
        return name;
    }

    std::string type_name(const Symbol* s) {
        if (auto it = type_names_.find(s); it != type_names_.end()) return it->second;
        std::vector<std::string> parts = split_module(s->module ? s->module->name : "");
        parts.push_back(s->name);
        std::string name = "r_" + mangle_parts(parts);
        type_names_[s]   = name;
        type_order_.push_back(s);
        type_queue_.push_back(s);
        return name;
    }

    std::string global_name(const Symbol* s) {
        if (auto it = global_names_.find(s); it != global_names_.end()) return it->second;
        std::vector<std::string> parts = split_module(s->module ? s->module->name : "");
        parts.push_back(s->name);
        std::string name  = "g_" + mangle_parts(parts);
        global_names_[s]  = name;
        global_queue_.push_back(s);
        return name;
    }

    static std::string local_name(const Symbol* s) { return "v_" + s->name; }

    std::string tmp() { return std::format("t{}", ++tmp_counter_); }

    std::string loc(Span span) {
        auto key = std::make_pair(span.file, span.begin);
        auto it  = locs_.find(key);
        if (it == locs_.end()) it = locs_.emplace(key, locs_.size()).first;
        return std::format("&rift_loc_{}", it->second);
    }

    // --- types ----------------------------------------------------------------------------

    /// The C type for a Rift type (references and pointers become C pointers).
    std::string ctype(const Type* t) {
        if (!t) return "void";
        switch (t->kind) {
        case Kind::int_:
            if (const IntInfo* i = int_info(t->name)) return i->ctype;
            break;
        case Kind::float_: return t->bits == 32 ? "float" : "double";
        case Kind::bool_: return "bool";
        case Kind::char_: return "uint32_t";
        case Kind::void_:
        case Kind::never: return "void";
        case Kind::static_str: return "rift_str";
        case Kind::nullptr_: return "void*";
        case Kind::reference:
        case Kind::pointer: return ctype(t->inner) + "*";
        case Kind::user: {
            const Symbol* s = t->symbol;
            if (s->decl && s->decl->is_template) break;
            if (s->kind == SymbolKind::struct_ || s->kind == SymbolKind::class_ || s->kind == SymbolKind::enum_) {
                return type_name(s);
            }
            break;
        }
        case Kind::external:
            if (t->name == "std::str") return "rift_str";
            break;
        default: break;
        }
        if (!t || t->is_unknown()) unsupported(cur_, "values of this type", "its type comes from the standard library or a template");
        else if (t->is(Kind::external)) unsupported(cur_, std::format("values of type {}", type_text(t)), no_std);
        else unsupported(cur_, std::format("values of type {}", type_text(t)));
        return "int";
    }

    static std::string type_text(const Type* t) {
        if (!t || t->is_unknown()) return "an unknown type";
        return "'" + types::to_string(t) + "'";
    }

    const Type* lower(const TypeExpr& t) { return tc_.lower(t); }

    std::vector<const Symbol*> fields_of(const Symbol* s) {
        std::vector<const Symbol*> out;
        for (const Symbol* m : s->member_order) {
            if (m->kind == SymbolKind::field && !m->is_static) out.push_back(m);
        }
        return out;
    }

    static bool has_payloads(const Symbol* e) {
        for (const Symbol* a : e->member_order) {
            if (a->alternative && a->alternative->payload) return true;
        }
        return false;
    }

    /// Tag value of each alternative (explicit values, otherwise previous + 1).
    std::map<const Symbol*, u128> tag_values(const Symbol* e) {
        std::map<const Symbol*, u128> out;
        u128                          next = 0;
        for (const Symbol* a : e->member_order) {
            if (!a->alternative) continue;
            if (const Expr* v = a->alternative->value.get()) {
                bool negative = false;
                if (v->kind == ExprKind::unary && v->as<UnaryExpr>().op == UnaryOp::negate) {
                    negative = true;
                    v        = v->as<UnaryExpr>().operand.get();
                }
                if (v->kind == ExprKind::int_literal) {
                    u128 m = v->as<LiteralExpr>().int_value;
                    next   = negative ? u128{0} - m : m;
                } else {
                    unsupported(v->span, "enum tags that are not integer literals");
                }
            }
            out[a] = next;
            next   = next + 1;
        }
        return out;
    }

    std::string enum_tag_ctype(const Symbol* e) {
        const auto& d = e->decl->as<EnumDecl>();
        return ctype(lower(*d.tag_type));
    }

    /// Value of an alternative without payload, or the tag constant for comparisons.
    std::string tag_constant(const Symbol* alt) {
        const Symbol* e = alt->parent;
        return int_constant(tag_values(e)[alt], enum_tag_ctype(e));
    }

    std::string alternative_value(const Symbol* alt, const std::string* payload) {
        const Symbol* e = alt->parent;
        if (!has_payloads(e)) return tag_constant(alt);
        std::string out = std::format("(({}){{ .tag = {}", type_name(e), tag_constant(alt));
        if (payload) out += std::format(", .u.p_{} = {}", alt->name, *payload);
        return out + " })";
    }

    struct TypeDef {
        std::string                text;
        std::vector<const Symbol*> deps;   // types it contains by value
    };

    void generate_types() {
        while (!type_queue_.empty()) {
            const Symbol* s = type_queue_.front();
            type_queue_.pop_front();
            type_defs_[s] = define_type(s);
        }
    }

    void by_value_deps(const Type* t, std::vector<const Symbol*>& deps) {
        if (t && t->is(Kind::user)) deps.push_back(t->symbol);
    }

    TypeDef define_type(const Symbol* s) {
        TypeDef     def;
        std::string name = type_names_[s];
        Span        where = s->span;
        Span        saved = cur_;
        cur_              = where;
        if (s->kind == SymbolKind::enum_) {
            std::string tag = enum_tag_ctype(s);
            if (!has_payloads(s)) {
                def.text = std::format("typedef {} {};\n", tag, name);
            } else {
                def.text = std::format("struct {} {{\n    {} tag;\n    union {{\n", name, tag);
                bool any = false;
                for (const Symbol* a : s->member_order) {
                    if (!a->alternative || !a->alternative->payload) continue;
                    const Type* p = lower(*a->alternative->payload);
                    def.text += std::format("        {} p_{};\n", ctype(p), a->name);
                    by_value_deps(p, def.deps);
                    any = true;
                }
                if (!any) def.text += "        char none;\n";
                def.text += "    } u;\n};\n";
            }
            cur_ = saved;
            return def;
        }
        const auto& decl = s->decl->as<TypeDecl>();
        if (!decl.bases.empty() || s->has_opaque_base) unsupported(where, "inheritance and interfaces");
        for (const auto& [mname, overloads] : s->members) {
            for (const Symbol* m : overloads) {
                const FunctionDecl* f = m->function();
                if (!f) continue;
                if (f->is_destructor) unsupported(m->span, "destructors");
                if (f->is_virtual || f->is_abstract) unsupported(m->span, "virtual methods");
            }
        }
        def.text = std::format("struct {} {{\n", name);
        std::vector<const Symbol*> fields = fields_of(s);
        for (const Symbol* f : fields) {
            const Type* ft = lower(*f->decl->as<VariableDecl>().type);
            cur_           = f->span;
            def.text += std::format("    {} f_{};\n", ctype(ft), f->name);
            by_value_deps(ft, def.deps);
        }
        if (fields.empty()) def.text += "    char empty;\n";
        def.text += "};\n";
        cur_ = saved;
        return def;
    }

    // --- functions ------------------------------------------------------------------------

    struct FunctionContext {
        const Symbol*       symbol = nullptr;
        const Type*         result = nullptr;
        const Symbol*       self   = nullptr;   // enclosing type of a method with `this`
    };

    std::string signature(const Symbol* s) {
        const FunctionDecl* f = body_decl(s);
        const Type* result    = f->result ? lower(*f->result) : tc_.void_();
        std::string params;
        auto        add = [&](const std::string& p) { params += (params.empty() ? "" : ", ") + p; };
        if (s->parent && !f->is_static) add(type_name(s->parent) + "* self");
        for (const Param& p : f->params) {
            cur_ = p.span;
            add(std::format("{} v_{}", ctype(lower(*p.type)), p.name));
        }
        if (params.empty()) params = "void";
        return std::format("static {} {}({})", ctype(result), function_name(s), params);
    }

    void generate_function(const Symbol* s) {
        const FunctionDecl* f = body_decl(s);
        Span                saved = cur_;
        cur_                      = s->span;
        if (!f || !f->body) {
            if (f && f->is_extern) unsupported(s->span, "extern functions");
            else unsupported(s->span, "functions without a body");
            cur_ = saved;
            return;
        }
        if (f->is_template || (s->parent && s->parent->decl && s->parent->decl->is_template)) {
            unsupported(s->span, "templates");
            cur_ = saved;
            return;
        }
        if (f->result && f->result->kind == TypeKind::deduce) {
            unsupported(f->result->span, "'deduce' return types");
            cur_ = saved;
            return;
        }
        fn_          = FunctionContext{};
        fn_.symbol   = s;
        fn_.result   = f->result ? lower(*f->result) : tc_.void_();
        fn_.self     = s->parent && !f->is_static ? s->parent : nullptr;
        tmp_counter_ = 0;

        std::string sig = signature(s);
        prototypes_ += sig + ";\n";
        outs_.push_back(Out{"", 1});
        for (StmtPtr& st : f->body->statements) exec_stmt(*st);
        if (f->body->tail) exec(*f->body->tail);
        Out body = std::move(outs_.back());
        outs_.pop_back();
        bodies_ += std::format("{} {{\n{}}}\n\n", sig, body.text);
        cur_ = saved;
    }

    void generate_global(const Symbol* g) {
        const auto& v     = g->decl->as<VariableDecl>();
        Span        saved = cur_;
        cur_              = g->span;
        const Type* t     = lower(*v.type);
        fn_               = FunctionContext{};
        fn_.result        = tc_.void_();
        tmp_counter_      = 0;
        std::string name  = global_names_[g];
        GlobalDef   def;
        def.decl = std::format("static {} {};\n", ctype(t), name);
        outs_.push_back(Out{"", 1});
        if (v.init) line(std::format("{} = {};", name, value_as(*v.init, t)));
        def.init = std::move(outs_.back().text);
        outs_.pop_back();
        globals_[g] = std::move(def);
        cur_        = saved;
    }

    // --- output buffers -------------------------------------------------------------------

    struct Out {
        std::string text;
        int         indent = 0;
    };

    void line(std::string_view s) {
        Out& o = outs_.back();
        o.text.append(static_cast<std::size_t>(o.indent) * 4, ' ');
        o.text += s;
        o.text += '\n';
    }
    void indent() { ++outs_.back().indent; }
    void dedent() { --outs_.back().indent; }

    /// Runs `body` with its own buffer and returns a GNU statement expression `({ ... })`.
    std::string statement_expr(const std::function<void()>& body) {
        int base = outs_.back().indent;
        outs_.push_back(Out{"", base + 1});
        body();
        std::string text = std::move(outs_.back().text);
        outs_.pop_back();
        return "({\n" + text + std::string(static_cast<std::size_t>(base) * 4, ' ') + "})";
    }

    // --- statements -----------------------------------------------------------------------

    void exec_stmt(Stmt& s) {
        Span saved = cur_;
        cur_       = s.span;
        switch (s.kind) {
        case StmtKind::variable: exec_variable(s.as<VariableStmt>()); break;
        case StmtKind::assign: exec_assign(s.as<AssignStmt>()); break;
        case StmtKind::discard: line(std::format("(void)({});", value(*s.as<DiscardStmt>().value))); break;
        case StmtKind::expression: exec(*s.as<ExprStmt>().expr); break;
        }
        cur_ = saved;
    }

    void exec_variable(VariableStmt& v) {
        if (v.storage == Storage::static_) {
            unsupported(v.span, "'static' variables inside functions");
            return;
        }
        const Symbol* sym = v.symbol;
        if (!sym || !v.init) return;
        const Type* t = sym->type ? sym->type : lower(*v.type);
        // The initializer first: if it is unsupported, that is the error to show (not its type).
        std::size_t before = diags_.all().size();
        std::string init   = t->is(Kind::reference) ? address(*v.init) : value_as(*v.init, t);
        if (diags_.all().size() != before) return;
        line(std::format("{} {} = {};", ctype(t), local_name(sym), init));
    }

    void exec_assign(AssignStmt& a) {
        const Type* t = types::strip_ref(a.target->type);
        if (a.op == TokenKind::eq) {
            line(std::format("{} = {};", value(*a.target), value_as(*a.value, t)));
            return;
        }
        TokenKind op = compound_operator(a.op);
        if (op == TokenKind::error) {
            unsupported(a.span, "this assignment operator");
            return;
        }
        // Evaluate the place once: `a[f()] += 1` must call f once.
        std::string p   = tmp();
        std::string rhs = value_as(*a.value, op == TokenKind::shl || op == TokenKind::shr ? types::strip_ref(a.value->type) : t);
        line("{");
        indent();
        line(std::format("{}* {} = {};", ctype(t), p, address(*a.target)));
        line(std::format("*{} = {};", p, binary_op(op, t, "(*" + p + ")", rhs, a.span)));
        dedent();
        line("}");
    }

    static TokenKind compound_operator(TokenKind op) {
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
        default: return TokenKind::error;
        }
    }

    /// `e` as a statement.
    void exec(Expr& e) {
        Span saved = cur_;
        cur_       = e.span;
        switch (e.kind) {
        case ExprKind::block: {
            auto& b = e.as<BlockExpr>();
            line("{");
            indent();
            for (StmtPtr& s : b.statements) exec_stmt(*s);
            if (b.tail) exec(*b.tail);
            dedent();
            line("}");
            break;
        }
        case ExprKind::if_: exec_if(e.as<IfExpr>(), nullptr); break;
        case ExprKind::match: exec_match(e.as<MatchExpr>(), nullptr); break;
        case ExprKind::while_: {
            auto& w = e.as<WhileExpr>();
            line(std::format("while ({}) {{", condition(*w.condition)));
            indent();
            exec_block_contents(*w.body);
            dedent();
            line("}");
            break;
        }
        case ExprKind::for_: exec_for(e.as<ForExpr>()); break;
        case ExprKind::return_: exec_return(e.as<ReturnExpr>()); break;
        case ExprKind::break_: line("break;"); break;
        case ExprKind::continue_: line("continue;"); break;
        case ExprKind::call:
            if (exec_std_call(e.as<CallExpr>())) break;
            line(value(e) + ";");
            break;
        default: line(std::format("(void)({});", value(e))); break;
        }
        cur_ = saved;
    }

    void exec_block_contents(BlockExpr& b) {
        for (StmtPtr& s : b.statements) exec_stmt(*s);
        if (b.tail) exec(*b.tail);
    }

    /// Evaluates `e` and stores its value in `target` (nothing for void/never values).
    void exec_into(Expr& e, const std::string& target) {
        if (is_void_like(e.type)) {
            exec(e);
            return;
        }
        Span saved = cur_;
        cur_       = e.span;
        switch (e.kind) {
        case ExprKind::block: {
            auto& b = e.as<BlockExpr>();
            line("{");
            indent();
            for (StmtPtr& s : b.statements) exec_stmt(*s);
            if (b.tail) exec_into(*b.tail, target);
            dedent();
            line("}");
            break;
        }
        case ExprKind::if_: exec_if(e.as<IfExpr>(), &target); break;
        case ExprKind::match: exec_match(e.as<MatchExpr>(), &target); break;
        default: line(std::format("{} = {};", target, value_as(e, types::strip_ref(e.type)))); break;
        }
        cur_ = saved;
    }

    void exec_if(IfExpr& i, const std::string* target) {
        if (i.is_compile_time) {
            unsupported(i.span, "'if compile_time'");
            return;
        }
        line(std::format("if ({}) {{", condition(*i.condition)));
        indent();
        for (StmtPtr& s : i.then_block->statements) exec_stmt(*s);
        if (i.then_block->tail) {
            if (target) exec_into(*i.then_block->tail, *target);
            else exec(*i.then_block->tail);
        }
        dedent();
        if (i.else_branch) {
            line("} else {");
            indent();
            Expr& el = *i.else_branch;
            if (el.kind == ExprKind::block) {
                auto& b = el.as<BlockExpr>();
                for (StmtPtr& s : b.statements) exec_stmt(*s);
                if (b.tail) {
                    if (target) exec_into(*b.tail, *target);
                    else exec(*b.tail);
                }
            } else if (target) {
                exec_into(el, *target);
            } else {
                exec(el);
            }
            dedent();
        }
        line("}");
    }

    void exec_return(ReturnExpr& r) {
        if (!r.value) {
            line("return;");
            return;
        }
        const Type* result = fn_.result;
        if (result && result->is(Kind::reference)) {
            line(std::format("return {};", address(*r.value)));
        } else if (is_void_like(r.value->type) || is_void_like(result)) {
            exec(*r.value);
            line("return;");
        } else {
            line(std::format("return {};", value_as(*r.value, result)));
        }
    }

    void exec_for(ForExpr& f) {
        Expr* it = f.iterable.get();
        if (it->kind != ExprKind::range) {
            unsupported(it->span, "'for' over anything but a range 'a..b'");
            return;
        }
        auto&       r  = it->as<RangeExpr>();
        const Type* t  = f.symbol && f.symbol->type ? types::strip_ref(f.symbol->type) : lower(*f.type);
        std::string ct = ctype(t);
        std::string i = tmp(), end = tmp(), go = tmp();
        line("{");
        indent();
        line(std::format("{} {} = {};", ct, i, value_as(*r.lo, t)));
        line(std::format("{} {} = {};", ct, end, value_as(*r.hi, t)));
        if (r.inclusive) {
            // `go` turns false after the last value, so `a..=MAX` never overflows.
            line(std::format("for (bool {} = {} <= {}; {}; {} = {} != {}, {} = {} ? {} + 1 : {}) {{", go, i, end, go, go,
                             i, end, i, go, i, i));
        } else {
            line(std::format("for (; {} < {}; {} = {} + 1) {{", i, end, i, i));
        }
        indent();
        if (f.symbol) line(std::format("{} {} = {};", ct, local_name(f.symbol), i));
        exec_block_contents(*f.body);
        dedent();
        line("}");
        dedent();
        line("}");
    }

    // --- match ----------------------------------------------------------------------------

    void exec_match(MatchExpr& m, const std::string* target) {
        Expr* scrutinee = m.scrutinee.get();
        if (scrutinee->kind == ExprKind::unary && scrutinee->as<UnaryExpr>().op == UnaryOp::mut_marker) {
            unsupported(scrutinee->span, "'match (mut x)'");
            return;
        }
        const Type* t    = types::strip_ref(scrutinee->type);
        std::string s    = tmp();
        std::string done = tmp();
        line(std::format("{} {} = {};", ctype(t), s, value(*scrutinee)));
        line(std::format("bool {} = false;", done));
        for (MatchArm& arm : m.arms) {
            line(std::format("if (!{} && {}) {{", done, pattern_test(*arm.pattern, s, t)));
            indent();
            pattern_bind(*arm.pattern, s, t);
            if (arm.guard) {
                line(std::format("if ({}) {{", condition(*arm.guard)));
                indent();
            }
            line(std::format("{} = true;", done));
            if (target) exec_into(*arm.body, *target);
            else exec(*arm.body);
            if (arm.guard) {
                dedent();
                line("}");
            }
            dedent();
            line("}");
        }
        line(std::format("if (!{}) rift_panic({}, \"no match arm matched\");", done, loc(m.span)));
    }

    /// A C condition that is true when `pattern` matches the value `v` of type `t`.
    std::string pattern_test(Pattern& p, const std::string& v, const Type* t) {
        switch (p.kind) {
        case PatternKind::wildcard:
        case PatternKind::binding: return "1";
        case PatternKind::literal: {
            Expr& lit = *p.as<LiteralPattern>().literal;
            if (t->is(Kind::static_str) || (t->is(Kind::external) && t->name == "std::str")) {
                return std::format("rift_str_eq({}, {})", v, value(lit));
            }
            return std::format("({} == {})", v, value_as(lit, t));
        }
        case PatternKind::range: {
            auto& r = p.as<RangePattern>();
            std::string out = "(1";
            if (r.lo) out += std::format(" && {} >= {}", v, value_as(*r.lo, t));
            if (r.hi) out += std::format(" && {} {} {}", v, r.inclusive ? "<=" : "<", value_as(*r.hi, t));
            return out + ")";
        }
        case PatternKind::enum_alternative: {
            auto&         ep  = p.as<EnumPattern>();
            const Symbol* alt = ep.symbol;
            if (!alt || !alt->parent || alt->kind != SymbolKind::alternative) {
                unsupported(p.span, "this pattern (standard library enums)");
                return "0";
            }
            if (!has_payloads(alt->parent)) return std::format("({} == {})", v, tag_constant(alt));
            std::string out = std::format("({}.tag == {}", v, tag_constant(alt));
            if (ep.payload && alt->alternative->payload) {
                const Type* pt = lower(*alt->alternative->payload);
                out += " && " + pattern_test(*ep.payload, std::format("{}.u.p_{}", v, alt->name), pt);
            }
            return out + ")";
        }
        case PatternKind::struct_: {
            auto& sp = p.as<StructPattern>();
            if (!t->is(Kind::user)) {
                unsupported(p.span, "this pattern");
                return "0";
            }
            std::string out = "(1";
            for (FieldPattern& fp : sp.fields) {
                const Type* ft = field_type(t->symbol, fp.name);
                if (!ft || !fp.pattern) continue;
                out += " && " + pattern_test(*fp.pattern, std::format("{}.f_{}", v, fp.name), ft);
            }
            return out + ")";
        }
        case PatternKind::error: return "0";
        }
        return "0";
    }

    void pattern_bind(Pattern& p, const std::string& v, const Type* t) {
        switch (p.kind) {
        case PatternKind::binding: {
            const Symbol* s = p.as<BindingPattern>().symbol;
            if (s) line(std::format("{} {} = {};", ctype(t), local_name(s), v));
            return;
        }
        case PatternKind::enum_alternative: {
            auto&         ep  = p.as<EnumPattern>();
            const Symbol* alt = ep.symbol;
            if (ep.payload && alt && alt->alternative && alt->alternative->payload) {
                pattern_bind(*ep.payload, std::format("{}.u.p_{}", v, alt->name), lower(*alt->alternative->payload));
            }
            return;
        }
        case PatternKind::struct_:
            if (!t->is(Kind::user)) return;
            for (FieldPattern& fp : p.as<StructPattern>().fields) {
                const Type* ft = field_type(t->symbol, fp.name);
                if (ft && fp.pattern) pattern_bind(*fp.pattern, std::format("{}.f_{}", v, fp.name), ft);
            }
            return;
        default: return;
        }
    }

    const Type* field_type(const Symbol* type, const std::string& name) {
        auto it = type->members.find(name);
        if (it == type->members.end() || it->second.front()->kind != SymbolKind::field) return nullptr;
        return lower(*it->second.front()->decl->as<VariableDecl>().type);
    }

    // --- expressions ----------------------------------------------------------------------

    /// A C condition for a Rift condition (integers and pointers convert contextually, spec 02).
    std::string condition(Expr& e) {
        const Type* t = types::strip_ref(e.type);
        std::string v = value(e);
        if (t && (t->is_integer() || t->is(Kind::pointer))) return std::format("(({}) != 0)", v);
        return v;
    }

    /// `e` converted to `want` (a never-typed value becomes an unreachable placeholder).
    std::string value_as(Expr& e, const Type* want) {
        if (e.type && e.type->is(Kind::never) && want && !is_void_like(want)) {
            std::string ct = ctype(types::strip_ref(want));
            return statement_expr([&] {
                exec(e);
                line(std::format("*({}*)0;", ct));
            });
        }
        return value(e);
    }

    /// A pointer to `e`'s object; temporaries get storage in a compound literal.
    std::string address(Expr& e) {
        if (e.kind == ExprKind::unary) {
            UnaryOp op = e.as<UnaryExpr>().op;
            if (op == UnaryOp::mut_marker || op == UnaryOp::move) return address(*e.as<UnaryExpr>().operand);
        }
        if (is_place(e)) return std::format("(&{})", value(e));
        const Type* t = types::strip_ref(e.type);
        return std::format("(({}[1]){{ {} }})", ctype(t), value_as(e, t));
    }

    bool is_place(Expr& e) {
        switch (e.kind) {
        case ExprKind::path: {
            const Symbol* s = e.as<PathExpr>().symbol;
            return s && (s->kind == SymbolKind::local || s->kind == SymbolKind::global);
        }
        case ExprKind::this_: return true;
        case ExprKind::member: {
            auto& m = e.as<MemberExpr>();
            return m.arrow || is_place(*m.object) || (m.object->type && m.object->type->is(Kind::reference));
        }
        case ExprKind::unary: return e.as<UnaryExpr>().op == UnaryOp::deref;
        case ExprKind::call: return e.type && e.type->is(Kind::reference);
        default: return false;
        }
    }

    std::string value(Expr& e) {
        Span saved = cur_;
        cur_       = e.span;
        std::string out = value_inner(e);
        cur_            = saved;
        return out;
    }

    std::string value_inner(Expr& e) {
        switch (e.kind) {
        case ExprKind::int_literal: {
            const Type* t = types::strip_ref(e.type);
            if (!t || !t->is_integer()) {
                unsupported(e.span, std::format("an integer literal of type {}", type_text(t)));
                return "0";
            }
            return int_constant(e.as<LiteralExpr>().int_value, ctype(t));
        }
        case ExprKind::float_literal: return float_literal(e.as<LiteralExpr>(), types::strip_ref(e.type));
        case ExprKind::char_literal: {
            const std::string& text = e.as<LiteralExpr>().value;
            char32_t           cp   = 0;
            if (text.empty() || decode_utf8(text, 0, cp) == 0) cp = text.empty() ? 0 : static_cast<unsigned char>(text[0]);
            return std::format("((uint32_t){}u)", static_cast<std::uint32_t>(cp));
        }
        case ExprKind::string_literal: {
            auto& lit = e.as<LiteralExpr>();
            if (lit.token_kind == TokenKind::format_string_literal) {
                unsupported(e.span, "f-strings outside 'std::print', 'std::println', 'std::eprintln' and 'std::panic'");
                return "((rift_str){0})";
            }
            return std::format("RIFT_STR({}, {})", c_string(lit.value), lit.value.size());
        }
        case ExprKind::bool_literal: return e.as<LiteralExpr>().bool_value ? "true" : "false";
        case ExprKind::nullptr_literal: return "NULL";
        case ExprKind::this_: return "(*self)";
        case ExprKind::default_: return default_value(types::strip_ref(e.type), e.span);
        case ExprKind::path: return path_value(e.as<PathExpr>());
        case ExprKind::unary: return unary_value(e.as<UnaryExpr>());
        case ExprKind::binary: return binary_value(e.as<BinaryExpr>());
        case ExprKind::call: return call_value(e.as<CallExpr>());
        case ExprKind::member: return member_value(e.as<MemberExpr>());
        case ExprKind::cast: return cast_value(e.as<CastExpr>());
        case ExprKind::bit: {
            auto&       b = e.as<BitExpr>();
            const Type* t = types::strip_ref(e.type);
            if (!t || !t->is_integer()) {
                unsupported(e.span, "'bit(n)' without an integer type");
                return "0";
            }
            return std::format("rift_bit_{}((rift_i128)({}), {})", t->name, value(*b.operand), loc(e.span));
        }
        case ExprKind::struct_literal: return struct_literal(e.as<StructLiteralExpr>());
        case ExprKind::if_:
        case ExprKind::match:
        case ExprKind::block:
        case ExprKind::while_:
        case ExprKind::for_:
        case ExprKind::return_:
        case ExprKind::break_:
        case ExprKind::continue_: return block_value(e);
        case ExprKind::index:
            unsupported(e.span, "indexing (containers come with the standard library)");
            return "0";
        case ExprKind::range: unsupported(e.span, "ranges outside 'for' loops"); return "0";
        case ExprKind::try_: unsupported(e.span, "'?'"); return "0";
        case ExprKind::lambda: unsupported(e.span, "lambdas"); return "0";
        case ExprKind::error: return "0";
        }
        return "0";
    }

    std::string block_value(Expr& e) {
        if (is_void_like(e.type)) {
            return statement_expr([&] { exec(e); });
        }
        const Type* t = types::strip_ref(e.type);
        return statement_expr([&] {
            std::string r = tmp();
            line(std::format("{} {};", ctype(t), r));
            exec_into(e, r);
            line(r + ";");
        });
    }

    std::string float_literal(const LiteralExpr& lit, const Type* t) {
        std::string digits;
        for (char c : lit.text) {
            if (c != '_') digits += c;
        }
        if (!lit.suffix.empty() && digits.ends_with(lit.suffix)) digits.resize(digits.size() - lit.suffix.size());
        double v = std::strtod(digits.c_str(), nullptr);
        char   buf[64];
        std::snprintf(buf, sizeof buf, "%.17g", v);
        std::string text = buf;
        if (text.find_first_of(".en") == std::string::npos) text += ".0";
        if (text == "inf") text = "HUGE_VAL";
        return t && t->bits == 32 ? std::format("((float){})", text) : std::format("({})", text);
    }

    std::string default_value(const Type* t, Span where) {
        if (!t) return "0";
        switch (t->kind) {
        case Kind::int_:
        case Kind::float_:
        case Kind::char_: return std::format("(({})0)", ctype(t));
        case Kind::bool_: return "false";
        case Kind::pointer: return "NULL";
        case Kind::static_str: return "((rift_str){0})";
        case Kind::external:
            if (t->name == "std::str") return "((rift_str){0})";
            break;
        case Kind::user:
            if (t->symbol->kind == SymbolKind::struct_ || t->symbol->kind == SymbolKind::class_) {
                std::string out = std::format("(({}){{ ", ctype(t));
                bool        first = true;
                for (const Symbol* f : fields_of(t->symbol)) {
                    const auto& d  = f->decl->as<VariableDecl>();
                    const Type* ft = lower(*d.type);
                    std::string v  = d.init ? value_as(*d.init, ft) : default_value(ft, where);
                    out += std::format("{}.f_{} = {}", first ? "" : ", ", f->name, v);
                    first = false;
                }
                return out + (first ? "0 })" : " })");
            }
            break;
        default: break;
        }
        unsupported(where, std::format("'default' for {}", type_text(t)));
        return "0";
    }

    std::string path_value(PathExpr& pe) {
        const Symbol* s = pe.symbol;
        if (!s) {
            unsupported(pe.span, std::format("'{}'", ast::to_string(pe.path)), no_std);
            return "0";
        }
        switch (s->kind) {
        case SymbolKind::local:
            if (s->local_kind == LocalKind::capture) {
                unsupported(pe.span, "lambdas");
                return "0";
            }
            if (s->type && s->type->is(Kind::reference)) return std::format("(*{})", local_name(s));
            return local_name(s);
        case SymbolKind::global: return global_name(s);
        case SymbolKind::alternative:
            if (s->alternative->payload) {
                unsupported(pe.span, "enum alternatives with a payload used without a value");
                return "0";
            }
            return alternative_value(s, nullptr);
        case SymbolKind::function:
        case SymbolKind::method: unsupported(pe.span, "functions used as values"); return "0";
        default: unsupported(pe.span, std::format("'{}' as a value", ast::to_string(pe.path))); return "0";
        }
    }

    std::string member_value(MemberExpr& m) {
        const Symbol* f = m.symbol;
        if (!f || f->kind != SymbolKind::field) {
            unsupported(m.span, "this member access");
            return "0";
        }
        if (f->is_static) {
            unsupported(m.span, "static fields");
            return "0";
        }
        if (m.arrow) return std::format("({})->f_{}", value(*m.object), f->name);
        if (m.object->kind == ExprKind::this_) return std::format("self->f_{}", f->name);
        return std::format("({}).f_{}", value(*m.object), f->name);
    }

    // --- operators ------------------------------------------------------------------------

    std::string unary_value(UnaryExpr& u) {
        const Type* t = types::strip_ref(u.operand->type);
        switch (u.op) {
        case UnaryOp::negate:
            if (t && t->is_integer()) {
                if (u.operand->kind == ExprKind::int_literal) {   // `-128` is a literal, not a negation
                    const Type* rt = types::strip_ref(u.type);
                    return std::format("(({})((rift_u128)0 - {}))", ctype(rt),
                                       int_constant(u.operand->as<LiteralExpr>().int_value, "rift_u128"));
                }
                return std::format("rift_neg_{}({}, {})", t->name, value(*u.operand), loc(u.span));
            }
            return std::format("(-{})", value(*u.operand));
        case UnaryOp::logical_not: return std::format("(!{})", condition(*u.operand));
        case UnaryOp::bit_not: return std::format("(({})~{})", ctype(t), value(*u.operand));
        case UnaryOp::deref: return std::format("(*{})", value(*u.operand));
        case UnaryOp::address_of:
        case UnaryOp::address_of_mut: return address(*u.operand);
        case UnaryOp::move:
        case UnaryOp::mut_marker: return value(*u.operand);
        }
        return "0";
    }

    static bool is_str(const Type* t) {
        return t && (t->is(Kind::static_str) || (t->is(Kind::external) && t->name == "std::str"));
    }

    std::string binary_value(BinaryExpr& b) {
        const Type* t = types::strip_ref(b.lhs->type);
        if (b.op == TokenKind::amp_amp || b.op == TokenKind::pipe_pipe) {
            return std::format("({} {} {})", condition(*b.lhs), b.op == TokenKind::amp_amp ? "&&" : "||", condition(*b.rhs));
        }
        const Type* rt = types::strip_ref(b.rhs->type);
        std::vector<Operand> ops = {operand(*b.lhs, t), operand(*b.rhs, rt)};
        return sequenced(ops, [&](const std::vector<std::string>& v) { return binary_op(b.op, t, v[0], v[1], b.span); });
    }

    /// `a op b` for operand type `t`, with Rift's checked semantics.
    std::string binary_op(TokenKind op, const Type* t, const std::string& a, const std::string& b, Span span) {
        if (!t) return "0";
        bool integer = t->is_integer();
        switch (op) {
        case TokenKind::eq_eq:
        case TokenKind::bang_eq: {
            const char* sym = op == TokenKind::eq_eq ? "==" : "!=";
            if (is_str(t)) return std::format("({}rift_str_eq({}, {}))", op == TokenKind::eq_eq ? "" : "!", a, b);
            if (t->is(Kind::user) && !(t->symbol->kind == SymbolKind::enum_ && !has_payloads(t->symbol))) {
                unsupported(span, "user-defined operators");
                return "0";
            }
            return std::format("({} {} {})", a, sym, b);
        }
        case TokenKind::less:
        case TokenKind::less_eq:
        case TokenKind::greater:
        case TokenKind::greater_eq: {
            if (t->is(Kind::user) || is_str(t)) {
                unsupported(span, "comparing these values with '<' and friends");
                return "0";
            }
            const char* sym = op == TokenKind::less ? "<" : op == TokenKind::less_eq ? "<=" : op == TokenKind::greater ? ">" : ">=";
            return std::format("({} {} {})", a, sym, b);
        }
        case TokenKind::plus:
        case TokenKind::minus:
        case TokenKind::star:
        case TokenKind::slash:
        case TokenKind::percent: {
            if (integer) {
                const char* fn = op == TokenKind::plus ? "add" : op == TokenKind::minus ? "sub" : op == TokenKind::star ? "mul"
                               : op == TokenKind::slash ? "div" : "rem";
                return std::format("rift_{}_{}({}, {}, {})", fn, t->name, a, b, loc(span));
            }
            if (t->is_float()) {
                if (op == TokenKind::percent) return std::format("{}({}, {})", t->bits == 32 ? "fmodf" : "fmod", a, b);
                const char* sym = op == TokenKind::plus ? "+" : op == TokenKind::minus ? "-" : op == TokenKind::star ? "*" : "/";
                return std::format("({} {} {})", a, sym, b);
            }
            break;
        }
        case TokenKind::plus_percent:
        case TokenKind::minus_percent:
        case TokenKind::star_percent:
            if (integer) {
                const char* fn = op == TokenKind::plus_percent ? "wadd" : op == TokenKind::minus_percent ? "wsub" : "wmul";
                return std::format("rift_{}_{}({}, {})", fn, t->name, a, b);
            }
            break;
        case TokenKind::amp:
        case TokenKind::pipe:
        case TokenKind::caret:
            if (integer) {
                const char* sym = op == TokenKind::amp ? "&" : op == TokenKind::pipe ? "|" : "^";
                return std::format("(({})({} {} {}))", ctype(t), a, sym, b);
            }
            break;
        case TokenKind::shl:
        case TokenKind::shr:
            if (integer) {
                return std::format("rift_{}_{}({}, (rift_i128)({}), {})", op == TokenKind::shl ? "shl" : "shr", t->name, a, b,
                                   loc(span));
            }
            break;
        default: break;
        }
        unsupported(span, std::format("'{}' on {}", token_kind_spelling(op), type_text(t)));
        return "0";
    }

    // --- evaluation order -----------------------------------------------------------------

    struct Operand {
        std::string code;
        std::string ctype;
        Effect      effect = Effect::none;
    };

    Operand operand(Expr& e, const Type* want) {
        const Type* t = want ? want : types::strip_ref(e.type);
        return Operand{value_as(e, t), ctype(t), effect_of(e)};
    }

    Operand ref_operand(Expr& e) {
        return Operand{address(e), ctype(types::strip_ref(e.type)) + "*", effect_of(e)};
    }

    /// Builds an expression from operands, evaluating them left to right (C leaves the order
    /// of operands and arguments unspecified). Temporaries are needed when any operand has side
    /// effects, or when two operands could panic (which panic is reported must not vary).
    std::string sequenced(std::vector<Operand>& ops, const std::function<std::string(const std::vector<std::string>&)>& build) {
        std::vector<std::string> codes;
        bool                     side_effects = false;
        int                      may_panic    = 0;
        for (Operand& o : ops) {
            codes.push_back(o.code);
            side_effects = side_effects || o.effect == Effect::side_effects;
            may_panic += o.effect != Effect::none;
        }
        if (ops.size() < 2 || (!side_effects && may_panic < 2)) return build(codes);
        return statement_expr([&] {
            for (std::size_t i = 0; i < ops.size(); ++i) {
                std::string t = tmp();
                line(std::format("{} {} = {};", ops[i].ctype, t, ops[i].code));
                codes[i] = t;
            }
            line(build(codes) + ";");
        });
    }

    // --- calls ----------------------------------------------------------------------------

    std::string call_value(CallExpr& c) {
        if (is_std_output(c)) {
            return statement_expr([&] { exec_std_call(c); });
        }
        Expr& callee = *c.callee;
        if (callee.kind == ExprKind::path) {
            auto&         pe = callee.as<PathExpr>();
            const Symbol* s  = pe.symbol;
            if (!s) {
                unsupported(pe.span, std::format("'{}'", ast::to_string(pe.path)), no_std);
                return "0";
            }
            if (s->kind == SymbolKind::alternative) {
                if (c.args.size() != 1 || !s->alternative->payload) return alternative_value(s, nullptr);
                std::string payload = value_as(*c.args[0], lower(*s->alternative->payload));
                return alternative_value(s, &payload);
            }
            if (s->kind == SymbolKind::function || s->kind == SymbolKind::method) return call_function(s, nullptr, c);
            unsupported(c.span, "calling lambdas and function values");
            return "0";
        }
        if (callee.kind == ExprKind::member) {
            auto&         me  = callee.as<MemberExpr>();
            const Symbol* s   = me.symbol;
            const Type*   obj = types::strip_ref(me.object->type);
            std::string   name = me.name.segments.back().name;
            if (s && s->kind == SymbolKind::method) {
                if (me.arrow) {
                    unsupported(c.span, "method calls through raw pointers");
                    return "0";
                }
                return call_function(s, s->function() && s->function()->is_static ? nullptr : me.object.get(), c);
            }
            if (name == "clone" && c.args.empty() && obj && obj->is(Kind::user)) {
                return value(*me.object);   // only plain data is supported, so a clone is a copy
            }
            if (name == "len" && c.args.empty() && is_str(obj)) return std::format("({}).len", value(*me.object));
            if (name == "is_empty" && c.args.empty() && is_str(obj)) return std::format("(({}).len == 0)", value(*me.object));
            unsupported(c.span, std::format("the method '{}' on {}", name, type_text(obj)));
            return "0";
        }
        unsupported(c.span, "calling this expression");
        return "0";
    }

    std::string call_function(const Symbol* s, Expr* receiver, CallExpr& c) {
        const FunctionDecl* f = s->function();
        if (!f) return "0";
        if (f->is_template || (s->parent && s->parent->decl && s->parent->decl->is_template)) {
            unsupported(c.span, "calling templates");
            return "0";
        }
        if (f->is_virtual || f->is_abstract || (s->parent && s->parent->kind == SymbolKind::interface)) {
            unsupported(c.span, "virtual calls");
            return "0";
        }
        // No compile-time interpreter yet: compile_time functions are compiled like normal ones.
        std::vector<Operand> ops;
        if (receiver) ops.push_back(ref_operand(*receiver));
        for (std::size_t i = 0; i < c.args.size() && i < f->params.size(); ++i) {
            const Type* pt = lower(*f->params[i].type);
            if (pt->is(Kind::reference)) ops.push_back(ref_operand(*c.args[i]));
            else ops.push_back(operand(*c.args[i], pt));
        }
        std::string name   = function_name(s);
        const Type* result = f->result ? lower(*f->result) : tc_.void_();
        std::string call   = sequenced(ops, [&](const std::vector<std::string>& args) {
            std::string list;
            for (const std::string& a : args) list += (list.empty() ? "" : ", ") + a;
            return std::format("{}({})", name, list);
        });
        return result->is(Kind::reference) ? std::format("(*{})", call) : call;
    }

    // --- the standard library's output functions ------------------------------------------

    static std::string std_name(const CallExpr& c) {
        if (c.callee->kind != ExprKind::path) return {};
        const auto& pe = c.callee->as<PathExpr>();
        if (pe.symbol || pe.path.segments.size() != 2 || pe.path.segments[0].name != "std") return {};
        return pe.path.segments[1].name;
    }

    static bool is_std_output(const CallExpr& c) {
        std::string n = std_name(c);
        return n == "print" || n == "println" || n == "eprint" || n == "eprintln" || n == "panic";
    }

    /// std::print / println / eprint / eprintln / panic, written straight into the current
    /// buffer. Returns false for any other call.
    bool exec_std_call(CallExpr& c) {
        if (!is_std_output(c)) return false;
        std::string name    = std_name(c);
        bool        panic   = name == "panic";
        bool        newline = name.ends_with("ln");
        const char* stream  = name.starts_with("e") ? "stderr" : "stdout";
        if (c.args.size() != 1) {
            unsupported(c.span, std::format("'std::{}' with {} arguments", name, c.args.size()));
            return true;
        }
        Expr& arg = *c.args[0];
        if (arg.kind == ExprKind::string_literal && arg.as<LiteralExpr>().token_kind == TokenKind::format_string_literal) {
            std::string buf = tmp();
            line("{");
            indent();
            line(std::format("rift_buf {} = {{ 0, 0, 0 }};", buf));
            for (FormatPart& part : arg.as<LiteralExpr>().format_parts) {
                if (part.expr) format_hole(buf, part);
                else line(std::format("rift_buf_put(&{}, {}, {});", buf, c_string(part.text), part.text.size()));
            }
            if (panic) line(std::format("rift_panic_buf({}, &{});", loc(c.span), buf));
            else line(std::format("rift_flush({}, &{}, {});", stream, buf, newline ? "true" : "false"));
            dedent();
            line("}");
            return true;
        }
        const Type* t = types::strip_ref(arg.type);
        if (!is_str(t) && !(t && t->is(Kind::external) && t->name == "std::string")) {
            unsupported(arg.span, std::format("'std::{}' with a value of type {}", name, type_text(t)));
            return true;
        }
        if (!is_str(t)) {
            unsupported(arg.span, "'std::string'");
            return true;
        }
        if (panic) line(std::format("rift_panic_str({}, {});", loc(c.span), value(arg)));
        else line(std::format("rift_write_str({}, {}, {});", stream, value(arg), newline ? "true" : "false"));
        return true;
    }

    void format_hole(const std::string& buf, FormatPart& part) {
        Expr&       e = *part.expr;
        const Type* t = types::strip_ref(e.type);
        std::string spec = "&rift_no_spec";
        if (part.spec.present) {
            const FormatSpec& s = part.spec;
            spec = std::format("&(rift_spec){{ {}, {}, {}, {}, {} }}", s.align ? std::format("'{}'", s.align) : "0",
                               s.plus ? "true" : "false", s.width, s.precision, s.radix ? std::format("'{}'", s.radix) : "0");
        }
        std::string v = value(e);
        if (t && t->is_integer()) {
            const IntInfo* info = int_info(t->name);
            if (!t->is_signed) {
                line(std::format("rift_fmt_u(&{}, (rift_u128)({}), false, {});", buf, v, spec));
            } else if (part.spec.radix) {   // two's complement bits in another base
                line(std::format("rift_fmt_u(&{}, (rift_u128)({})({}), false, {});", buf, info->unsigned_ctype, v, spec));
            } else {
                line(std::format("rift_fmt_i(&{}, (rift_i128)({}), {});", buf, v, spec));
            }
        } else if (t && t->is_float()) {
            line(std::format("rift_fmt_f(&{}, (double)({}), {}, {});", buf, v, t->bits == 32 ? "true" : "false", spec));
        } else if (t && t->is(Kind::bool_)) {
            line(std::format("rift_fmt_bool(&{}, {}, {});", buf, v, spec));
        } else if (t && t->is(Kind::char_)) {
            line(std::format("rift_fmt_char(&{}, {}, {});", buf, v, spec));
        } else if (is_str(t)) {
            line(std::format("rift_fmt_str(&{}, {}, {});", buf, v, spec));
        } else {
            unsupported(e.span, std::format("formatting values of type {}", type_text(t)));
        }
    }

    // --- casts and struct literals --------------------------------------------------------

    std::string cast_value(CastExpr& c) {
        const Type* to   = types::strip_ref(c.type);
        const Type* from = types::strip_ref(c.operand->type);
        const std::string& variant = c.variant;
        if (variant == "checked" || variant == "dynamic") {
            unsupported(c.span, std::format("'cast::{}' (it returns std::option)", variant));
            return "0";
        }
        std::string v = value(*c.operand);
        if (variant == "reinterpret") {
            if (to->is(Kind::pointer) || from->is(Kind::pointer)) return std::format("(({})(uintptr_t)({}))", ctype(to), v);
            std::string ct = ctype(to), cf = ctype(from);
            return statement_expr([&] {
                std::string a = tmp(), r = tmp();
                line(std::format("{} {} = {};", cf, a, v));
                line(std::format("{} {};", ct, r));
                line(std::format("memcpy(&{}, &{}, sizeof {});", r, a, r));
                line(r + ";");
            });
        }
        if (variant == "saturate" && to->is_integer()) {
            const IntInfo* ti = int_info(to->name);
            if (from->is_float()) {
                std::string f = tmp();
                return statement_expr([&] {
                    line(std::format("double {} = (double)({});", f, v));
                    line(std::format("{} != {} ? ({})0 : {} <= (double){} ? ({}){} : {} >= (double){} ? ({}){} : ({}){};", f, f,
                                     ti->ctype, f, ti->min, ti->ctype, ti->min, f, ti->max, ti->ctype, ti->max, ti->ctype, f));
                });
            }
            if (from->is_integer()) {
                if (from->is_signed) {
                    if (to->is_signed) return std::format("(({})rift_sat_s((rift_i128)({}), {}, {}))", ti->ctype, v, ti->min, ti->max);
                    return std::format("(({})rift_sat_su((rift_i128)({}), {}))", ti->ctype, v, ti->max);
                }
                if (to->is_signed) return std::format("(({})rift_sat_us((rift_u128)({}), {}))", ti->ctype, v, ti->max);
                return std::format("(({})rift_sat_u((rift_u128)({}), {}))", ti->ctype, v, ti->max);
            }
        }
        if (variant == "round" && to->is_float()) return std::format("(({})({}))", ctype(to), v);
        // cast<T> (lossless) and cast::truncate (keeps the low bits) are plain C conversions.
        return std::format("(({})({}))", ctype(to), v);
    }

    std::string struct_literal(StructLiteralExpr& sl) {
        const Type* t = types::strip_ref(static_cast<Expr&>(sl).type);   // the value's type, not the written one
        if (!t || !t->is(Kind::user)) {
            unsupported(sl.span, std::format("building values of type {}", type_text(t)));
            return "0";
        }
        const Symbol* s = t->symbol;
        // Given fields in the order written (that is the evaluation order), then defaults.
        std::vector<Operand>     ops;
        std::vector<std::string> names;
        for (FieldInit& fi : sl.fields) {
            const Type* ft = field_type(s, fi.name);
            if (!ft) continue;
            ops.push_back(operand(*fi.value, ft));
            names.push_back(fi.name);
        }
        std::string tname = ctype(t);
        return sequenced(ops, [&](const std::vector<std::string>& vals) {
            std::string out = std::format("(({}){{ ", tname);
            bool        first = true;
            for (const Symbol* f : fields_of(s)) {
                std::string v;
                auto        given = std::find(names.begin(), names.end(), f->name);
                if (given != names.end()) {
                    v = vals[static_cast<std::size_t>(given - names.begin())];
                } else {
                    const auto& d  = f->decl->as<VariableDecl>();
                    const Type* ft = lower(*d.type);
                    v              = d.init ? value_as(*d.init, ft) : default_value(ft, sl.span);
                }
                out += std::format("{}.f_{} = {}", first ? "" : ", ", f->name, v);
                first = false;
            }
            return out + (first ? "0 })" : " })");
        });
    }

    // --- assembling the translation unit --------------------------------------------------

    struct GlobalDef {
        std::string decl;
        std::string init;
    };

    std::string assemble(const Symbol* main) {
        std::ostringstream out;
        out << c_runtime << "\n/* ---- types ---- */\n\n";
        for (const Symbol* s : type_order_) {
            if (s->kind == SymbolKind::enum_ && !has_payloads(s)) continue;   // a typedef of the tag type
            out << std::format("typedef struct {} {};\n", type_names_[s], type_names_[s]);
        }
        std::set<const Symbol*> written;
        std::function<void(const Symbol*)> write = [&](const Symbol* s) {
            if (!written.insert(s).second) return;
            for (const Symbol* d : type_defs_[s].deps) write(d);
            out << type_defs_[s].text << '\n';
        };
        for (const Symbol* s : type_order_) write(s);

        out << "/* ---- source locations ---- */\n\n";
        std::vector<std::pair<std::size_t, std::pair<FileId, std::uint32_t>>> locs;
        for (const auto& [key, index] : locs_) locs.emplace_back(index, key);
        std::sort(locs.begin(), locs.end());
        for (const auto& [index, key] : locs) {
            const SourceFile& file = sources_.file(key.first);
            LineCol           lc   = file.line_col(key.second);
            out << std::format("static const rift_loc rift_loc_{} = {{ {}, {}, {} }};\n", index, c_string(file.path()), lc.line,
                               lc.column);
        }

        out << "\n/* ---- globals ---- */\n\n";
        std::vector<const Module*> order = module_order();
        std::vector<const Symbol*> globals;
        for (const Module* m : order) {
            for (const Symbol* s : m->item_order) {
                if (globals_.count(s)) globals.push_back(s);
            }
        }
        for (const Symbol* g : globals) out << globals_[g].decl;
        out << "\nstatic void rift_init_globals(void) {\n";
        for (const Symbol* g : globals) out << globals_[g].init;
        out << "}\n\n/* ---- functions ---- */\n\n" << prototypes_ << '\n' << bodies_;

        const FunctionDecl* f      = main ? body_decl(main) : nullptr;
        const Type*         result = f && f->result ? lower(*f->result) : tc_.void_();
        out << "int main(int argc, char** argv) {\n    (void)argc;\n    (void)argv;\n    rift_init_globals();\n";
        if (!main) {
            out << "    return 0;\n";
        } else if (result->is_integer()) {
            out << std::format("    return rift_exit_status((rift_i128){}(), {});\n", main_name_, loc_text(main->span));
        } else {
            out << std::format("    {}();\n    return 0;\n", main_name_);
        }
        out << "}\n";
        return out.str();
    }

    /// A location spelled inline (main's wrapper is written after the location table).
    std::string loc_text(Span span) {
        const SourceFile& file = sources_.file(span.file);
        LineCol           lc   = file.line_col(span.begin);
        return std::format("&(rift_loc){{ {}, {}, {} }}", c_string(file.path()), lc.line, lc.column);
    }

    Program&              program_;
    types::TypeContext&   tc_;
    const SourceManager&  sources_;
    DiagnosticEngine&     diags_;
    CodegenOptions        options_;
    Span                  cur_{};
    FunctionContext       fn_;
    int                   tmp_counter_ = 0;
    std::vector<Out>      outs_;
    std::string           main_name_;
    std::string           prototypes_;
    std::string           bodies_;

    std::map<const Symbol*, std::string> fn_names_;
    std::map<const Symbol*, std::string> type_names_;
    std::vector<const Symbol*>           type_order_;   // registration order: stable output
    std::map<const Symbol*, std::string> global_names_;
    std::deque<const Symbol*>            fn_queue_;
    std::deque<const Symbol*>            type_queue_;
    std::deque<const Symbol*>            global_queue_;
    std::map<const Symbol*, TypeDef>     type_defs_;
    std::map<const Symbol*, GlobalDef>   globals_;
    std::map<std::pair<FileId, std::uint32_t>, std::size_t> locs_;
    std::set<std::tuple<FileId, std::uint32_t, std::string>> reported_;
};

} // namespace

std::string generate_c(sema::Program& program, const SourceManager& sources, DiagnosticEngine& diags,
                       CodegenOptions options) {
    return Generator(program, sources, diags, options).run();
}

} // namespace rift

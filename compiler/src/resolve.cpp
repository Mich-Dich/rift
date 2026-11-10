#include "rift/resolve.hpp"

#include "rift/ast_printer.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <ostream>
#include <set>

namespace rift {

using namespace ast;
using sema::Module;
using sema::Program;
using sema::Symbol;
using sema::SymbolKind;
using sema::LocalKind;

namespace sema {
std::string_view to_string(SymbolKind kind) {
    switch (kind) {
    case SymbolKind::function: return "function";
    case SymbolKind::global: return "variable";
    case SymbolKind::struct_: return "struct";
    case SymbolKind::class_: return "class";
    case SymbolKind::interface: return "interface";
    case SymbolKind::enum_: return "enum";
    case SymbolKind::alternative: return "enum alternative";
    case SymbolKind::field: return "field";
    case SymbolKind::method: return "method";
    case SymbolKind::type_param: return "template type parameter";
    case SymbolKind::value_param: return "template value parameter";
    case SymbolKind::local: return "local variable";
    }
    return "?";
}
} // namespace sema

namespace {

// --- signatures ---------------------------------------------------------------------------

/// Parameter types and trailing `mut`: what overloads and header/source pairs are matched on.
std::string params_sig(const FunctionDecl& f) {
    std::string s = "(";
    for (std::size_t i = 0; i < f.params.size(); ++i) {
        s += (i ? ", " : "") + to_string(*f.params[i].type);
    }
    s += ")";
    return f.is_mut ? s + " mut" : s;
}

std::string result_sig(const FunctionDecl& f) { return f.result ? to_string(*f.result) : ""; }

std::string template_sig(const Decl& d) {
    if (!d.is_template) {
        return "";
    }
    std::string s = "template<";
    for (std::size_t i = 0; i < d.template_params.size(); ++i) {
        const TemplateParam& p = d.template_params[i];
        s += i ? ", " : "";
        if (p.is_type) {
            s += "type " + p.name;
            for (std::size_t c = 0; c < p.constraints.size(); ++c) {
                s += (c ? " & " : " : ") + to_string(*p.constraints[c]);
            }
        } else {
            s += to_string(*p.value_type) + " " + p.name;
        }
    }
    return s + ">";
}

std::string bases_sig(const TypeDecl& t) {
    std::string s;
    for (std::size_t i = 0; i < t.bases.size(); ++i) {
        s += (i ? ", " : "") + to_string(*t.bases[i]);
    }
    return s;
}

std::string describe_fn(const Symbol& s) {
    const FunctionDecl* f = s.function();
    std::string         prefix = s.parent ? s.parent->name + "::" : "";
    return f ? prefix + s.name + params_sig(*f) : prefix + s.name;
}

/// Edit distance where swapping two adjacent characters counts as one edit ("inptu" -> "input").
std::size_t edit_distance(std::string_view a, std::string_view b) {
    std::vector<std::vector<std::size_t>> d(a.size() + 1, std::vector<std::size_t>(b.size() + 1));
    for (std::size_t i = 0; i <= a.size(); ++i) d[i][0] = i;
    for (std::size_t j = 0; j <= b.size(); ++j) d[0][j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        for (std::size_t j = 1; j <= b.size(); ++j) {
            std::size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) {
                d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + 1);
            }
        }
    }
    return d[a.size()][b.size()];
}

std::optional<std::string> closest(std::string_view name, const std::vector<std::string>& candidates) {
    std::size_t best_dist = std::max<std::size_t>(1, name.size() / 3) + 1;
    std::optional<std::string> best;
    for (const std::string& c : candidates) {
        if (c == name) {
            continue;
        }
        std::size_t d = edit_distance(name, c);
        if (d < best_dist) {
            best_dist = d;
            best      = c;
        }
    }
    return best;
}

bool is_builtin_type_name(std::string_view name) {
    std::optional<TokenKind> k = keyword_kind(name);
    return k && is_type_keyword(*k);
}

enum class Want { value, type };

struct ExprFlags {
    bool allow_mut_marker = false;
    bool is_callee        = false;
};

struct Scope {
    std::map<std::string, Symbol*> names;
    bool                           lambda_boundary = false;
};

/// What we know while resolving inside one function body.
struct FunctionContext {
    Symbol* self_type   = nullptr;   // enclosing struct/class/interface
    bool    has_this    = false;
    bool    is_static   = false;
    int     loop_depth  = 0;
};

class Resolver {
public:
    Resolver(Program& program, DiagnosticEngine& diags) : program_(program), diags_(diags) {}

    void run() {
        for (auto& [name, m] : program_.modules) collect(*m);
        for (auto& [name, m] : program_.modules) resolve_bases(*m);
        for (auto& [name, m] : program_.modules) check_hierarchy(*m);
        for (auto& [name, m] : program_.modules) resolve_bodies(*m);
    }

private:
    // --- diagnostics ----------------------------------------------------------------------

    Diagnostic& error(DiagCode code, Span span, std::string message, std::string label = {}) {
        return diags_.report(code, span, std::move(message), std::move(label));
    }
    static void note(Diagnostic& d, std::string message, std::optional<Span> span = std::nullopt) {
        d.notes.push_back(Note{std::move(message), span});
    }
    static void suggest(Diagnostic& d, std::string_view name, const std::vector<std::string>& candidates) {
        if (std::optional<std::string> best = closest(name, candidates)) {
            note(d, std::format("did you mean '{}'?", *best));
        }
    }

    // --- phase A: collect declarations, merge header and source ---------------------------

    void collect(Module& m) {
        module_ = &m;
        if (m.header) {
            check_header(m);
            for (const DeclPtr& d : m.header->ast.decls) {
                add_item(m, *d, /*from_header=*/true);
            }
        }
        if (m.source) {
            for (const DeclPtr& d : m.source->ast.decls) {
                if (m.header) {
                    merge_source_item(m, *d);
                } else {
                    add_item(m, *d, /*from_header=*/false);
                }
            }
        }
        check_definitions(m);
        check_duplicates(m.items, nullptr);
        for (Symbol* s : m.item_order) {
            if (s->is_type()) {
                check_duplicates(s->members, s);
            }
        }
    }

    void check_header(Module& m) {
        for (const DeclPtr& d : m.header->ast.decls) {
            if (d->is_private) {
                note(error(DiagCode::E3012, d->name_span, std::format("'{}' is marked 'private' in a header", d->name),
                           "private item"),
                     "private items live only in the .rifts");
            }
            if (d->kind == DeclKind::function && d->as<FunctionDecl>().body) {
                note(error(DiagCode::E3011, d->as<FunctionDecl>().body->span,
                           std::format("function '{}' has a body in a header", d->name), "body"),
                     "declare it here as 'T f(...);' and move the body into the .rifts");
            }
            if (d->kind == DeclKind::struct_ || d->kind == DeclKind::class_) {
                const auto& t = d->as<TypeDecl>();
                for (const DeclPtr& mem : t.members) {
                    if (mem->kind == DeclKind::function && mem->as<FunctionDecl>().body) {
                        note(error(DiagCode::E3011, mem->as<FunctionDecl>().body->span,
                                   std::format("method '{}::{}' has a body in a header", d->name, mem->name), "body"),
                             "method bodies are defined only inside the type's block in the .rifts");
                    }
                    if (d->kind == DeclKind::class_ &&
                        (mem->access == Access::private_ || mem->access == Access::none)) {
                        Diagnostic& e = error(DiagCode::E3012, mem->name_span,
                                              std::format("'{}::{}' is private in a header", d->name, mem->name),
                                              "private member");
                        note(e, mem->access == Access::none
                                    ? "class members in a header go under 'public:' or 'protected:'"
                                    : "private members are declared only in the class block of the .rifts");
                    }
                }
            }
        }
    }

    Symbol* add_item(Module& m, const Decl& d, bool from_header) {
        Symbol* sym = nullptr;
        switch (d.kind) {
        case DeclKind::import: return nullptr;
        case DeclKind::function: {
            sym = m.make(SymbolKind::function, d.name, d.name_span);
            sym->is_static = d.as<FunctionDecl>().is_static;
            break;
        }
        case DeclKind::variable:
        case DeclKind::field: {
            sym = m.make(SymbolKind::global, d.name, d.name_span);
            sym->is_mut = d.as<VariableDecl>().is_mut;
            break;
        }
        case DeclKind::struct_:
        case DeclKind::class_:
        case DeclKind::interface: {
            SymbolKind k = d.kind == DeclKind::struct_ ? SymbolKind::struct_
                         : d.kind == DeclKind::class_  ? SymbolKind::class_
                                                       : SymbolKind::interface;
            sym = m.make(k, d.name, d.name_span);
            for (const DeclPtr& mem : d.as<TypeDecl>().members) {
                add_member(m, sym, *mem, from_header);
            }
            break;
        }
        case DeclKind::enum_: {
            sym = m.make(SymbolKind::enum_, d.name, d.name_span);
            for (const EnumAlternative& alt : d.as<EnumDecl>().alternatives) {
                Symbol* a      = m.make(SymbolKind::alternative, alt.name, alt.span);
                a->parent      = sym;
                a->alternative = &alt;
                a->in_header   = from_header;
                sym->members[alt.name].push_back(a);
                sym->member_order.push_back(a);
            }
            break;
        }
        }
        sym->decl       = &d;
        sym->definition = from_header ? nullptr : &d;
        sym->in_header  = from_header;
        sym->is_private = d.is_private || (!from_header && m.header.has_value());
        m.items[d.name].push_back(sym);
        m.item_order.push_back(sym);
        symbol_of_[&d] = sym;
        return sym;
    }

    static ast::Access member_access(const Symbol* type, const Decl& mem) {
        if (type->kind != SymbolKind::class_) {
            return Access::public_;
        }
        return mem.access == Access::none ? Access::private_ : mem.access;
    }

    Symbol* add_member(Module& m, Symbol* type, const Decl& mem, bool from_header) {
        bool    is_fn = mem.kind == DeclKind::function;
        Symbol* sym   = m.make(is_fn ? SymbolKind::method : SymbolKind::field, mem.name, mem.name_span);
        sym->parent     = type;
        sym->decl       = &mem;
        sym->definition = from_header ? nullptr : &mem;
        sym->in_header  = from_header;
        sym->access     = member_access(type, mem);
        sym->is_static  = is_fn ? mem.as<FunctionDecl>().is_static
                                : mem.as<VariableDecl>().storage != Storage::local;
        type->members[mem.name].push_back(sym);
        type->member_order.push_back(sym);
        symbol_of_[&mem] = sym;
        return sym;
    }

    Symbol* header_item(Module& m, const std::string& name, SymbolKind kind) {
        auto it = m.items.find(name);
        if (it == m.items.end()) return nullptr;
        for (Symbol* s : it->second) {
            if (s->in_header && s->kind == kind) return s;
        }
        return nullptr;
    }

    std::vector<Symbol*> header_items(Module& m, const std::string& name) {
        std::vector<Symbol*> out;
        if (auto it = m.items.find(name); it != m.items.end()) {
            for (Symbol* s : it->second) {
                if (s->in_header) out.push_back(s);
            }
        }
        return out;
    }

    void not_in_header(const Decl& d, std::string_view what) {
        Diagnostic& e = error(DiagCode::E3017, d.name_span,
                              std::format("{} '{}' is not declared in the header", what, d.name),
                              "not in the .rifth");
        note(e, "mark it 'private' if it is local to this module, or declare it in the .rifth");
    }

    /// Header and source versions of one function must agree on everything but the body.
    void check_signature(const FunctionDecl& header, const FunctionDecl& source) {
        std::vector<std::string> diffs;
        if (result_sig(header) != result_sig(source)) {
            diffs.push_back(std::format("return type '{}' vs '{}'", result_sig(header), result_sig(source)));
        }
        if (header.is_static != source.is_static) diffs.push_back("'static'");
        if (header.is_virtual != source.is_virtual) diffs.push_back("'virtual'");
        if (header.is_override != source.is_override) diffs.push_back("'override'");
        if (header.is_compile_time != source.is_compile_time) diffs.push_back("'compile_time'");
        if (header.is_unsafe != source.is_unsafe) diffs.push_back("'unsafe'");
        if (template_sig(header) != template_sig(source)) diffs.push_back("template parameters");
        if (diffs.empty()) return;
        std::string list;
        for (std::size_t i = 0; i < diffs.size(); ++i) list += (i ? ", " : "") + diffs[i];
        Diagnostic& e = error(DiagCode::E3014, source.name_span,
                              std::format("'{}' differs from its header declaration: {}", source.name, list),
                              "differs from the header");
        note(e, "declared in the header here", header.name_span);
    }

    void merge_source_item(Module& m, const Decl& d) {
        switch (d.kind) {
        case DeclKind::import: return;
        case DeclKind::function: {
            const auto&          fn    = d.as<FunctionDecl>();
            std::vector<Symbol*> cands = header_items(m, d.name);
            std::erase_if(cands, [](Symbol* s) { return s->kind != SymbolKind::function; });
            auto match = std::find_if(cands.begin(), cands.end(), [&](Symbol* s) {
                return params_sig(*s->function()) == params_sig(fn);
            });
            if (match != cands.end()) {
                Symbol* s = *match;
                if (s->definition) {
                    note(error(DiagCode::E3010, d.name_span, std::format("'{}' is defined twice", d.name), "second definition"),
                         "first definition here", s->definition->name_span);
                    return;
                }
                check_signature(*s->function(), fn);
                s->definition  = &d;
                symbol_of_[&d] = s;
                return;
            }
            if (d.is_private) {
                add_item(m, d, false);
            } else if (!cands.empty()) {
                Diagnostic& e = error(DiagCode::E3014, d.name_span,
                                      std::format("'{}{}' does not match its header declaration", d.name, params_sig(fn)),
                                      "different parameters");
                note(e, std::format("the header declares '{}'", describe_fn(*cands.front())), cands.front()->span);
                for (Symbol* c : cands) mismatched_.insert(c);   // no extra "never defined"
            } else {
                not_in_header(d, "function");
            }
            return;
        }
        case DeclKind::struct_:
        case DeclKind::class_: {
            SymbolKind k    = d.kind == DeclKind::struct_ ? SymbolKind::struct_ : SymbolKind::class_;
            Symbol*    same = header_item(m, d.name, k);
            if (same) {
                merge_type(m, same, d.as<TypeDecl>());
                return;
            }
            std::vector<Symbol*> other = header_items(m, d.name);
            if (!other.empty()) {
                note(error(DiagCode::E3014, d.name_span,
                           std::format("'{}' is a {} here but a {} in the header", d.name,
                                       d.kind == DeclKind::struct_ ? "struct" : "class",
                                       sema::to_string(other.front()->kind)),
                           "different kind"),
                     "declared in the header here", other.front()->span);
                return;
            }
            if (d.is_private) add_item(m, d, false);
            else not_in_header(d, d.kind == DeclKind::struct_ ? "struct" : "class");
            return;
        }
        case DeclKind::interface:
        case DeclKind::enum_: {
            std::vector<Symbol*> other = header_items(m, d.name);
            if (!other.empty()) {
                note(error(DiagCode::E3018, d.name_span,
                           std::format("'{}' is already complete in the header", d.name), "defined again"),
                     "enums and interfaces are written only in the .rifth", other.front()->span);
                return;
            }
            if (d.is_private) add_item(m, d, false);
            else not_in_header(d, d.kind == DeclKind::enum_ ? "enum" : "interface");
            return;
        }
        case DeclKind::variable:
        case DeclKind::field: {
            std::vector<Symbol*> other = header_items(m, d.name);
            if (!other.empty()) {
                note(error(DiagCode::E3010, d.name_span, std::format("'{}' is already defined in the header", d.name),
                           "defined again"),
                     "header definition here", other.front()->span);
                return;
            }
            if (d.is_private) add_item(m, d, false);
            else not_in_header(d, "variable");
            return;
        }
        }
    }

    void merge_type(Module& m, Symbol* type, const TypeDecl& src) {
        symbol_of_[&src] = type;
        const auto& header = type->decl->as<TypeDecl>();
        if (template_sig(header) != template_sig(src)) {
            note(error(DiagCode::E3014, src.name_span,
                       std::format("template parameters of '{}' differ from the header", src.name), "differs"),
                 "declared in the header here", header.name_span);
        }
        if (!src.bases.empty() && bases_sig(src) != bases_sig(header)) {
            note(error(DiagCode::E3014, src.name_span, std::format("bases of '{}' differ from the header", src.name),
                       "differs"),
                 "declared in the header here", header.name_span);
        }
        bool is_struct = type->kind == SymbolKind::struct_;
        for (const DeclPtr& mem : src.members) {
            Access access = member_access(type, *mem);
            std::vector<Symbol*> cands;
            if (auto it = type->members.find(mem->name); it != type->members.end()) {
                for (Symbol* s : it->second) {
                    if (s->in_header) cands.push_back(s);
                }
            }
            if (mem->kind == DeclKind::function) {
                const auto& fn = mem->as<FunctionDecl>();
                std::erase_if(cands, [](Symbol* s) { return s->kind != SymbolKind::method; });
                auto match = std::find_if(cands.begin(), cands.end(), [&](Symbol* s) {
                    return params_sig(*s->function()) == params_sig(fn);
                });
                if (match != cands.end()) {
                    Symbol* s = *match;
                    if (s->definition) {
                        note(error(DiagCode::E3010, mem->name_span,
                                   std::format("'{}::{}' is defined twice", type->name, mem->name), "second definition"),
                             "first definition here", s->definition->name_span);
                        continue;
                    }
                    if (!is_struct && s->access != access) {
                        note(error(DiagCode::E3014, mem->name_span,
                                   std::format("'{}::{}' is in a different access section than in the header",
                                               type->name, mem->name),
                                   "different access"),
                             "declared in the header here", s->span);
                    }
                    check_signature(*s->function(), fn);
                    s->definition    = mem.get();
                    symbol_of_[mem.get()] = s;
                } else if (!is_struct && access == Access::private_) {
                    add_member(m, type, *mem, false);
                } else if (!cands.empty()) {
                    Diagnostic& e = error(DiagCode::E3014, mem->name_span,
                                          std::format("'{}::{}{}' does not match its header declaration",
                                                      type->name, mem->name, params_sig(fn)),
                                          "different parameters");
                    note(e, std::format("the header declares '{}'", describe_fn(*cands.front())), cands.front()->span);
                    for (Symbol* c : cands) mismatched_.insert(c);
                } else {
                    Diagnostic& e = error(DiagCode::E3015, mem->name_span,
                                          std::format("'{}::{}' is not declared in the header", type->name, mem->name),
                                          "not in the .rifth");
                    note(e, is_struct ? "declare every method of a struct in its .rifth block"
                                      : "declare it under 'public:' in the .rifth, or move it under 'private:'");
                }
                continue;
            }
            // Fields.
            if (is_struct) {
                note(error(DiagCode::E3016, mem->name_span,
                           std::format("struct '{}' cannot add field '{}' in its .rifts block", type->name, mem->name),
                           "new field"),
                     "all fields of a struct are declared in the header");
            } else if (access == Access::private_) {
                add_member(m, type, *mem, false);
            } else if (!cands.empty()) {
                note(error(DiagCode::E3010, mem->name_span,
                           std::format("field '{}::{}' is already declared in the header", type->name, mem->name),
                           "declared again"),
                     "header declaration here", cands.front()->span);
            } else {
                note(error(DiagCode::E3015, mem->name_span,
                           std::format("public field '{}::{}' is not declared in the header", type->name, mem->name),
                           "not in the .rifth"),
                     "public and protected members are declared in the .rifth; private ones go under 'private:'");
            }
        }
    }

    static bool needs_body(const FunctionDecl& f) {
        return !f.is_extern && !f.is_defaulted && !f.is_abstract;
    }

    void check_definitions(Module& m) {
        auto check = [&](Symbol* s, std::string_view display) {
            const FunctionDecl* f = s->function();
            if (!f || !needs_body(*f)) return;
            if (s->in_header && !s->definition) {
                if (f->body || mismatched_.count(s)) return;   // already reported (E3011 / E3014)
                Diagnostic& e = error(DiagCode::E3013, s->span,
                                      std::format("'{}' is declared in the header but never defined", display),
                                      "no definition");
                note(e, m.source ? "define it in the module's .rifts"
                                 : "this module has no .rifts to define it in");
            } else if (!s->in_header && s->definition && !s->definition->as<FunctionDecl>().body) {
                error(DiagCode::E3013, s->span, std::format("'{}' has no body", display), "add '{ ... }'");
            }
        };
        for (Symbol* s : m.item_order) {
            check(s, s->name);
            if (s->kind == SymbolKind::struct_ || s->kind == SymbolKind::class_) {
                for (Symbol* mem : s->member_order) {
                    check(mem, s->name + "::" + mem->name);
                }
            }
        }
    }

    void check_duplicates(const std::map<std::string, std::vector<Symbol*>>& table, const Symbol* owner) {
        for (const auto& [name, syms] : table) {
            for (std::size_t i = 1; i < syms.size(); ++i) {
                for (std::size_t j = 0; j < i; ++j) {
                    const Symbol* a = syms[j];
                    const Symbol* b = syms[i];
                    bool overloadable = (a->kind == SymbolKind::function || a->kind == SymbolKind::method) &&
                                        a->kind == b->kind;
                    if (overloadable && params_sig(*a->function()) != params_sig(*b->function())) {
                        continue;
                    }
                    std::string display = owner ? owner->name + "::" + name : name;
                    Diagnostic& e = error(DiagCode::E3010, b->span,
                                          overloadable ? std::format("'{}' is defined twice with the same parameters", display)
                                                       : std::format("'{}' is defined more than once", display),
                                          "duplicate");
                    note(e, "first definition here", a->span);
                    break;
                }
            }
        }
    }

    // --- file context and template parameters --------------------------------------------

    struct FileScope {
        Resolver& r;
        Module*   saved_module;
        bool      saved_header;
        FileScope(Resolver& res, Module* m, bool header)
            : r(res), saved_module(res.module_), saved_header(res.in_header_) {
            r.module_    = m;
            r.in_header_ = header;
        }
        ~FileScope() {
            r.module_    = saved_module;
            r.in_header_ = saved_header;
        }
        FileScope(const FileScope&)            = delete;
        FileScope& operator=(const FileScope&) = delete;
    };

    /// Pushes a declaration's template parameters for the duration of a scope.
    struct TemplateScope {
        Resolver& r;
        bool      pushed = false;
        TemplateScope(Resolver& res, const Decl& d, bool resolve_constraints = true) : r(res) {
            if (!d.is_template) return;
            pushed = true;
            r.template_params_.emplace_back();
            for (const TemplateParam& p : d.template_params) {
                Symbol* s = r.module_->make(p.is_type ? SymbolKind::type_param : SymbolKind::value_param, p.name, p.span);
                r.template_params_.back().push_back(s);
            }
            // Constraints and value types may refer to the parameters themselves.
            if (!resolve_constraints) return;
            for (const TemplateParam& p : d.template_params) {
                for (const TypePtr& c : p.constraints) r.resolve_type(*c);
                if (p.value_type) r.resolve_type(*p.value_type);
            }
        }
        ~TemplateScope() {
            if (pushed) r.template_params_.pop_back();
        }
        TemplateScope(const TemplateScope&)            = delete;
        TemplateScope& operator=(const TemplateScope&) = delete;
    };

    Symbol* find_template_param(const std::string& name) {
        for (auto it = template_params_.rbegin(); it != template_params_.rend(); ++it) {
            for (Symbol* s : *it) {
                if (s->name == name) return s;
            }
        }
        return nullptr;
    }

    Module* visible_module(const std::string& name) const {
        for (Module* m : module_->header_imports) {
            if (m->name == name) return m;
        }
        if (!in_header_) {
            for (Module* m : module_->source_imports) {
                if (m->name == name) return m;
            }
        }
        return nullptr;
    }

    bool std_visible() const {
        return module_->header_imports_std || (!in_header_ && module_->source_imports_std);
    }

    // --- phase B and C: bases and class hierarchies --------------------------------------

    void resolve_bases(Module& m) {
        for (Symbol* t : m.item_order) {
            if (t->kind != SymbolKind::struct_ && t->kind != SymbolKind::class_ && t->kind != SymbolKind::interface) {
                continue;
            }
            FileScope     fs(*this, &m, t->in_header);
            TemplateScope ts(*this, *t->decl, /*resolve_constraints=*/false);   // phase D reports those
            for (const TypePtr& base : t->decl->as<TypeDecl>().bases) {
                resolve_type(*base);
                const Symbol* b = base->kind == TypeKind::named ? base->as<NamedType>().symbol : nullptr;
                if (b && b->is_type() && b->kind != SymbolKind::type_param) {
                    t->bases.push_back(const_cast<Symbol*>(b));
                } else if (!b || b->kind == SymbolKind::type_param) {
                    t->has_opaque_base = true;   // std:: types, template parameters, or already reported
                }
            }
        }
    }

    struct Inherited {
        std::vector<Symbol*> methods;   // every method of every ancestor
        bool                 opaque = false;
    };

    static void collect_inherited(const Symbol* t, Inherited& out, std::set<const Symbol*>& seen) {
        out.opaque = out.opaque || t->has_opaque_base;
        for (Symbol* b : t->bases) {
            if (!seen.insert(b).second) continue;
            for (Symbol* mem : b->member_order) {
                if (mem->kind == SymbolKind::method) out.methods.push_back(mem);
            }
            collect_inherited(b, out, seen);
        }
    }

    static bool is_dynamic(const Symbol* method) {
        const FunctionDecl* f = method->function();
        return method->parent->kind == SymbolKind::interface || f->is_virtual || f->is_abstract || f->is_override;
    }

    static bool same_signature(const Symbol* a, const Symbol* b) {
        return a->name == b->name && params_sig(*a->function()) == params_sig(*b->function());
    }

    static bool is_ancestor(const Symbol* t, const Symbol* maybe_base) {
        for (const Symbol* b : t->bases) {
            if (b == maybe_base || is_ancestor(b, maybe_base)) return true;
        }
        return false;
    }

    void check_hierarchy(Module& m) {
        for (Symbol* t : m.item_order) {
            if (t->kind != SymbolKind::struct_ && t->kind != SymbolKind::class_ && t->kind != SymbolKind::interface) {
                continue;
            }
            check_bases(t);
            check_methods(t);
        }
    }

    void check_bases(Symbol* t) {
        const auto& decl        = t->decl->as<TypeDecl>();
        int         class_bases = 0;
        for (std::size_t i = 0; i < decl.bases.size(); ++i) {
            const TypeExpr& base_expr = *decl.bases[i];
            const Symbol*   b = base_expr.kind == TypeKind::named ? base_expr.as<NamedType>().symbol : nullptr;
            if (!b || b->kind == SymbolKind::type_param) continue;
            if (t->kind == SymbolKind::struct_ || t->kind == SymbolKind::interface) {
                if (b->kind != SymbolKind::interface) {
                    note(error(DiagCode::E3046, base_expr.span,
                               std::format("{} '{}' can only extend interfaces; '{}' is a {}", sema::to_string(t->kind),
                                           t->name, b->name, sema::to_string(b->kind)),
                               "not an interface"),
                         t->kind == SymbolKind::struct_ ? "structs cannot inherit; use a class" : "interfaces extend interfaces");
                }
                continue;
            }
            if (b->kind == SymbolKind::class_) {
                ++class_bases;
                if (class_bases > 1) {
                    error(DiagCode::E3041, base_expr.span,
                          std::format("class '{}' has more than one base class", t->name), "second base class");
                } else if (i != 0) {
                    error(DiagCode::E3041, base_expr.span,
                          std::format("base class '{}' must be listed first", b->name), "move this first");
                }
                if (!b->is_open()) {
                    Diagnostic& e = error(DiagCode::E3040, base_expr.span,
                                          std::format("class '{}' is not open and cannot be inherited from", b->name),
                                          "not open");
                    note(e, std::format("declare it as 'open class {}'", b->name), b->span);
                }
            } else if (b->kind != SymbolKind::interface) {
                error(DiagCode::E3046, base_expr.span,
                      std::format("cannot inherit from {} '{}'", sema::to_string(b->kind), b->name),
                      "not a class or interface");
            }
        }
    }

    void check_methods(Symbol* t) {
        Inherited               inh;
        std::set<const Symbol*> seen;
        collect_inherited(t, inh, seen);
        bool open_class = t->is_open();

        for (Symbol* mem : t->member_order) {
            if (mem->kind != SymbolKind::method) continue;
            const FunctionDecl& f = *mem->function();
            if (f.is_destructor && f.name != "~" + t->name) {
                error(DiagCode::E3049, mem->span, std::format("destructor '{}' in class '{}'", f.name, t->name),
                      std::format("write '~{}'", t->name));
            }
            if ((f.is_virtual || f.is_abstract) && !open_class && t->kind != SymbolKind::interface) {
                Diagnostic& e = error(DiagCode::E3042, mem->span,
                                      std::format("'{}' is {} but '{}' is not an open class", mem->name,
                                                  f.is_abstract ? "abstract" : "virtual", t->name),
                                      "needs an open class");
                note(e, std::format("declare 'open class {}', or remove the keyword", t->name));
            }
            if (f.is_abstract) {
                const Decl* def = mem->definition;
                if (!f.is_virtual) {
                    error(DiagCode::E3048, mem->span, std::format("abstract method '{}' must be declared 'virtual'", mem->name),
                          "write 'virtual ... abstract'");
                }
                if (def && def->as<FunctionDecl>().body) {
                    error(DiagCode::E3048, mem->span, std::format("abstract method '{}' has a body", mem->name),
                          "abstract methods have no implementation");
                }
            }
            if (f.is_destructor) continue;

            std::vector<Symbol*> same_name;
            for (Symbol* b : inh.methods) {
                if (b->name == mem->name) same_name.push_back(b);
            }
            if (f.is_override) {
                bool ok = std::any_of(same_name.begin(), same_name.end(),
                                      [&](Symbol* b) { return is_dynamic(b) && same_signature(b, mem); });
                if (!ok && !inh.opaque) {
                    Diagnostic& e = error(DiagCode::E3043, mem->span,
                                          std::format("'{}' is marked 'override' but overrides nothing", describe_fn(*mem)),
                                          "no matching base method");
                    for (Symbol* b : same_name) {
                        note(e, is_dynamic(b) ? std::format("'{}' has different parameters", describe_fn(*b))
                                              : std::format("'{}' is not virtual", describe_fn(*b)),
                             b->span);
                    }
                }
            } else if (!same_name.empty() && t->kind != SymbolKind::interface) {
                Diagnostic& e = error(DiagCode::E3044, mem->span,
                                      std::format("'{}' has the same name as '{}'", mem->name, describe_fn(*same_name.front())),
                                      "add 'override' or rename it");
                note(e, "inherited method declared here", same_name.front()->span);
            }
        }

        // Every interface method and every abstract base method needs an implementation in
        // classes that cannot be inherited from further.
        if (open_class || t->kind == SymbolKind::interface || inh.opaque) return;
        std::vector<Symbol*> chain{t};   // t and its base classes
        for (Symbol* b : inh.methods) {
            if (b->parent->kind == SymbolKind::class_ &&
                std::find(chain.begin(), chain.end(), b->parent) == chain.end()) {
                chain.push_back(b->parent);
            }
        }
        std::set<std::string> reported;
        for (Symbol* req : inh.methods) {
            bool required = req->parent->kind == SymbolKind::interface || req->function()->is_abstract;
            if (!required) continue;
            bool implemented = false;
            for (Symbol* c : chain) {
                for (Symbol* mem : c->member_order) {
                    if (mem->kind == SymbolKind::method && same_signature(mem, req) && !mem->function()->is_abstract) {
                        implemented = true;
                    }
                }
            }
            std::string key = describe_fn(*req);
            if (!implemented && reported.insert(req->name + params_sig(*req->function())).second) {
                Diagnostic& e = error(DiagCode::E3045, t->span,
                                      std::format("'{}' does not implement '{}'", t->name, key),
                                      "missing implementation");
                note(e, "declared here", req->span);
            }
        }
    }

    // --- phase D: bodies ------------------------------------------------------------------

    void resolve_bodies(Module& m) {
        if (m.header) {
            FileScope fs(*this, &m, true);
            for (const DeclPtr& d : m.header->ast.decls) resolve_decl(*d, nullptr);
        }
        if (m.source) {
            FileScope fs(*this, &m, false);
            for (const DeclPtr& d : m.source->ast.decls) resolve_decl(*d, nullptr);
        }
    }

    void resolve_decl(Decl& d, Symbol* self) {
        TemplateScope ts(*this, d);
        switch (d.kind) {
        case DeclKind::import: return;
        case DeclKind::function: resolve_function(d.as<FunctionDecl>(), self); return;
        case DeclKind::variable:
        case DeclKind::field: {
            auto& v = d.as<VariableDecl>();
            resolve_type(*v.type);
            if (v.init) resolve_expr(*v.init);
            return;
        }
        case DeclKind::struct_:
        case DeclKind::class_:
        case DeclKind::interface: {
            auto    it = symbol_of_.find(&d);
            Symbol* t  = it == symbol_of_.end() ? nullptr : it->second;
            for (const DeclPtr& mem : d.as<TypeDecl>().members) resolve_decl(*mem, t);
            return;
        }
        case DeclKind::enum_: {
            auto& e = d.as<EnumDecl>();
            resolve_type(*e.tag_type);
            for (EnumAlternative& alt : e.alternatives) {
                if (alt.payload) resolve_type(*alt.payload);
                if (alt.value) resolve_expr(*alt.value);
            }
            return;
        }
        }
    }

    void resolve_function(FunctionDecl& fn, Symbol* self) {
        FunctionContext saved = fn_;
        fn_                   = FunctionContext{};
        fn_.self_type         = self;
        fn_.is_static         = fn.is_static;
        fn_.has_this          = self && !fn.is_static;
        if (fn.result) resolve_type(*fn.result);
        for (Param& p : fn.params) resolve_type(*p.type);
        if (fn.body) {
            declared_later_.clear();
            collect_declarations(*fn.body);
            scopes_.emplace_back();
            for (Param& p : fn.params) {
                bool ref = p.type->kind == TypeKind::reference;
                p.symbol = declare_local(p.name, p.span, ref ? LocalKind::ref_param : LocalKind::value_param, p.is_mut, ref);
            }
            resolve_block(*fn.body);
            scopes_.pop_back();
        }
        fn_ = saved;
    }

    // --- locals ---------------------------------------------------------------------------

    Symbol* declare_local(const std::string& name, Span span, LocalKind kind, bool is_mut, bool is_reference,
                          bool check_shadowing = true) {
        if (check_shadowing) {
            for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
                if (auto found = it->names.find(name); found != it->names.end()) {
                    Diagnostic& e = error(DiagCode::E3028, span,
                                          std::format("'{}' is already declared in this function", name),
                                          "declared again");
                    note(e, "earlier declaration here", found->second->span);
                    note(e, "Rift has no shadowing; choose a different name");
                    break;
                }
            }
            if (Symbol* tp = find_template_param(name)) {
                note(error(DiagCode::E3028, span, std::format("'{}' is already a template parameter", name),
                           "declared again"),
                     "template parameter here", tp->span);
            }
        }
        Symbol* s       = module_->make(SymbolKind::local, name, span);
        s->local_kind   = kind;
        s->is_mut       = is_mut;
        s->is_reference = is_reference;
        scopes_.back().names[name] = s;
        return s;
    }

    Symbol* lookup_local(const std::string& name, Span use) {
        bool crossed_lambda = false;
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (auto found = it->names.find(name); found != it->names.end()) {
                if (crossed_lambda) {
                    Diagnostic& e = error(DiagCode::E3050, use,
                                          std::format("the lambda uses '{}' without capturing it", name),
                                          "not captured");
                    note(e, std::format("add it to the capture list: [{0}], [&{0}] or [move {0}]", name));
                }
                return found->second;
            }
            if (it->lambda_boundary) crossed_lambda = true;
        }
        return nullptr;
    }

    std::vector<std::string> value_candidates() const {
        std::vector<std::string> out;
        for (const Scope& s : scopes_) {
            for (const auto& [n, sym] : s.names) out.push_back(n);
        }
        for (const auto& [n, syms] : module_->items) out.push_back(n);
        return out;
    }

    std::vector<std::string> type_candidates() const {
        std::vector<std::string> out = {"bool", "char", "void", "never", "i8", "i16", "i32", "i64", "i128", "isize",
                                        "u8", "u16", "u32", "u64", "u128", "usize", "f32", "f64"};
        for (const auto& [n, syms] : module_->items) {
            if (!syms.empty() && syms.front()->is_type()) out.push_back(n);
        }
        for (const auto& level : template_params_) {
            for (Symbol* s : level) out.push_back(s->name);
        }
        return out;
    }

    static std::vector<std::string> member_names(const Symbol* t) {
        std::vector<std::string> out;
        for (const auto& [n, syms] : t->members) out.push_back(n);
        return out;
    }

    // --- paths ----------------------------------------------------------------------------

    void resolve_template_args(Path& path) {
        for (PathSegment& seg : path.segments) {
            for (TemplateArg& arg : seg.template_args) {
                if (arg.type) resolve_type(*arg.type);
                if (arg.value) resolve_expr(*arg.value);
            }
        }
    }

    /// Finds a member (field, method, alternative) in a type or its ancestors.
    static Symbol* find_member(const Symbol* t, const std::string& name, bool& opaque) {
        opaque = opaque || t->has_opaque_base;
        if (auto it = t->members.find(name); it != t->members.end() && !it->second.empty()) {
            return it->second.front();
        }
        for (const Symbol* b : t->bases) {
            if (Symbol* s = find_member(b, name, opaque)) return s;
        }
        return nullptr;
    }

    bool member_visible(const Symbol* mem) const {
        if (mem->access == Access::private_ && mem->parent->module != module_) return false;
        if (mem->access == Access::protected_ && mem->parent->module != module_) {
            return fn_.self_type && (fn_.self_type == mem->parent || is_ancestor(fn_.self_type, mem->parent));
        }
        return true;
    }

    Symbol* check_want(Symbol* sym, Want want, Span span, std::string_view display) {
        if (!sym) return nullptr;
        if (want == Want::type && !sym->is_type()) {
            error(DiagCode::E3022, span, std::format("'{}' is a {}, not a type", display, sema::to_string(sym->kind)),
                  "not a type");
            return nullptr;
        }
        if (want == Want::value && sym->is_type()) {
            Diagnostic& e = error(DiagCode::E3022, span,
                                  std::format("'{}' is a {}, not a value", display, sema::to_string(sym->kind)),
                                  "not a value");
            if (sym->kind == SymbolKind::struct_ || sym->kind == SymbolKind::class_) {
                note(e, std::format("construct it with '{}{{ ... }}'", display));
            }
            return nullptr;
        }
        return sym;
    }

    Symbol* lookup_single(PathSegment& seg, Want want, Span span) {
        const std::string& name = seg.name;
        if (want == Want::type) {
            if (Symbol* tp = find_template_param(name)) {
                return check_want(tp, want, span, name);
            }
            if (auto it = module_->items.find(name); it != module_->items.end()) {
                return check_want(it->second.front(), want, span, name);
            }
            if (visible_module(name)) {
                error(DiagCode::E3022, span, std::format("'{}' is a module, not a type", name), "module");
                return nullptr;
            }
            suggest(error(DiagCode::E3021, span, std::format("cannot find type '{}'", name), "unknown type"), name,
                    type_candidates());
            return nullptr;
        }
        if (Symbol* local = lookup_local(name, span)) {
            return local;
        }
        if (Symbol* tp = find_template_param(name)) {
            return check_want(tp, want, span, name);
        }
        if (auto it = module_->items.find(name); it != module_->items.end()) {
            return check_want(it->second.front(), want, span, name);
        }
        if (visible_module(name) || name == "std") {
            error(DiagCode::E3022, span, std::format("'{}' is a module, not a value", name), "module");
            return nullptr;
        }
        if (fn_.self_type) {
            bool opaque = false;
            if (Symbol* mem = find_member(fn_.self_type, name, opaque);
                mem && (mem->kind == SymbolKind::field || mem->kind == SymbolKind::method)) {
                Diagnostic& e = error(DiagCode::E3026, span,
                                      std::format("'{}' is a member of '{}'; write 'this.{}'", name,
                                                  fn_.self_type->name, name),
                                      std::format("use 'this.{}'", name));
                note(e, "members are always accessed through 'this.'");
                return nullptr;
            }
        }
        Diagnostic& e = error(DiagCode::E3020, span, std::format("cannot find '{}' in this scope", name), "not found");
        if (auto later = declared_later_.find(name); later != declared_later_.end() && later->second.begin > span.begin) {
            note(e, std::format("'{}' is declared later in this function; move the declaration up", name), later->second);
        } else {
            suggest(e, name, value_candidates());
        }
        return nullptr;
    }

    /// Records every local variable declared in a function body, for "declared later" notes.
    void collect_declarations(const BlockExpr& block) {
        std::function<void(const Expr&)> walk_expr;
        std::function<void(const BlockExpr&)> walk_block = [&](const BlockExpr& b) {
            for (const StmtPtr& st : b.statements) {
                if (st->kind == StmtKind::variable) {
                    const auto& v = st->as<VariableStmt>();
                    declared_later_.emplace(v.name, v.name_span);
                } else if (st->kind == StmtKind::expression) {
                    walk_expr(*st->as<ExprStmt>().expr);
                }
            }
            if (b.tail) walk_expr(*b.tail);
        };
        walk_expr = [&](const Expr& e) {
            switch (e.kind) {
            case ExprKind::block: walk_block(e.as<BlockExpr>()); break;
            case ExprKind::if_: {
                const auto& i = e.as<IfExpr>();
                walk_block(*i.then_block);
                if (i.else_branch) walk_expr(*i.else_branch);
                break;
            }
            case ExprKind::while_: walk_block(*e.as<WhileExpr>().body); break;
            case ExprKind::for_: walk_block(*e.as<ForExpr>().body); break;
            default: break;
            }
        };
        walk_block(block);
    }

    /// Walks `T::member::...` from a type symbol.
    Symbol* lookup_members(Symbol* base, Path& path, std::size_t i, Want want, Span span) {
        std::string display;
        for (std::size_t k = 0; k < i; ++k) display += (k ? "::" : "") + path.segments[k].name;
        while (i < path.segments.size()) {
            const std::string& name = path.segments[i].name;
            Span               seg  = path.segments[i].span;
            if (base->kind == SymbolKind::type_param) {
                return nullptr;   // depends on the instantiation
            }
            if (!base->is_type() || base->kind == SymbolKind::interface) {
                error(DiagCode::E3025, seg,
                      std::format("'{}' is a {} and has no members that can be named with '::'", display,
                                  sema::to_string(base->kind)),
                      "no such member");
                return nullptr;
            }
            bool    opaque = false;
            Symbol* mem    = find_member(base, name, opaque);
            if (!mem) {
                if (opaque) return nullptr;
                Diagnostic& e = error(DiagCode::E3025, seg,
                                      base->kind == SymbolKind::enum_
                                          ? std::format("enum '{}' has no alternative '{}'", display, name)
                                          : std::format("'{}' has no member '{}'", display, name),
                                      "not found");
                suggest(e, name, member_names(base));
                return nullptr;
            }
            if (mem->kind != SymbolKind::alternative && !member_visible(mem)) {
                std::string why = mem->access == Access::protected_
                                      ? std::format("'{}::{}' is protected", display, name)
                                      : std::format("'{}::{}' is private to module '{}'", display, name,
                                                    mem->parent->module->name);
                Diagnostic& e = error(DiagCode::E3024, seg, why, "not visible here");
                if (mem->access == Access::protected_) note(e, "protected members are visible to derived classes only");
                note(e, "declared here", mem->span);
                return nullptr;
            }
            if (mem->kind == SymbolKind::field || (mem->kind == SymbolKind::method && !mem->is_static)) {
                Diagnostic& e = error(DiagCode::E3038, seg,
                                      std::format("'{}::{}' is a {}{}", display, name,
                                                  mem->kind == SymbolKind::field ? "field" : "method",
                                                  mem->kind == SymbolKind::field ? "" : " but not static"),
                                      "not static");
                note(e, std::format("access it on a value: 'value.{}'", name));
                return nullptr;
            }
            display += "::" + name;
            base = mem;
            ++i;
        }
        return check_want(base, want, span, display);
    }

    Symbol* lookup_path(Path& path, Want want, Span span) {
        resolve_template_args(path);
        auto& segs = path.segments;
        if (segs.size() == 1) {
            if (want == Want::type && is_builtin_type_name(segs[0].name)) return nullptr;
            return lookup_single(segs[0], want, span);
        }
        const std::string& first = segs[0].name;
        if (is_builtin_type_name(first)) {
            return nullptr;   // `u32::max`: built-in members come with the type checker
        }
        if (first == "std") {
            if (!std_visible() && std_reported_.insert(segs[0].span.file).second) {
                note(error(DiagCode::E3023, segs[0].span, "the standard library is not imported", "'std' not imported"),
                     "add 'import std;' at the top of the file (reported once per file)");
            }
            return nullptr;
        }
        if (Symbol* tp = find_template_param(first)) {
            (void)tp;
            return nullptr;
        }
        if (auto it = module_->items.find(first); it != module_->items.end()) {
            return lookup_members(it->second.front(), path, 1, want, span);
        }
        // Longest prefix that names an imported module: `shapes::circle::circle`.
        for (std::size_t k = segs.size() - 1; k >= 1; --k) {
            std::string name;
            for (std::size_t j = 0; j < k; ++j) name += (j ? "::" : "") + segs[j].name;
            Module* mod = visible_module(name);
            if (!mod && program_.modules.count(name)) {
                note(error(DiagCode::E3023, segs[0].span, std::format("module '{}' is not imported here", name),
                           "not imported"),
                     std::format("add 'import {};'", name));
                return nullptr;
            }
            if (!mod) continue;
            const std::string& item_name = segs[k].name;
            auto               it        = mod->items.find(item_name);
            Symbol*            item      = nullptr;
            bool               hidden    = false;
            if (it != mod->items.end()) {
                for (Symbol* s : it->second) {
                    if (s->is_private) hidden = true;
                    else if (!item) item = s;
                }
            }
            if (!item) {
                if (hidden) {
                    note(error(DiagCode::E3024, segs[k].span,
                               std::format("'{}' is private to module '{}'", item_name, name), "private"),
                         "only items declared in the module's .rifth can be used from other modules");
                } else {
                    std::vector<std::string> names;
                    for (const auto& [n, syms] : mod->items) {
                        if (!syms.front()->is_private) names.push_back(n);
                    }
                    suggest(error(DiagCode::E3020, segs[k].span,
                                  std::format("cannot find '{}' in module '{}'", item_name, name), "not found"),
                            item_name, names);
                }
                return nullptr;
            }
            return lookup_members(item, path, k + 1, want, span);
        }
        suggest(error(DiagCode::E3020, segs[0].span, std::format("cannot find '{}' in this scope", first),
                      "not a module, type or enum"),
                first, value_candidates());
        return nullptr;
    }

    // --- types ----------------------------------------------------------------------------

    void resolve_type(TypeExpr& t) {
        switch (t.kind) {
        case TypeKind::named: {
            auto& nt  = t.as<NamedType>();
            nt.symbol = lookup_path(nt.path, Want::type, nt.span);
            return;
        }
        case TypeKind::reference: resolve_type(*t.as<ReferenceType>().inner); return;
        case TypeKind::function: {
            auto& f = t.as<FunctionType>();
            resolve_type(*f.result);
            for (TypePtr& p : f.params) resolve_type(*p);
            return;
        }
        case TypeKind::auto_:
        case TypeKind::deduce: return;
        }
    }

    // --- statements and expressions -------------------------------------------------------

    void resolve_block(BlockExpr& b) {
        scopes_.emplace_back();
        for (StmtPtr& s : b.statements) resolve_stmt(*s);
        if (b.tail) resolve_expr(*b.tail);
        scopes_.pop_back();
    }

    void resolve_stmt(Stmt& s) {
        switch (s.kind) {
        case StmtKind::variable: {
            auto& v = s.as<VariableStmt>();
            resolve_type(*v.type);
            if (v.init) resolve_expr(*v.init);
            v.symbol = declare_local(v.name, v.name_span, LocalKind::variable, v.is_mut,
                                     v.type->kind == TypeKind::reference);
            return;
        }
        case StmtKind::assign: {
            auto& a = s.as<AssignStmt>();
            resolve_expr(*a.target);
            resolve_expr(*a.value);
            return;
        }
        case StmtKind::discard: resolve_expr(*s.as<DiscardStmt>().value); return;
        case StmtKind::expression: resolve_expr(*s.as<ExprStmt>().expr); return;
        }
    }

    void resolve_expr(Expr& e, ExprFlags flags = {}) {
        switch (e.kind) {
        case ExprKind::int_literal:
        case ExprKind::float_literal:
        case ExprKind::char_literal:
        case ExprKind::string_literal:
        case ExprKind::bool_literal:
        case ExprKind::nullptr_literal:
        case ExprKind::default_:
        case ExprKind::error: return;
        case ExprKind::this_:
            if (!fn_.has_this) {
                error(DiagCode::E3027, e.span,
                      fn_.self_type && fn_.is_static ? "static methods have no 'this'"
                                                     : "'this' is only available inside methods",
                      "no 'this' here");
            }
            return;
        case ExprKind::break_:
        case ExprKind::continue_:
            if (fn_.loop_depth == 0) {
                error(DiagCode::E3030, e.span,
                      std::format("'{}' outside of a loop", e.kind == ExprKind::break_ ? "break" : "continue"),
                      "not inside 'while' or 'for'");
            }
            return;
        case ExprKind::path: resolve_path_expr(e.as<PathExpr>(), flags.is_callee); return;
        case ExprKind::unary: {
            auto& u = e.as<UnaryExpr>();
            if (u.op == UnaryOp::mut_marker && !flags.allow_mut_marker) {
                note(error(DiagCode::E3031, e.span, "'mut' is not allowed here", "misplaced 'mut'"),
                     "'mut x' marks a call argument passed as a mutable reference, or a match subject; "
                     "'mut &x' takes a mutable pointer");
            }
            resolve_expr(*u.operand);
            if (u.op == UnaryOp::move) check_move(*u.operand);
            return;
        }
        case ExprKind::binary: {
            auto& b = e.as<BinaryExpr>();
            resolve_expr(*b.lhs);
            resolve_expr(*b.rhs);
            return;
        }
        case ExprKind::range: {
            auto& r = e.as<RangeExpr>();
            resolve_expr(*r.lo);
            resolve_expr(*r.hi);
            return;
        }
        case ExprKind::call: {
            auto& c = e.as<CallExpr>();
            resolve_expr(*c.callee, ExprFlags{false, true});
            for (ExprPtr& arg : c.args) resolve_expr(*arg, ExprFlags{true, false});
            return;
        }
        case ExprKind::index: {
            auto& i = e.as<IndexExpr>();
            resolve_expr(*i.object);
            resolve_expr(*i.index);
            return;
        }
        case ExprKind::member: resolve_member(e.as<MemberExpr>()); return;
        case ExprKind::try_: resolve_expr(*e.as<TryExpr>().operand); return;
        case ExprKind::cast: {
            auto& c = e.as<CastExpr>();
            resolve_type(*c.target);
            resolve_expr(*c.operand);
            return;
        }
        case ExprKind::bit: {
            auto& b = e.as<BitExpr>();
            if (b.type) resolve_type(*b.type);
            resolve_expr(*b.operand);
            return;
        }
        case ExprKind::struct_literal: resolve_struct_literal(e.as<StructLiteralExpr>()); return;
        case ExprKind::lambda: resolve_lambda(e.as<LambdaExpr>()); return;
        case ExprKind::if_: {
            auto& i = e.as<IfExpr>();
            resolve_expr(*i.condition);
            resolve_block(*i.then_block);
            if (i.else_branch) resolve_expr(*i.else_branch);
            return;
        }
        case ExprKind::match: {
            auto& m = e.as<MatchExpr>();
            resolve_expr(*m.scrutinee, ExprFlags{true, false});
            for (MatchArm& arm : m.arms) {
                scopes_.emplace_back();
                resolve_pattern(*arm.pattern);
                if (arm.guard) resolve_expr(*arm.guard);
                resolve_expr(*arm.body);
                scopes_.pop_back();
            }
            return;
        }
        case ExprKind::block: resolve_block(e.as<BlockExpr>()); return;
        case ExprKind::while_: {
            auto& w = e.as<WhileExpr>();
            resolve_expr(*w.condition);
            ++fn_.loop_depth;
            resolve_block(*w.body);
            --fn_.loop_depth;
            return;
        }
        case ExprKind::for_: {
            auto& f = e.as<ForExpr>();
            resolve_expr(*f.iterable);
            scopes_.emplace_back();
            resolve_type(*f.type);
            f.symbol = declare_local(f.name, f.name_span, LocalKind::loop_variable, f.is_mut,
                                     f.type->kind == TypeKind::reference);
            ++fn_.loop_depth;
            resolve_block(*f.body);
            --fn_.loop_depth;
            scopes_.pop_back();
            return;
        }
        case ExprKind::return_: {
            auto& r = e.as<ReturnExpr>();
            if (r.value) resolve_expr(*r.value);
            return;
        }
        }
    }

    void resolve_path_expr(PathExpr& pe, bool is_callee) {
        Symbol* sym = lookup_path(pe.path, Want::value, pe.span);
        pe.symbol   = sym;
        if (sym && sym->kind == SymbolKind::alternative) {
            bool        payload = sym->alternative->payload != nullptr;
            std::string display = to_string(pe.path);
            if (payload && !is_callee) {
                note(error(DiagCode::E3037, pe.span, std::format("'{}' carries a value", display), "missing payload"),
                     std::format("construct it with '{}(...)'", display));
            } else if (!payload && is_callee) {
                note(error(DiagCode::E3037, pe.span, std::format("'{}' has no payload", display), "not callable"),
                     std::format("write '{}' without '(...)'", display));
            }
        }
    }

    void check_move(const Expr& operand) {
        if (operand.kind == ExprKind::path) {
            const auto& pe = operand.as<PathExpr>();
            if (!pe.symbol) return;   // already reported, or unchecked (std::)
            if (pe.symbol->kind == SymbolKind::local) {
                if (pe.symbol->is_reference) {
                    note(error(DiagCode::E3032, operand.span,
                               std::format("'{}' is a reference and cannot be moved", pe.symbol->name), "reference"),
                         "references never own their value; pass the owner by value to move it");
                }
                return;
            }
            error(DiagCode::E3032, operand.span,
                  std::format("cannot move {} '{}'", sema::to_string(pe.symbol->kind), pe.symbol->name),
                  "not a local variable");
            return;
        }
        Diagnostic& e = error(DiagCode::E3032, operand.span, "only local variables and by-value parameters can be moved",
                              "not a local variable");
        if (operand.kind == ExprKind::member || operand.kind == ExprKind::index) {
            note(e, "to take a field's value use 'std::take(mut obj.field)' or 'std::replace(mut obj.field, value)'");
        }
    }

    void resolve_member(MemberExpr& me) {
        resolve_expr(*me.object);
        resolve_template_args(me.name);
        if (me.object->kind != ExprKind::this_ || !fn_.self_type || !fn_.has_this || me.arrow) {
            return;   // members of other values are resolved by the type checker
        }
        Symbol* self = fn_.self_type;
        auto&   segs = me.name.segments;
        bool    opaque = false;
        if (segs.size() == 1) {
            Symbol* mem = find_member(self, segs[0].name, opaque);
            if (!mem && !opaque) {
                Diagnostic& e = error(DiagCode::E3025, segs[0].span,
                                      std::format("'{}' has no member '{}'", self->name, segs[0].name), "not found");
                suggest(e, segs[0].name, member_names(self));
            }
            me.symbol = mem;
            return;
        }
        // this.base::method(): `base` must be an ancestor class.
        const std::string& base_name = segs[0].name;
        Symbol*            base      = nullptr;
        std::function<void(Symbol*)> search = [&](Symbol* t) {
            for (Symbol* b : t->bases) {
                if (!base && b->name == base_name) base = b;
                search(b);
            }
        };
        search(self);
        if (!base) {
            if (!self->has_opaque_base) {
                error(DiagCode::E3047, segs[0].span,
                      std::format("'{}' is not a base class of '{}'", base_name, self->name), "not a base");
            }
            return;
        }
        Symbol* mem = find_member(base, segs[1].name, opaque);
        if (!mem && !opaque) {
            error(DiagCode::E3025, segs[1].span, std::format("'{}' has no member '{}'", base->name, segs[1].name),
                  "not found");
        }
        me.symbol = mem;
    }

    /// Fields of a type and its base classes, base fields first.
    static void collect_fields(const Symbol* t, std::vector<Symbol*>& out, bool& opaque) {
        opaque = opaque || t->has_opaque_base;
        for (const Symbol* b : t->bases) {
            if (b->kind == SymbolKind::class_) collect_fields(b, out, opaque);
        }
        for (Symbol* m : t->member_order) {
            if (m->kind == SymbolKind::field && !m->is_static) out.push_back(m);
        }
    }

    void resolve_struct_literal(StructLiteralExpr& sl) {
        resolve_type(*sl.type);
        for (FieldInit& f : sl.fields) resolve_expr(*f.value);
        const Symbol* t = sl.type->kind == TypeKind::named ? sl.type->as<NamedType>().symbol : nullptr;
        if (!t || t->kind == SymbolKind::type_param) return;
        std::string display = to_string(*sl.type);
        if (t->kind == SymbolKind::enum_ || t->kind == SymbolKind::interface) {
            Diagnostic& e = error(DiagCode::E3022, sl.type->span,
                                  std::format("{} '{}' cannot be constructed with '{{...}}'", sema::to_string(t->kind), display),
                                  "not a struct or class");
            if (t->kind == SymbolKind::enum_) note(e, std::format("write '{}::alternative'", display));
            return;
        }
        std::vector<Symbol*> fields;
        bool                 opaque = false;
        collect_fields(t, fields, opaque);
        std::map<std::string, const FieldInit*> given;
        for (const FieldInit& f : sl.fields) {
            auto field = std::find_if(fields.begin(), fields.end(), [&](Symbol* s) { return s->name == f.name; });
            if (field == fields.end()) {
                if (opaque) continue;
                std::vector<std::string> names;
                for (Symbol* s : fields) names.push_back(s->name);
                suggest(error(DiagCode::E3033, f.span, std::format("'{}' has no field '{}'", display, f.name), "unknown field"),
                        f.name, names);
                continue;
            }
            if (auto prev = given.find(f.name); prev != given.end()) {
                note(error(DiagCode::E3034, f.span, std::format("field '{}' is given twice", f.name), "given again"),
                     "first given here", prev->second->span);
                continue;
            }
            given[f.name] = &f;
            if ((*field)->access == Access::private_ && (*field)->parent->module != module_) {
                note(error(DiagCode::E3036, f.span,
                           std::format("field '{}' is private to module '{}'", f.name, (*field)->parent->module->name),
                           "cannot be set here"),
                     "outside its module a class's private fields always take their defaults");
            }
        }
        if (opaque) return;
        std::vector<std::string> missing;
        bool                     missing_private = false;
        for (Symbol* f : fields) {
            if (given.count(f->name) || f->decl->as<VariableDecl>().init) continue;
            missing.push_back("'" + f->name + "'");
            missing_private = missing_private || (f->access == Access::private_ && f->parent->module != module_);
        }
        if (!missing.empty()) {
            std::string list;
            for (std::size_t i = 0; i < missing.size(); ++i) list += (i ? ", " : "") + missing[i];
            Diagnostic& e = error(DiagCode::E3035, sl.type->span,
                                  std::format("missing {} {} in '{}{{...}}'", missing.size() == 1 ? "field" : "fields",
                                              list, display),
                                  "incomplete");
            note(e, "fields without a default value must be given");
            if (missing_private) {
                note(e, "private fields cannot be set outside their module; give them a default or provide a "
                        "static factory method");
            }
        }
    }

    void resolve_lambda(LambdaExpr& l) {
        for (Capture& c : l.captures) {
            if (c.init) resolve_expr(*c.init);
        }
        std::vector<Symbol*> outer(l.captures.size(), nullptr);
        for (std::size_t i = 0; i < l.captures.size(); ++i) {
            Capture& c = l.captures[i];
            if (c.kind == CaptureKind::init) continue;
            outer[i] = lookup_local(c.name, c.span);
            if (!outer[i]) {
                Diagnostic& e = error(DiagCode::E3020, c.span, std::format("cannot capture '{}': no such local", c.name),
                                      "not a local variable");
                if (module_->items.count(c.name)) note(e, "module-level items can be used without capturing them");
                else suggest(e, c.name, value_candidates());
            } else if (c.kind == CaptureKind::move && outer[i]->is_reference) {
                error(DiagCode::E3032, c.span, std::format("'{}' is a reference and cannot be moved", c.name),
                      "reference");
            }
        }
        Scope boundary;
        boundary.lambda_boundary = true;
        scopes_.push_back(boundary);
        for (std::size_t i = 0; i < l.captures.size(); ++i) {
            Capture& c   = l.captures[i];
            bool     ref = c.kind == CaptureKind::ref || c.kind == CaptureKind::mut_ref;
            c.symbol     = declare_local(c.name, c.span, LocalKind::capture, c.kind == CaptureKind::mut_ref, ref,
                                         /*check_shadowing=*/c.kind == CaptureKind::init);
        }
        for (Param& p : l.params) {
            resolve_type(*p.type);
            bool ref = p.type->kind == TypeKind::reference;
            p.symbol = declare_local(p.name, p.span, ref ? LocalKind::ref_param : LocalKind::value_param, p.is_mut, ref);
        }
        if (l.result) resolve_type(*l.result);
        int saved_loops  = fn_.loop_depth;
        fn_.loop_depth   = 0;
        resolve_block(*l.body);
        fn_.loop_depth = saved_loops;
        scopes_.pop_back();
    }

    void resolve_pattern(Pattern& p) {
        switch (p.kind) {
        case PatternKind::wildcard:
        case PatternKind::literal:
        case PatternKind::range:
        case PatternKind::error: return;
        case PatternKind::binding: {
            auto& b  = p.as<BindingPattern>();
            b.symbol = declare_local(b.name, b.span, LocalKind::binding, false, false);
            return;
        }
        case PatternKind::enum_alternative: {
            auto&   ep  = p.as<EnumPattern>();
            Symbol* sym = lookup_path(ep.path, Want::value, ep.path.span);
            ep.symbol   = sym;
            std::string display = to_string(ep.path);
            if (sym && sym->kind != SymbolKind::alternative) {
                error(DiagCode::E3022, ep.path.span,
                      std::format("'{}' is a {}, not an enum alternative", display, sema::to_string(sym->kind)),
                      "not an alternative");
            } else if (sym) {
                bool payload = sym->alternative->payload != nullptr;
                if (payload && !ep.payload) {
                    note(error(DiagCode::E3037, ep.path.span, std::format("'{}' carries a value", display), "missing payload"),
                         std::format("match it with '{}(x)' or '{}(_)'", display, display));
                } else if (!payload && ep.payload) {
                    note(error(DiagCode::E3037, ep.payload->span, std::format("'{}' has no payload", display),
                               "nothing to match"),
                         std::format("write '{}' without '(...)'", display));
                }
            }
            if (ep.payload) resolve_pattern(*ep.payload);
            return;
        }
        case PatternKind::struct_: {
            auto&   sp  = p.as<StructPattern>();
            Symbol* sym = lookup_path(sp.path, Want::type, sp.path.span);
            for (FieldPattern& f : sp.fields) {
                if (sym && (sym->kind == SymbolKind::struct_ || sym->kind == SymbolKind::class_)) {
                    std::vector<Symbol*> fields;
                    bool                 opaque = false;
                    collect_fields(sym, fields, opaque);
                    bool found = std::any_of(fields.begin(), fields.end(), [&](Symbol* s) { return s->name == f.name; });
                    if (!found && !opaque) {
                        error(DiagCode::E3033, f.span, std::format("'{}' has no field '{}'", sym->name, f.name),
                              "unknown field");
                    }
                }
                resolve_pattern(*f.pattern);
            }
            return;
        }
        }
    }

    Program&                              program_;
    DiagnosticEngine&                     diags_;
    Module*                               module_    = nullptr;
    bool                                  in_header_ = false;
    std::map<const Decl*, Symbol*>        symbol_of_;
    std::vector<std::vector<Symbol*>>     template_params_;
    std::vector<Scope>                    scopes_;
    FunctionContext                       fn_;
    std::set<FileId>                      std_reported_;
    std::set<const Symbol*>               mismatched_;     // header functions with a mismatching definition
    std::multimap<std::string, Span>      declared_later_;   // locals of the current function   // "std not imported" once per file
};

// --- riftc symbols ------------------------------------------------------------------------

std::string fn_line(const Symbol& s) {
    const FunctionDecl& f = *s.function();
    std::string out;
    out += template_sig(f).empty() ? "" : template_sig(f) + " ";
    out += f.is_static && s.parent ? "static " : "";
    out += f.is_virtual ? "virtual " : "";
    out += f.is_destructor ? "destructor " : s.parent ? "method " : "fn ";
    out += s.name + params_sig(f);
    if (f.result) out += " -> " + to_string(*f.result);
    out += f.is_override ? " override" : "";
    out += f.is_abstract ? " abstract" : "";
    return out;
}

std::string origin(const Symbol& s) {
    if (s.in_header) return s.definition ? "  [header, defined in source]" : "  [header]";
    return "";
}

std::string access_prefix(const Symbol& s) {
    if (!s.parent || s.parent->kind != SymbolKind::class_) return "";
    switch (s.access) {
    case Access::public_: return "public ";
    case Access::protected_: return "protected ";
    case Access::private_: return "private ";
    case Access::none: return "";
    }
    return "";
}

} // namespace

void resolve_program(Program& program, DiagnosticEngine& diags) { Resolver(program, diags).run(); }

void print_symbols(std::ostream& out, const Module& m, const SourceManager& sources) {
    out << "module " << m.name << '\n';
    if (m.header) out << "  header: " << sources.file(m.header->id).path() << '\n';
    if (m.source) out << "  source: " << sources.file(m.source->id).path() << '\n';
    std::vector<std::string> imports;
    if (m.header_imports_std || m.source_imports_std) imports.push_back("std");
    for (const Module* i : m.header_imports) imports.push_back(i->name);
    for (const Module* i : m.source_imports) imports.push_back(i->name + " (source)");
    if (!imports.empty()) {
        out << "  imports:";
        for (const std::string& i : imports) out << ' ' << i;
        out << '\n';
    }
    for (const Symbol* s : m.item_order) {
        std::string priv = s->is_private ? "private " : "";
        switch (s->kind) {
        case SymbolKind::function: out << "  " << priv << fn_line(*s) << origin(*s) << '\n'; break;
        case SymbolKind::global: {
            const auto& v = s->decl->as<VariableDecl>();
            out << "  " << priv << (v.storage == Storage::compile_time ? "compile_time " : "static ")
                << (v.is_mut ? "mut " : "") << s->name << " : " << to_string(*v.type) << '\n';
            break;
        }
        case SymbolKind::enum_: {
            out << "  " << priv << "enum " << s->name << " : " << to_string(*s->decl->as<EnumDecl>().tag_type) << '\n';
            for (const Symbol* a : s->member_order) {
                out << "    " << a->name;
                if (a->alternative->payload) out << '(' << to_string(*a->alternative->payload) << ')';
                out << '\n';
            }
            break;
        }
        case SymbolKind::struct_:
        case SymbolKind::class_:
        case SymbolKind::interface: {
            const auto& t = s->decl->as<TypeDecl>();
            out << "  " << priv << (t.is_open ? "open " : "") << sema::to_string(s->kind) << ' ' << s->name;
            if (!t.bases.empty()) out << " : " << bases_sig(t);
            out << '\n';
            std::vector<std::string> layout;
            for (const Symbol* mem : s->member_order) {
                if (mem->kind == SymbolKind::method) {
                    out << "    " << access_prefix(*mem) << fn_line(*mem) << origin(*mem) << '\n';
                } else {
                    const auto& v = mem->decl->as<VariableDecl>();
                    out << "    " << access_prefix(*mem) << (mem->is_static ? "static " : "") << "field " << mem->name
                        << " : " << to_string(*v.type) << (v.init ? " = ..." : "") << '\n';
                    if (!mem->is_static) layout.push_back(mem->name);
                }
            }
            if (s->kind != SymbolKind::interface) {
                out << "    layout:";
                for (const std::string& l : layout) out << ' ' << l;
                out << '\n';
            }
            break;
        }
        default: break;
        }
    }
}

} // namespace rift

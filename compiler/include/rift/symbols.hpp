#pragma once
// Semantic model produced by module loading and name resolution (M3).
//
//   Program   every module reached from the entry file through `import`
//   Module    one `.rifth` header and/or `.rifts` source with its own namespace (spec 09)
//   Symbol    anything a name can refer to: module items, members, enum alternatives,
//             template parameters, locals
//
// Symbols are owned by their Module (stable addresses in a deque) and referenced from the AST.

#include "rift/ast.hpp"

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rift::types {
class TypeContext;
struct Type;
}

namespace rift::sema {

struct Module;

enum class SymbolKind {
    function,      // free function (all overloads are separate symbols)
    global,        // file-scope static / compile_time variable
    struct_,
    class_,
    interface,
    enum_,
    alternative,   // enum alternative
    field,
    method,        // includes destructors and operators
    type_param,    // `template<type T>`
    value_param,   // `template<usize N>`
    local,         // variable, parameter, loop variable, match binding, lambda capture
};

std::string_view to_string(SymbolKind kind);

enum class LocalKind { variable, value_param, ref_param, binding, loop_variable, capture };

struct Symbol {
    SymbolKind  kind = SymbolKind::local;
    std::string name;
    Span        span;                 // where the name is declared
    Module*     module  = nullptr;    // owning module (null for locals)
    Symbol*     parent  = nullptr;    // enclosing type for members / alternatives

    const ast::Decl* decl       = nullptr;   // declaration (the header one if there is a header)
    const ast::Decl* definition = nullptr;   // the `.rifts` definition with the body, if separate

    bool        in_header  = false;   // declared in the module's .rifth
    bool        is_private = false;   // module-local (`private`, or only in a .rifts with a header)
    ast::Access access     = ast::Access::none;   // class member section (public/protected/private)
    bool        is_static  = false;
    bool        is_mut     = false;   // locals and globals: declared `mut`

    // Types: members by name (methods may be overloaded), bases in declaration order.
    std::map<std::string, std::vector<Symbol*>> members;
    std::vector<Symbol*>                        member_order;   // fields + methods in layout order
    std::vector<Symbol*>                        bases;          // resolved base class / interfaces
    bool                                        has_opaque_base = false;   // e.g. std::formattable

    // Instances of a template type (spec 14): the template, the copy's index among the template's
    // instances (for names), and the type each template parameter stands for.
    const Symbol*                               template_of    = nullptr;
    unsigned                                    instance_index = 0;
    std::map<const Symbol*, const types::Type*> template_subst;

    // Alternatives.
    const ast::EnumAlternative* alternative = nullptr;

    // Locals.
    LocalKind local_kind   = LocalKind::variable;
    bool      is_reference = false;   // declared with a reference type (`T& x`): cannot be moved
    const types::Type* type = nullptr;  // locals: set by the type checker

    [[nodiscard]] bool is_type() const {
        return kind == SymbolKind::struct_ || kind == SymbolKind::class_ ||
               kind == SymbolKind::interface || kind == SymbolKind::enum_ ||
               kind == SymbolKind::type_param;
    }
    [[nodiscard]] const ast::FunctionDecl* function() const {
        return decl && decl->kind == ast::DeclKind::function ? &decl->as<ast::FunctionDecl>() : nullptr;
    }
    [[nodiscard]] bool is_open() const {
        return decl && (decl->kind == ast::DeclKind::class_) && decl->as<ast::TypeDecl>().is_open;
    }
};

/// The parameter substitution of the template instance that declares `s` (a member, payload or
/// alternative belongs to its enclosing type), or null outside instances.
inline const std::map<const Symbol*, const types::Type*>* substitution_of(const Symbol* s) {
    for (; s; s = s->parent) {
        if (!s->template_subst.empty()) return &s->template_subst;
    }
    return nullptr;
}

/// One parsed file of a module.
struct ModuleFile {
    FileId    id;
    ast::File ast;
};

struct Module {
    std::string               name;   // "counter", "shapes::circle"; the entry file's own name too
    std::optional<ModuleFile> header;
    std::optional<ModuleFile> source;

    std::vector<Module*> header_imports;   // imports visible to the header
    std::vector<Module*> source_imports;   // imports written in the source (header imports apply too)
    bool                 header_imports_std = false;
    bool                 source_imports_std = false;

    std::map<std::string, std::vector<Symbol*>> items;   // module scope (functions may overload)
    std::vector<Symbol*>                        item_order;

    std::deque<Symbol> storage;

    Symbol* make(SymbolKind kind, std::string symbol_name, Span span) {
        Symbol& s = storage.emplace_back();
        s.kind    = kind;
        s.name    = std::move(symbol_name);
        s.span    = span;
        s.module  = this;
        return &s;
    }
};

struct Program {
    std::map<std::string, std::unique_ptr<Module>> modules;   // by module name
    Module*                                        entry = nullptr;
    std::shared_ptr<types::TypeContext>            types;     // owns every Type the checker creates
    /// Instances of template types, in creation order. They are not in any module's items.
    std::vector<Symbol*>                           instances;
    /// The AST copy of each instance (same order), which the later phases walk.
    std::vector<ast::Decl*>                        instance_copies;
    /// The re-parsed copies of templates that instances are made of (see check.cpp).
    std::deque<ast::File>                          instance_files;
};

} // namespace rift::sema

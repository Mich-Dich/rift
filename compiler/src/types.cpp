#include "rift/types.hpp"

#include "rift/ast_printer.hpp"

namespace rift::types {

bool Type::is_user(sema::SymbolKind k) const { return kind == Kind::user && symbol && symbol->kind == k; }

std::string_view Type::external_base() const {
    if (kind != Kind::external) return {};
    std::string_view n = name;
    return n.substr(0, n.find('<'));
}

TypeContext::TypeContext() {
    error_         = make(Type{Kind::error, 0, false, false, nullptr, nullptr, false, "{unknown}"});
    untyped_int_   = make(Type{Kind::untyped_int, 0, false, false, nullptr, nullptr, false, "{integer}"});
    untyped_float_ = make(Type{Kind::untyped_float, 0, false, false, nullptr, nullptr, false, "{float}"});
    bool_t_        = make(Type{Kind::bool_, 0, false, false, nullptr, nullptr, false, "bool"});
    char_t_        = make(Type{Kind::char_, 0, false, false, nullptr, nullptr, false, "char"});
    void_t_        = make(Type{Kind::void_, 0, false, false, nullptr, nullptr, false, "void"});
    never_         = make(Type{Kind::never, 0, false, false, nullptr, nullptr, false, "never"});
    nullptr_t_     = make(Type{Kind::nullptr_, 0, false, false, nullptr, nullptr, false, "nullptr"});
    static_str_    = make(Type{Kind::static_str, 0, false, false, nullptr, nullptr, false, "std::static_str"});
    untyped_default_ = make(Type{Kind::untyped_default, 0, false, false, nullptr, nullptr, false, "{default}"});
    builtins_["bool"]  = bool_t_;
    builtins_["char"]  = char_t_;
    builtins_["void"]  = void_t_;
    builtins_["never"] = never_;
    struct IntSpec {
        const char* name;
        int         bits;
        bool        is_signed;
        bool        is_size;
    };
    // isize/usize are taken as 64-bit for range checks until targets exist.
    for (IntSpec s : {IntSpec{"i8", 8, true, false}, IntSpec{"i16", 16, true, false}, IntSpec{"i32", 32, true, false},
                      IntSpec{"i64", 64, true, false}, IntSpec{"i128", 128, true, false}, IntSpec{"isize", 64, true, true},
                      IntSpec{"u8", 8, false, false}, IntSpec{"u16", 16, false, false}, IntSpec{"u32", 32, false, false},
                      IntSpec{"u64", 64, false, false}, IntSpec{"u128", 128, false, false},
                      IntSpec{"usize", 64, false, true}}) {
        builtins_[s.name] = make(Type{Kind::int_, s.bits, s.is_signed, s.is_size, nullptr, nullptr, false, s.name});
    }
    builtins_["f32"] = make(Type{Kind::float_, 32, true, false, nullptr, nullptr, false, "f32"});
    builtins_["f64"] = make(Type{Kind::float_, 64, true, false, nullptr, nullptr, false, "f64"});
}

const Type* TypeContext::make(Type t) {
    storage_.push_back(std::move(t));
    return &storage_.back();
}

const Type* TypeContext::builtin(std::string_view name) const {
    auto it = builtins_.find(std::string(name));
    return it == builtins_.end() ? nullptr : it->second;
}

const Type* TypeContext::reference(const Type* inner, bool is_mut) {
    if (inner->kind == Kind::reference) inner = inner->inner;   // no references to references
    auto key = std::make_tuple(static_cast<int>(Kind::reference), inner, is_mut);
    if (auto it = composites_.find(key); it != composites_.end()) return it->second;
    return composites_[key] = make(Type{Kind::reference, 0, false, false, nullptr, inner, is_mut, ""});
}

const Type* TypeContext::pointer(const Type* inner, bool is_mut) {
    auto key = std::make_tuple(static_cast<int>(Kind::pointer), inner, is_mut);
    if (auto it = composites_.find(key); it != composites_.end()) return it->second;
    return composites_[key] = make(Type{Kind::pointer, 0, false, false, nullptr, inner, is_mut, ""});
}

const Type* TypeContext::range(const Type* element) {
    auto key = std::make_tuple(static_cast<int>(Kind::range), element, false);
    if (auto it = composites_.find(key); it != composites_.end()) return it->second;
    return composites_[key] = make(Type{Kind::range, 0, false, false, nullptr, element, false, ""});
}

const Type* TypeContext::user(const sema::Symbol* symbol) {
    if (auto it = users_.find(symbol); it != users_.end()) return it->second;
    return users_[symbol] = make(Type{Kind::user, 0, false, false, symbol, nullptr, false, symbol->name});
}

const Type* TypeContext::type_param(const sema::Symbol* symbol) {
    if (auto it = params_.find(symbol); it != params_.end()) return it->second;
    return params_[symbol] = make(Type{Kind::type_param, 0, false, false, symbol, nullptr, false, symbol->name});
}

const Type* TypeContext::external(const std::string& spelling) {
    if (spelling == "std::static_str") return static_str_;
    if (auto it = externals_.find(spelling); it != externals_.end()) return it->second;
    return externals_[spelling] = make(Type{Kind::external, 0, false, false, nullptr, nullptr, false, spelling});
}

const Type* TypeContext::lower(const ast::TypeExpr& t, const std::map<const sema::Symbol*, const Type*>* subst) {
    using namespace ast;
    switch (t.kind) {
    case TypeKind::auto_:
    case TypeKind::deduce:
    case TypeKind::function: return error_;
    case TypeKind::reference: {
        const auto& r = t.as<ReferenceType>();
        return reference(lower(*r.inner, subst), r.is_mut);
    }
    case TypeKind::named: break;
    }
    const auto& nt   = t.as<NamedType>();
    const auto& segs = nt.path.segments;
    if (segs.size() == 1 && !nt.symbol) {
        if (const Type* b = builtin(segs[0].name)) return b;
    }
    if (nt.symbol) {
        const sema::Symbol* s = nt.symbol;
        if (s->kind == sema::SymbolKind::type_param) {
            if (subst) {
                if (auto it = subst->find(s); it != subst->end()) return it->second;
            }
            return type_param(s);
        }
        if (s->is_type() && s->decl && !s->decl->is_template) return user(s);
        if (s->is_type() && s->decl && s->decl->is_template) {
            // `box<u32>`: an instance of the template for these arguments. Arguments that are
            // still unknown (inside a template that is not instantiated) make it unknown too.
            std::vector<const Type*> args;
            for (const TemplateArg& a : segs.back().template_args) {
                if (!a.type) return error_;   // value arguments are not supported yet
                args.push_back(lower(*a.type, subst));
            }
            return instance(s, args);
        }
        return error_;
    }
    if (!segs.empty() && segs[0].name == "std") {
        if (segs.size() == 2 && segs[1].name == "raw_ptr" && segs[1].template_args.size() == 1 &&
            segs[1].template_args[0].type) {
            const TemplateArg& arg = segs[1].template_args[0];
            return pointer(lower(*arg.type, subst), arg.is_mut);
        }
        const Type*              ext = external(ast::to_string(nt.path));
        std::vector<const Type*> args;
        for (const TemplateArg& a : segs.back().template_args) {
            args.push_back(a.type ? lower(*a.type, subst) : error_);
        }
        external_args_[ext] = args;
        return ext;
    }
    return error_;
}

const Type* TypeContext::instance(const sema::Symbol* templ, const std::vector<const Type*>& args) {
    auto key = std::make_pair(templ, args);
    if (auto it = instances_.find(key); it != instances_.end()) return it->second;   // made before
    if (!instantiator_ || !templ->decl || args.size() != templ->decl->template_params.size()) return error_;
    for (const Type* a : args) {
        if (a->is_unknown()) return error_;
    }
    const Type* t = instantiator_(templ, args);
    instances_[key] = t;
    return t;
}

const std::vector<const Type*>* TypeContext::external_args(const Type* t) const {
    auto it = external_args_.find(t);
    return it == external_args_.end() ? nullptr : &it->second;
}

std::string to_string(const Type* t) {
    if (!t) return "?";
    switch (t->kind) {
    case Kind::reference: return (t->is_mut ? "mut " : "") + to_string(t->inner) + "&";
    case Kind::pointer: return std::string("std::raw_ptr<") + (t->is_mut ? "mut " : "") + to_string(t->inner) + ">";
    case Kind::range: return "range<" + to_string(t->inner) + ">";
    case Kind::user: return t->symbol->name;
    default: return t->name;
    }
}

const Type* strip_ref(const Type* t) { return t && t->kind == Kind::reference ? t->inner : t; }

bool literal_fits(u128 magnitude, bool negative, const Type* t) {
    if (!t || t->kind != Kind::int_) return true;
    int bits = t->bits;
    if (!t->is_signed) {
        if (negative) return magnitude == 0;
        return bits == 128 || magnitude <= ((u128{1} << bits) - 1);
    }
    u128 limit = u128{1} << (bits - 1);   // |min|
    return negative ? magnitude <= limit : magnitude <= limit - 1;
}

bool is_mutating_library_method(const Type* receiver, std::string_view method) {
    if (!receiver || receiver->kind != Kind::external) return false;
    std::string_view base = receiver->external_base();
    static const char* containers[] = {"std::darray", "std::string", "std::hash_map", "std::tree_map", "std::bitset"};
    static const char* mutating[]   = {"push", "pop", "insert", "remove", "clear", "retain", "sort", "sort_by_key",
                                       "resize", "reserve", "append", "extend", "truncate", "erase", "set"};
    bool is_container = false;
    for (const char* c : containers) is_container = is_container || base == c;
    if (!is_container) return false;
    for (const char* m : mutating) {
        if (method == m) return true;
    }
    return false;
}

std::string integer_range(const Type* t) {
    if (!t || t->kind != Kind::int_) return "";
    int bits = t->bits;
    if (!t->is_signed) {
        u128 max = bits == 128 ? ~u128{0} : (u128{1} << bits) - 1;
        return "0 to " + rift::to_string(max);
    }
    u128 limit = u128{1} << (bits - 1);
    return "-" + rift::to_string(limit) + " to " + rift::to_string(limit - 1);
}

} // namespace rift::types

#include "rift/ast_printer.hpp"

#include "rift/types.hpp"

#include <ostream>

namespace rift::ast {

namespace {

/// A printable tree: `(head child child ...)`. Leaves with no children and `atom` set print
/// without parentheses.
struct S {
    std::string    head;
    std::vector<S> kids;
    bool           atom = false;
};

S atom(std::string text) { return S{std::move(text), {}, true}; }

std::string quote(std::string_view text) {
    std::string out = "\"";
    for (char c : text) {
        switch (c) {
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        case '\0': out += "\\0"; break;
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        default: out.push_back(c);
        }
    }
    return out + "\"";
}

std::string inline_form(const S& s) {
    if (s.atom) {
        return s.head;
    }
    std::string out = "(" + s.head;
    for (const S& kid : s.kids) {
        out += " " + inline_form(kid);
    }
    return out + ")";
}

constexpr std::size_t line_width = 100;

void render(std::ostream& out, const S& s, std::size_t indent) {
    std::string flat = inline_form(s);
    if (s.atom || indent + flat.size() <= line_width || s.kids.empty()) {
        out << flat;
        return;
    }
    out << "(" << s.head;
    for (const S& kid : s.kids) {
        out << "\n" << std::string(indent + 2, ' ');
        render(out, kid, indent + 2);
    }
    out << ")";
}

// --- builders -----------------------------------------------------------------------------

S build(const Expr& e);
S build(const Pattern& p);
S build(const Stmt& s);
S build(const Decl& d);

std::string template_args(const PathSegment& seg) {
    if (!seg.has_template_args) {
        return "";
    }
    std::string out = "<";
    for (std::size_t i = 0; i < seg.template_args.size(); ++i) {
        const TemplateArg& arg = seg.template_args[i];
        out += i ? ", " : "";
        out += arg.is_mut ? "mut " : "";
        out += arg.type ? to_string(*arg.type) : inline_form(build(*arg.value));
    }
    return out + ">";
}

S build_type(const TypeExpr& t) { return atom(to_string(t)); }

S build_block(const BlockExpr& b) {
    S s{b.is_unsafe ? "unsafe-block" : "block", {}};
    for (const StmtPtr& stmt : b.statements) {
        s.kids.push_back(build(*stmt));
    }
    if (b.tail) {
        s.kids.push_back(S{"value", {build(*b.tail)}});
    }
    return s;
}

std::string literal_head(const LiteralExpr& lit) {
    switch (lit.kind) {
    case ExprKind::int_literal: return "int " + lit.text;
    case ExprKind::float_literal: return "float " + lit.text;
    case ExprKind::char_literal: return "char " + quote(lit.value);
    case ExprKind::bool_literal: return lit.bool_value ? "true" : "false";
    case ExprKind::nullptr_literal: return "nullptr";
    case ExprKind::string_literal:
        switch (lit.token_kind) {
        case TokenKind::raw_string_literal: return "raw-string " + quote(lit.value);
        case TokenKind::format_string_literal: return "format-string " + quote(lit.value);
        default: return "string " + quote(lit.value);
        }
    default: return "literal";
    }
}

std::string_view unary_name(UnaryOp op) {
    switch (op) {
    case UnaryOp::negate: return "-";
    case UnaryOp::logical_not: return "!";
    case UnaryOp::bit_not: return "~";
    case UnaryOp::deref: return "deref";
    case UnaryOp::address_of: return "addr";
    case UnaryOp::address_of_mut: return "addr-mut";
    case UnaryOp::move: return "move";
    case UnaryOp::mut_marker: return "mut";
    }
    return "?";
}

std::string_view capture_name(CaptureKind k) {
    switch (k) {
    case CaptureKind::copy: return "copy";
    case CaptureKind::move: return "move";
    case CaptureKind::ref: return "ref";
    case CaptureKind::mut_ref: return "mut-ref";
    case CaptureKind::init: return "init";
    }
    return "?";
}

S build_param(const Param& p) {
    return S{"param " + p.name + " : " + std::string(p.is_mut ? "mut " : "") + to_string(*p.type), {}};
}

S build_untyped(const Expr& e);

bool g_show_types = false;   // `riftc typed`: append each expression's type

S build(const Expr& e) {
    S s = build_untyped(e);
    if (g_show_types && e.type) {
        s.head += " : " + types::to_string(e.type);
        s.atom = false;
    }
    return s;
}

S build_untyped(const Expr& e) {
    switch (e.kind) {
    case ExprKind::int_literal:
    case ExprKind::float_literal:
    case ExprKind::char_literal:
    case ExprKind::string_literal:
    case ExprKind::bool_literal:
    case ExprKind::nullptr_literal: {
        const auto& lit = e.as<LiteralExpr>();
        S s{literal_head(lit), {}};
        for (const FormatPart& part : lit.format_parts) {
            if (!part.expr) continue;
            S hole{"hole", {build(*part.expr)}};
            if (part.spec.present) {
                std::string spec;
                if (part.spec.align) spec += part.spec.align;
                if (part.spec.plus) spec += '+';
                if (part.spec.width >= 0) spec += std::to_string(part.spec.width);
                if (part.spec.precision >= 0) spec += "." + std::to_string(part.spec.precision);
                if (part.spec.radix) spec += part.spec.radix;
                hole.head += " :" + spec;
            }
            hole.atom = false;
            s.kids.push_back(std::move(hole));
            s.atom = false;
        }
        return s;
    }
    case ExprKind::this_: return S{"this", {}};
    case ExprKind::default_: return S{"default", {}};
    case ExprKind::break_: return S{"break", {}};
    case ExprKind::continue_: return S{"continue", {}};
    case ExprKind::error: return S{"error", {}};
    case ExprKind::path: return S{"path " + to_string(e.as<PathExpr>().path), {}};
    case ExprKind::unary: {
        const auto& u = e.as<UnaryExpr>();
        return S{std::string(unary_name(u.op)), {build(*u.operand)}};
    }
    case ExprKind::binary: {
        const auto& b = e.as<BinaryExpr>();
        return S{std::string(token_kind_spelling(b.op)), {build(*b.lhs), build(*b.rhs)}};
    }
    case ExprKind::range: {
        const auto& r = e.as<RangeExpr>();
        return S{r.inclusive ? "..=" : "..", {build(*r.lo), build(*r.hi)}};
    }
    case ExprKind::call: {
        const auto& c = e.as<CallExpr>();
        S s{"call", {build(*c.callee)}};
        for (const ExprPtr& arg : c.args) {
            s.kids.push_back(build(*arg));
        }
        return s;
    }
    case ExprKind::index: {
        const auto& i = e.as<IndexExpr>();
        return S{"index", {build(*i.object), build(*i.index)}};
    }
    case ExprKind::member: {
        const auto& m = e.as<MemberExpr>();
        return S{std::string(m.arrow ? "->" : ".") + to_string(m.name), {build(*m.object)}};
    }
    case ExprKind::try_: return S{"?", {build(*e.as<TryExpr>().operand)}};
    case ExprKind::cast: {
        const auto& c = e.as<CastExpr>();
        std::string head = c.variant.empty() ? "cast" : "cast::" + c.variant;
        return S{head + " " + to_string(*c.target), {build(*c.operand)}};
    }
    case ExprKind::bit: {
        const auto& b = e.as<BitExpr>();
        return S{b.type ? "bit " + to_string(*b.type) : "bit", {build(*b.operand)}};
    }
    case ExprKind::struct_literal: {
        const auto& sl = e.as<StructLiteralExpr>();
        S s{"new " + to_string(*sl.type), {}};
        for (const FieldInit& f : sl.fields) {
            s.kids.push_back(S{"." + f.name, {build(*f.value)}});
        }
        return s;
    }
    case ExprKind::lambda: {
        const auto& l = e.as<LambdaExpr>();
        S captures{"captures", {}};
        for (const Capture& c : l.captures) {
            S cap{std::string(capture_name(c.kind)) + " " + c.name, {}};
            if (c.init) {
                cap.kids.push_back(build(*c.init));
            }
            captures.kids.push_back(std::move(cap));
        }
        S s{"lambda", {std::move(captures)}};
        for (const Param& p : l.params) {
            s.kids.push_back(build_param(p));
        }
        if (l.result) {
            s.kids.push_back(S{"-> " + to_string(*l.result), {}});
        }
        s.kids.push_back(build_block(*l.body));
        return s;
    }
    case ExprKind::if_: {
        const auto& i = e.as<IfExpr>();
        S s{i.is_compile_time ? "if-compile-time" : "if", {build(*i.condition), build_block(*i.then_block)}};
        if (i.else_branch) {
            s.kids.push_back(S{"else", {build(*i.else_branch)}});
        }
        return s;
    }
    case ExprKind::match: {
        const auto& m = e.as<MatchExpr>();
        S s{"match", {build(*m.scrutinee)}};
        for (const MatchArm& arm : m.arms) {
            S a{"arm", {build(*arm.pattern)}};
            if (arm.guard) {
                a.kids.push_back(S{"if", {build(*arm.guard)}});
            }
            a.kids.push_back(build(*arm.body));
            s.kids.push_back(std::move(a));
        }
        return s;
    }
    case ExprKind::block: return build_block(e.as<BlockExpr>());
    case ExprKind::while_: {
        const auto& w = e.as<WhileExpr>();
        return S{"while", {build(*w.condition), build_block(*w.body)}};
    }
    case ExprKind::for_: {
        const auto& f = e.as<ForExpr>();
        std::string head = "for " + f.name + " : " + (f.is_mut ? "mut " : "") + to_string(*f.type);
        return S{head, {S{"in", {build(*f.iterable)}}, build_block(*f.body)}};
    }
    case ExprKind::return_: {
        const auto& r = e.as<ReturnExpr>();
        S s{"return", {}};
        if (r.value) {
            s.kids.push_back(build(*r.value));
        }
        return s;
    }
    }
    return S{"?", {}};
}

S build(const Pattern& p) {
    switch (p.kind) {
    case PatternKind::wildcard: return atom("_");
    case PatternKind::error: return S{"error", {}};
    case PatternKind::literal: return build(*p.as<LiteralPattern>().literal);
    case PatternKind::range: {
        const auto& r = p.as<RangePattern>();
        return S{r.inclusive ? "..=" : "..", {build(*r.lo), build(*r.hi)}};
    }
    case PatternKind::binding: return S{"bind " + p.as<BindingPattern>().name, {}};
    case PatternKind::enum_alternative: {
        const auto& ep = p.as<EnumPattern>();
        S s{"alt " + to_string(ep.path), {}};
        if (ep.payload) {
            s.kids.push_back(build(*ep.payload));
        }
        return s;
    }
    case PatternKind::struct_: {
        const auto& sp = p.as<StructPattern>();
        S s{"destructure " + to_string(sp.path), {}};
        for (const FieldPattern& f : sp.fields) {
            s.kids.push_back(S{"." + f.name, {build(*f.pattern)}});
        }
        if (sp.has_rest) {
            s.kids.push_back(atom(".."));
        }
        return s;
    }
    }
    return S{"?", {}};
}

std::string storage_prefix(Storage storage) {
    switch (storage) {
    case Storage::compile_time: return "compile_time ";
    case Storage::static_: return "static ";
    case Storage::local: return "";
    }
    return "";
}

S build(const Stmt& s) {
    switch (s.kind) {
    case StmtKind::variable: {
        const auto& v = s.as<VariableStmt>();
        std::string head = storage_prefix(v.storage) + "var " + v.name + " : " +
                           (v.is_mut ? "mut " : "") + to_string(*v.type);
        S out{head, {}};
        if (v.init) {
            out.kids.push_back(build(*v.init));
        }
        return out;
    }
    case StmtKind::assign: {
        const auto& a = s.as<AssignStmt>();
        return S{"assign " + std::string(token_kind_spelling(a.op)), {build(*a.target), build(*a.value)}};
    }
    case StmtKind::discard: return S{"discard", {build(*s.as<DiscardStmt>().value)}};
    case StmtKind::expression: return build(*s.as<ExprStmt>().expr);
    }
    return S{"?", {}};
}

std::string_view access_name(Access a) {
    switch (a) {
    case Access::public_: return "public ";
    case Access::protected_: return "protected ";
    case Access::private_: return "private ";
    case Access::none: return "";
    }
    return "";
}

void add_common(S& s, const Decl& d) {
    if (d.is_template) {
        S t{"template", {}};
        for (const TemplateParam& p : d.template_params) {
            if (p.is_type) {
                std::string text = "type " + p.name;
                for (std::size_t i = 0; i < p.constraints.size(); ++i) {
                    text += (i ? " & " : " : ") + to_string(*p.constraints[i]);
                }
                t.kids.push_back(S{text, {}});
            } else {
                t.kids.push_back(S{"value " + p.name + " : " + to_string(*p.value_type), {}});
            }
        }
        s.kids.insert(s.kids.begin(), std::move(t));
    }
    if (!d.doc.empty()) {
        s.kids.insert(s.kids.begin(), S{"doc " + quote(d.doc), {}});
    }
}

S build(const Decl& d) {
    std::string prefix = std::string(d.is_private ? "private " : "") + std::string(access_name(d.access));
    S s;
    switch (d.kind) {
    case DeclKind::import: s = S{"import " + to_string(d.as<ImportDecl>().path), {}}; break;
    case DeclKind::function: {
        const auto& f = d.as<FunctionDecl>();
        std::string head = prefix;
        head += f.is_extern ? "extern " + quote(f.abi) + " " : "";
        head += f.is_unsafe ? "unsafe " : "";
        head += f.is_static ? "static " : "";
        head += f.is_virtual ? "virtual " : "";
        head += f.is_compile_time ? "compile_time " : "";
        head += f.is_run_time ? "run_time " : "";
        head += f.is_destructor ? "destructor " : f.is_operator ? "operator" : "fn ";
        head += f.name;
        if (f.result) {
            head += " -> " + to_string(*f.result);
        }
        head += f.is_mut ? " mut" : "";
        head += f.is_override ? " override" : "";
        head += f.is_abstract ? " abstract" : "";
        head += f.is_defaulted ? " = default" : "";
        s = S{head, {}};
        for (const Param& p : f.params) {
            s.kids.push_back(build_param(p));
        }
        if (f.body) {
            s.kids.push_back(build_block(*f.body));
        }
        break;
    }
    case DeclKind::variable:
    case DeclKind::field: {
        const auto& v = d.as<VariableDecl>();
        std::string head = prefix + storage_prefix(v.storage) +
                           (d.kind == DeclKind::field ? "field " : "var ") + d.name + " : " +
                           (v.is_mut ? "mut " : "") + to_string(*v.type);
        s = S{head, {}};
        if (v.init) {
            s.kids.push_back(build(*v.init));
        }
        break;
    }
    case DeclKind::struct_:
    case DeclKind::class_:
    case DeclKind::interface: {
        const auto& t = d.as<TypeDecl>();
        std::string head = prefix + (t.is_open ? "open " : "") + (t.is_view ? "view " : "");
        head += d.kind == DeclKind::struct_ ? "struct " : d.kind == DeclKind::class_ ? "class " : "interface ";
        head += d.name;
        for (std::size_t i = 0; i < t.bases.size(); ++i) {
            head += (i ? ", " : " : ") + to_string(*t.bases[i]);
        }
        s = S{head, {}};
        for (const DeclPtr& m : t.members) {
            s.kids.push_back(build(*m));
        }
        break;
    }
    case DeclKind::enum_: {
        const auto& en = d.as<EnumDecl>();
        s = S{prefix + "enum " + d.name + " : " + to_string(*en.tag_type), {}};
        for (const EnumAlternative& alt : en.alternatives) {
            S a{alt.name, {}};
            if (alt.payload) {
                a.kids.push_back(build_type(*alt.payload));
            }
            if (alt.value) {
                a.kids.push_back(S{"=", {build(*alt.value)}});
            }
            s.kids.push_back(std::move(a));
        }
        break;
    }
    }
    add_common(s, d);
    return s;
}

} // namespace

std::string to_string(const Path& path) {
    std::string out;
    for (std::size_t i = 0; i < path.segments.size(); ++i) {
        out += (i ? "::" : "") + path.segments[i].name + template_args(path.segments[i]);
    }
    return out;
}

std::string to_string(const TypeExpr& type) {
    switch (type.kind) {
    case TypeKind::named: return to_string(type.as<NamedType>().path);
    case TypeKind::reference: {
        const auto& r = type.as<ReferenceType>();
        return (r.is_mut ? "mut " : "") + to_string(*r.inner) + "&";
    }
    case TypeKind::function: {
        const auto& f = type.as<FunctionType>();
        std::string out = to_string(*f.result) + "(";
        for (std::size_t i = 0; i < f.params.size(); ++i) {
            out += (i ? ", " : "") + to_string(*f.params[i]);
        }
        return out + ")";
    }
    case TypeKind::auto_: return "auto";
    case TypeKind::deduce: return "deduce";
    }
    return "?";
}

std::string to_sexpr(const Expr& expr) { return inline_form(build(expr)); }

void print(std::ostream& out, const File& file, bool with_types) {
    g_show_types = with_types;
    for (const DeclPtr& decl : file.decls) {
        render(out, build(*decl), 0);
        out << '\n';
    }
    g_show_types = false;
}

} // namespace rift::ast

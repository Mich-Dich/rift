#include "rift/compile_time.hpp"

#include "rift/ast_printer.hpp"
#include "rift/source.hpp"

#include <cmath>
#include <format>

namespace rift::ct {

using namespace ast;
using sema::Symbol;
using sema::SymbolKind;
using types::Kind;
using types::Type;

namespace {

u128 mask(u128 v, int bits) { return bits >= 128 ? v : v & ((u128{1} << bits) - 1); }

i128 signed_min(int bits) { return bits >= 128 ? static_cast<i128>(u128{1} << 127) : -(static_cast<i128>(1) << (bits - 1)); }
i128 signed_max(int bits) { return bits >= 128 ? static_cast<i128>(~u128{0} >> 1) : (static_cast<i128>(1) << (bits - 1)) - 1; }
u128 unsigned_max(int bits) { return bits >= 128 ? ~u128{0} : (u128{1} << bits) - 1; }

double round_f32(double v) { return static_cast<double>(static_cast<float>(v)); }

std::string signed_text(i128 v) {
    if (v >= 0) return rift::to_string(static_cast<u128>(v));
    return "-" + rift::to_string(static_cast<u128>(-(v + 1)) + 1);
}

/// Thrown to abandon an evaluation: after an error was reported, or (unknown) silently.
struct Stop {
    bool unknown = false;
};

enum class Flow { normal, break_, continue_, return_ };

const FunctionDecl* body_decl(const Symbol* s) {
    const Decl* d = s->definition ? s->definition : s->decl;
    return d && d->kind == DeclKind::function ? &d->as<FunctionDecl>() : nullptr;
}

} // namespace

// --- values --------------------------------------------------------------------------------

Value Value::of_integer(u128 bits, const Type* t) {
    Value v;
    v.kind    = Kind::integer;
    v.type    = t;
    v.integer = mask(bits, t && t->is_integer() ? t->bits : 128);
    return v;
}

Value Value::of_bool(bool b, const Type* t) {
    Value v;
    v.kind    = Kind::boolean;
    v.type    = t;
    v.boolean = b;
    return v;
}

Value Value::of_type(const Type* denoted, const Type* type_type) {
    Value v;
    v.kind       = Kind::type;
    v.type       = type_type;
    v.type_value = denoted;
    return v;
}

i128 Value::as_signed() const {
    int bits = type && type->is_integer() ? type->bits : 128;
    if (!type || !type->is_signed || bits >= 128) return static_cast<i128>(integer);
    u128 sign = u128{1} << (bits - 1);
    return (integer & sign) ? static_cast<i128>(integer | ~unsigned_max(bits)) : static_cast<i128>(integer);
}

std::string to_string(const Value& v) {
    switch (v.kind) {
    case Value::Kind::void_: return "()";
    case Value::Kind::integer:
        return v.type && v.type->is_signed ? signed_text(v.as_signed()) : rift::to_string(v.integer);
    case Value::Kind::floating: return std::format("{}", v.floating);
    case Value::Kind::boolean: return v.boolean ? "true" : "false";
    case Value::Kind::character: {
        std::string s;
        encode_utf8(v.character, s);
        return "'" + s + "'";
    }
    case Value::Kind::string: return "\"" + v.string + "\"";
    case Value::Kind::type: return types::to_string(v.type_value);
    case Value::Kind::aggregate: {
        std::string out = types::to_string(v.type) + "{";
        std::size_t i   = 0;
        if (v.type && v.type->is(Kind::user)) {
            for (const Symbol* f : v.type->symbol->member_order) {
                if (f->kind != SymbolKind::field || f->is_static || i >= v.elements.size()) continue;
                out += std::format("{} .{} = {}", i ? "," : "", f->name, to_string(v.elements[i]));
                ++i;
            }
        }
        return out + " }";
    }
    case Value::Kind::enumeration: {
        std::string out = types::to_string(v.type) + "::" + (v.symbol ? v.symbol->name : "?");
        if (!v.elements.empty()) out += "(" + to_string(v.elements[0]) + ")";
        return out;
    }
    case Value::Kind::list: {
        std::string out = "[";
        for (std::size_t i = 0; i < v.elements.size(); ++i) out += (i ? ", " : "") + to_string(v.elements[i]);
        return out + "]";
    }
    case Value::Kind::field: return std::format("field '{}' of {}", v.symbol->name, types::to_string(v.type_value));
    case Value::Kind::alternative: return std::format("alternative '{}' of {}", v.symbol->name, types::to_string(v.type_value));
    }
    return "?";
}

bool equal(const Value& a, const Value& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
    case Value::Kind::void_: return true;
    case Value::Kind::integer: return a.integer == b.integer;
    case Value::Kind::floating: return a.floating == b.floating;
    case Value::Kind::boolean: return a.boolean == b.boolean;
    case Value::Kind::character: return a.character == b.character;
    case Value::Kind::string: return a.string == b.string;
    case Value::Kind::type: return a.type_value == b.type_value;
    case Value::Kind::field:
    case Value::Kind::alternative: return a.symbol == b.symbol && a.type_value == b.type_value;
    case Value::Kind::enumeration:
        if (!a.symbol || !b.symbol || a.symbol->name != b.symbol->name) return false;
        [[fallthrough]];
    case Value::Kind::aggregate:
    case Value::Kind::list:
        if (a.elements.size() != b.elements.size()) return false;
        for (std::size_t i = 0; i < a.elements.size(); ++i) {
            if (!equal(a.elements[i], b.elements[i])) return false;
        }
        return true;
    }
    return false;
}

std::string builtin_name(const CallExpr& c) {
    const Expr& callee = *c.callee;
    if (callee.kind != ExprKind::path) return {};
    const auto& pe = callee.as<PathExpr>();
    if (pe.symbol) return {};
    const auto& segs = pe.path.segments;
    if (segs.size() == 2 && segs[0].name == "std") {
        const std::string& n = segs[1].name;
        if (n == "is_integer" || n == "is_float" || n == "is_same" || n == "size_of" || n == "align_of" || n == "type_name") {
            return n;
        }
    }
    if (segs.size() == 3 && segs[0].name == "std" && segs[1].name == "meta") {
        const std::string& n = segs[2].name;
        if (n == "fields" || n == "alternatives" || n == "is_struct" || n == "is_class" || n == "is_enum") return "meta::" + n;
    }
    return {};
}

// --- the interpreter -----------------------------------------------------------------------

struct Evaluator::Impl {
    struct Frame {
        const Symbol*                  function = nullptr;
        Span                           call_site;
        std::map<const Symbol*, Value> locals;
        const Type*                    result   = nullptr;
        std::optional<Value>           self;
    };

    sema::Program&      program;
    DiagnosticEngine&   diags;
    Hooks               hooks;
    types::TypeContext& tc;
    std::vector<Frame>  frames;
    std::size_t         steps = 0;
    Value               returned;
    std::string         why;   // what the evaluation is for, for messages

    static constexpr std::size_t max_steps = 2'000'000;
    static constexpr std::size_t max_depth = 256;

    Impl(sema::Program& p, DiagnosticEngine& d, Hooks h) : program(p), diags(d), hooks(std::move(h)), tc(*p.types) {}

    // --- errors ------------------------------------------------------------------------

    [[noreturn]] void unknown() { throw Stop{true}; }

    /// "while running 'f' at compile time" for the innermost calls, then the outermost one.
    void call_notes(Diagnostic& d) {
        std::vector<const Frame*> calls;
        for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
            if (it->function) calls.push_back(&*it);
        }
        constexpr std::size_t shown = 4;
        for (std::size_t i = 0; i < calls.size(); ++i) {
            if (calls.size() > shown + 1 && i == shown) {
                d.notes.push_back(Note{std::format("... {} more calls ...", calls.size() - shown - 1), std::nullopt});
                i = calls.size() - 1;
            }
            d.notes.push_back(Note{std::format("while running '{}' at compile time", calls[i]->function->name), calls[i]->call_site});
        }
    }

    [[noreturn]] void not_constant(Span span, std::string message, std::string note = {}) {
        Diagnostic& d = diags.report(DiagCode::E4200, span, std::move(message), "not known at compile time");
        if (!note.empty()) d.notes.push_back(Note{std::move(note), std::nullopt});
        if (!why.empty()) d.notes.push_back(Note{why, std::nullopt});
        call_notes(d);
        throw Stop{};
    }

    [[noreturn]] void failed(Span span, std::string message, std::string label) {
        Diagnostic& d = diags.report(DiagCode::E4201, span, std::move(message), std::move(label));
        if (!why.empty()) d.notes.push_back(Note{why, std::nullopt});
        call_notes(d);
        throw Stop{};
    }

    void step(Span span) {
        if (++steps > max_steps) {
            failed(span, std::format("compile-time evaluation took more than {} steps", max_steps), "still running here");
        }
    }

    // --- types -------------------------------------------------------------------------

    const Type* lower(const TypeExpr& t) {
        bool        top = frames.empty() || !frames.back().function;
        const Type* r   = top ? hooks.lower(t) : tc.lower(t, sema::substitution_of(frames.back().function));
        if (!r || r->is_unknown()) unknown();
        return r;
    }

    const Type* type_of(const Expr& e) {
        const Type* t = e.type ? types::strip_ref(e.type) : nullptr;
        if (!t || t->is_unknown() || t->is_untyped()) unknown();
        return t;
    }

    const Type* usize() { return tc.builtin("usize"); }

    Value string_value(std::string s) {
        Value v;
        v.kind   = Value::Kind::string;
        v.type   = tc.static_str();
        v.string = std::move(s);
        return v;
    }

    Value integer(u128 bits, const Type* t) { return Value::of_integer(bits, t); }

    /// The type a type symbol names in a path (`point`, `T`, `box<u32>`).
    const Type* type_of_symbol(const Symbol* s, const Path& path) {
        if (s->kind == SymbolKind::type_param) {
            if (frames.empty() || !frames.back().function) {
                const Type* t = hooks.substitute(s);
                if (!t || t->is_unknown()) unknown();
                return t;
            }
            if (const auto* subst = sema::substitution_of(frames.back().function)) {
                if (auto it = subst->find(s); it != subst->end()) return it->second;
            }
            unknown();
        }
        if (s->decl && s->decl->is_template && !s->template_of) {
            const PathSegment& seg = path.segments.back();
            std::vector<const Type*> args;
            for (const TemplateArg& a : seg.template_args) {
                if (!a.type) unknown();
                args.push_back(lower(*a.type));
            }
            const Type* t = tc.instance(s, args);
            if (t->is_unknown()) unknown();
            return t;
        }
        return tc.user(s);
    }

    // --- expressions ---------------------------------------------------------------------

    Value eval(const Expr& e) {
        step(e.span);
        if (e.constant) return *e.constant;
        switch (e.kind) {
        case ExprKind::int_literal: {
            const Type* t = type_of(e);
            if (!t->is_integer()) unknown();
            return integer(e.as<LiteralExpr>().int_value, t);
        }
        case ExprKind::float_literal: {
            const Type* t   = type_of(e);
            std::string txt = e.as<LiteralExpr>().text;
            std::string digits;
            for (char ch : txt) {
                if (ch == '_') continue;
                if (ch == 'f' && !digits.empty()) break;   // the suffix
                digits += ch;
            }
            Value v;
            v.kind     = Value::Kind::floating;
            v.type     = t;
            v.floating = std::stod(digits);
            if (t->bits == 32) v.floating = round_f32(v.floating);
            return v;
        }
        case ExprKind::bool_literal: return Value::of_bool(e.as<LiteralExpr>().bool_value, tc.bool_());
        case ExprKind::char_literal: {
            const std::string& text = e.as<LiteralExpr>().value;
            char32_t cp = 0;
            if (!text.empty() && decode_utf8(text, 0, cp) == 0) cp = static_cast<unsigned char>(text[0]);
            Value v;
            v.kind      = Value::Kind::character;
            v.type      = tc.char_();
            v.character = cp;
            return v;
        }
        case ExprKind::string_literal: {
            const auto& lit = e.as<LiteralExpr>();
            if (lit.token_kind == TokenKind::format_string_literal) {
                not_constant(e.span, "f-strings cannot be built at compile time yet");
            }
            return string_value(lit.value);
        }
        case ExprKind::path: return path_value(e.as<PathExpr>());
        case ExprKind::this_:
            if (frames.empty() || !frames.back().self) not_constant(e.span, "'this' is not known at compile time");
            return *frames.back().self;
        case ExprKind::unary: return unary(e.as<UnaryExpr>());
        case ExprKind::binary: return binary(e.as<BinaryExpr>());
        case ExprKind::call: return call(e.as<CallExpr>());
        case ExprKind::member: return member(e.as<MemberExpr>());
        case ExprKind::index: {
            const auto& ix = e.as<IndexExpr>();
            Value       o  = eval(*ix.object);
            Value       i  = eval(*ix.index);
            if (o.kind != Value::Kind::list) not_constant(e.span, "only lists can be indexed at compile time");
            if (i.integer >= o.elements.size()) {
                failed(ix.index->span, std::format("index {} is out of bounds for a list of {} elements", rift::to_string(i.integer),
                                                   o.elements.size()),
                       "out of bounds");
            }
            return o.elements[static_cast<std::size_t>(i.integer)];
        }
        case ExprKind::cast: return cast(e.as<CastExpr>());
        case ExprKind::bit: {
            const Type* t = type_of(e);
            Value       n = eval(*e.as<BitExpr>().operand);
            if (!t->is_integer() || n.integer >= static_cast<u128>(t->bits)) {
                failed(e.span, std::format("bit({}) is out of range for {}", rift::to_string(n.integer), types::to_string(t)),
                       "out of range");
            }
            return integer(u128{1} << static_cast<int>(n.integer), t);
        }
        case ExprKind::struct_literal: return struct_literal(e.as<StructLiteralExpr>());
        case ExprKind::default_: return default_of(type_of(e), e.span);
        case ExprKind::block: return block(e.as<BlockExpr>());
        case ExprKind::if_: return if_value(e.as<IfExpr>());
        case ExprKind::match: return match_value(e.as<MatchExpr>());
        case ExprKind::while_: {
            const auto& w = e.as<WhileExpr>();
            while (truth(eval(*w.condition))) {
                Flow f = run_block(*w.body);
                if (f == Flow::break_) break;
                if (f == Flow::return_) throw f;
            }
            return Value{};
        }
        case ExprKind::for_: return for_loop(e.as<ForExpr>());
        case ExprKind::return_: {
            const auto& r = e.as<ReturnExpr>();
            returned      = r.value ? eval(*r.value) : Value{};
            throw Flow::return_;
        }
        case ExprKind::break_: throw Flow::break_;
        case ExprKind::continue_: throw Flow::continue_;
        case ExprKind::try_: {
            const auto& t = e.as<TryExpr>();
            Value       v = eval(*t.operand);
            if (v.kind != Value::Kind::enumeration) not_constant(e.span, "'?' needs a std::option or std::result here");
            if (v.symbol->name == "ok" || v.symbol->name == "some") return v.elements.at(0);
            if (frames.empty() || !frames.back().result || !frames.back().result->is(Kind::user)) {
                not_constant(e.span, "'?' can only return from a compile_time function");
            }
            const Type* r = frames.back().result;
            Value       out;
            out.kind   = Value::Kind::enumeration;
            out.type   = r;
            out.symbol = r->symbol->members.at(v.symbol->name).front();
            out.elements = v.elements;
            returned     = out;
            throw Flow::return_;
        }
        case ExprKind::range: not_constant(e.span, "a range is only a value in a 'for' loop");
        case ExprKind::nullptr_literal: not_constant(e.span, "pointers do not exist at compile time");
        case ExprKind::lambda: not_constant(e.span, "lambdas cannot run at compile time yet");
        case ExprKind::error: unknown();
        }
        unknown();
    }

    bool truth(const Value& v) {
        if (v.kind == Value::Kind::boolean) return v.boolean;
        if (v.kind == Value::Kind::integer) return v.integer != 0;
        unknown();
    }

    Value path_value(const PathExpr& pe) {
        const Symbol* s = pe.symbol;
        if (!s) {
            if (pe.path.segments.size() == 1) {
                if (const Type* b = tc.builtin(pe.path.segments[0].name)) return Value::of_type(b, tc.type_value());
            }
            not_constant(pe.span, std::format("'{}' is not known at compile time", ast::to_string(pe.path)));
        }
        switch (s->kind) {
        case SymbolKind::local: {
            for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
                if (auto found = it->locals.find(s); found != it->locals.end()) return found->second;
                break;   // locals of other frames are not visible
            }
            if (s->ct_value) return *s->ct_value;
            if (auto v = hooks.constant_value(s)) return *v;
            if (hooks.depends_on_template && hooks.depends_on_template(s)) unknown();
            not_constant(pe.span, std::format("'{}' is not a compile-time value", s->name),
                         "only literals, compile_time constants and the results of compile_time functions are known "
                         "at compile time; declare it 'compile_time ...'");
        }
        case SymbolKind::global: {
            if (s->ct_value) return *s->ct_value;
            if (auto v = hooks.constant_value(s)) return *v;
            not_constant(pe.span, std::format("'{}' is not a compile-time value", s->name),
                         "a 'static' variable is initialized at run time; use 'compile_time' for a constant");
        }
        case SymbolKind::alternative: {
            Value v;
            v.kind   = Value::Kind::enumeration;
            v.type   = type_of(pe);
            v.symbol = s;
            if (!s->parent || !s->parent->decl) unknown();
            return v;
        }
        case SymbolKind::struct_:
        case SymbolKind::class_:
        case SymbolKind::enum_:
        case SymbolKind::interface:
        case SymbolKind::type_param: return Value::of_type(type_of_symbol(s, pe.path), tc.type_value());
        default:
            not_constant(pe.span, std::format("'{}' is not a compile-time value", ast::to_string(pe.path)));
        }
    }

    Value unary(const UnaryExpr& u) {
        switch (u.op) {
        case UnaryOp::move:
        case UnaryOp::mut_marker: return eval(*u.operand);
        case UnaryOp::logical_not: return Value::of_bool(!truth(eval(*u.operand)), tc.bool_());
        case UnaryOp::bit_not: {
            Value v = eval(*u.operand);
            return integer(~v.integer, v.type);
        }
        case UnaryOp::negate: {
            const Type* t = type_of(u);
            if (t->is_float()) {
                Value v = eval(*u.operand);
                v.floating = -v.floating;
                return v;
            }
            if (!t->is_integer()) unknown();
            if (u.operand->kind == ExprKind::int_literal) {   // `-128i8`: the literal is checked with its sign
                return integer(u128{0} - u.operand->as<LiteralExpr>().int_value, t);
            }
            Value v = eval(*u.operand);
            if (!t->is_signed) failed(u.span, "cannot negate an unsigned value", "unsigned");
            if (v.as_signed() == signed_min(t->bits)) {
                failed(u.span, std::format("-({}) overflows {}", to_string(v), types::to_string(t)), "overflow");
            }
            return integer(static_cast<u128>(-v.as_signed()), t);
        }
        default: not_constant(u.span, "pointers and addresses do not exist at compile time");
        }
    }

    Value binary(const BinaryExpr& b) {
        TokenKind op = b.op;
        if (op == TokenKind::amp_amp || op == TokenKind::pipe_pipe) {
            bool l = truth(eval(*b.lhs));
            if (op == TokenKind::amp_amp ? !l : l) return Value::of_bool(l, tc.bool_());
            return Value::of_bool(truth(eval(*b.rhs)), tc.bool_());
        }
        Value l = eval(*b.lhs);
        Value r = eval(*b.rhs);
        return apply(op, l, r, b.span, b.rhs->span);
    }

    Value apply(TokenKind op, const Value& l, const Value& r, Span span, Span rhs_span) {
        if (op == TokenKind::eq_eq) return Value::of_bool(equal(l, r), tc.bool_());
        if (op == TokenKind::bang_eq) return Value::of_bool(!equal(l, r), tc.bool_());
        if (l.kind == Value::Kind::floating) {
            double a = l.floating, c = r.floating, x = 0;
            switch (op) {
            case TokenKind::less: return Value::of_bool(a < c, tc.bool_());
            case TokenKind::less_eq: return Value::of_bool(a <= c, tc.bool_());
            case TokenKind::greater: return Value::of_bool(a > c, tc.bool_());
            case TokenKind::greater_eq: return Value::of_bool(a >= c, tc.bool_());
            case TokenKind::plus: x = a + c; break;
            case TokenKind::minus: x = a - c; break;
            case TokenKind::star: x = a * c; break;
            case TokenKind::slash: x = a / c; break;
            case TokenKind::percent: x = std::fmod(a, c); break;
            default: unknown();
            }
            Value v = l;
            v.floating = l.type && l.type->bits == 32 ? round_f32(x) : x;
            return v;
        }
        if (l.kind == Value::Kind::character) {
            switch (op) {
            case TokenKind::less: return Value::of_bool(l.character < r.character, tc.bool_());
            case TokenKind::less_eq: return Value::of_bool(l.character <= r.character, tc.bool_());
            case TokenKind::greater: return Value::of_bool(l.character > r.character, tc.bool_());
            case TokenKind::greater_eq: return Value::of_bool(l.character >= r.character, tc.bool_());
            default: unknown();
            }
        }
        if (l.kind == Value::Kind::boolean) {
            switch (op) {
            case TokenKind::amp: return Value::of_bool(l.boolean && r.boolean, tc.bool_());
            case TokenKind::pipe: return Value::of_bool(l.boolean || r.boolean, tc.bool_());
            case TokenKind::caret: return Value::of_bool(l.boolean != r.boolean, tc.bool_());
            default: unknown();
            }
        }
        if (l.kind != Value::Kind::integer) unknown();
        const Type* t    = l.type;
        int         bits = t->bits;
        bool        sgn  = t->is_signed;
        auto overflow = [&](const char* what) -> Value {
            failed(span, std::format("{} {} {} overflows {}", to_string(l), what, to_string(r), types::to_string(t)), "overflow");
        };
        switch (op) {
        case TokenKind::less:
            return Value::of_bool(sgn ? l.as_signed() < r.as_signed() : l.integer < r.integer, tc.bool_());
        case TokenKind::less_eq:
            return Value::of_bool(sgn ? l.as_signed() <= r.as_signed() : l.integer <= r.integer, tc.bool_());
        case TokenKind::greater:
            return Value::of_bool(sgn ? l.as_signed() > r.as_signed() : l.integer > r.integer, tc.bool_());
        case TokenKind::greater_eq:
            return Value::of_bool(sgn ? l.as_signed() >= r.as_signed() : l.integer >= r.integer, tc.bool_());
        case TokenKind::amp: return integer(l.integer & r.integer, t);
        case TokenKind::pipe: return integer(l.integer | r.integer, t);
        case TokenKind::caret: return integer(l.integer ^ r.integer, t);
        case TokenKind::plus_percent: return integer(l.integer + r.integer, t);
        case TokenKind::minus_percent: return integer(l.integer - r.integer, t);
        case TokenKind::star_percent: return integer(l.integer * r.integer, t);
        case TokenKind::shl:
        case TokenKind::shr: {
            u128 n = r.integer;
            if ((r.type && r.type->is_signed && r.as_signed() < 0) || n >= static_cast<u128>(bits)) {
                failed(rhs_span, std::format("shift by {} is out of range for {}", to_string(r), types::to_string(t)),
                       "shift count too large");
            }
            int c = static_cast<int>(n);
            if (op == TokenKind::shl) return integer(l.integer << c, t);
            return sgn ? integer(static_cast<u128>(l.as_signed() >> c), t) : integer(l.integer >> c, t);
        }
        case TokenKind::plus:
        case TokenKind::minus:
        case TokenKind::star: {
            const char* what = op == TokenKind::plus ? "+" : op == TokenKind::minus ? "-" : "*";
            if (sgn) {
                i128 a = l.as_signed(), c = r.as_signed(), x = 0;
                bool o = op == TokenKind::plus    ? __builtin_add_overflow(a, c, &x)
                       : op == TokenKind::minus ? __builtin_sub_overflow(a, c, &x)
                                                : __builtin_mul_overflow(a, c, &x);
                if (o || x < signed_min(bits) || x > signed_max(bits)) return overflow(what);
                return integer(static_cast<u128>(x), t);
            }
            u128 a = l.integer, c = r.integer, x = 0;
            bool o = op == TokenKind::plus    ? __builtin_add_overflow(a, c, &x)
                   : op == TokenKind::minus ? __builtin_sub_overflow(a, c, &x)
                                            : __builtin_mul_overflow(a, c, &x);
            if (o || x > unsigned_max(bits)) return overflow(what);
            return integer(x, t);
        }
        case TokenKind::slash:
        case TokenKind::percent: {
            if (r.integer == 0) failed(rhs_span, "division by zero at compile time", "this is zero");
            if (sgn) {
                i128 a = l.as_signed(), c = r.as_signed();
                if (a == signed_min(bits) && c == -1) return overflow(op == TokenKind::slash ? "/" : "%");
                return integer(static_cast<u128>(op == TokenKind::slash ? a / c : a % c), t);
            }
            return integer(op == TokenKind::slash ? l.integer / r.integer : l.integer % r.integer, t);
        }
        default: unknown();
        }
    }

    Value cast(const CastExpr& c) {
        const Type* to = type_of(c);
        Value       v  = eval(*c.operand);
        if (c.variant == "reinterpret" || c.variant == "dynamic") not_constant(c.span, std::format("'cast::{}' cannot run at compile time", c.variant));
        if (c.variant == "checked") {
            const Type* inner = to->symbol->template_args.at(0);
            std::optional<Value> converted = convert(v, inner, /*exact=*/true);
            Value out;
            out.kind   = Value::Kind::enumeration;
            out.type   = to;
            out.symbol = to->symbol->members.at(converted ? "some" : "none").front();
            if (converted) out.elements.push_back(*converted);
            return out;
        }
        if (c.variant == "saturate" && to->is_integer()) {
            if (v.kind == Value::Kind::floating) {
                double f = v.floating;
                if (std::isnan(f)) return integer(0, to);
                double lo = to->is_signed ? std::ldexp(-1.0, to->bits - 1) : 0.0;
                double hi = std::ldexp(1.0, to->is_signed ? to->bits - 1 : to->bits);
                if (f <= lo) return integer(to->is_signed ? static_cast<u128>(signed_min(to->bits)) : 0, to);
                if (f >= hi) return integer(to->is_signed ? static_cast<u128>(signed_max(to->bits)) : unsigned_max(to->bits), to);
            } else if (v.kind == Value::Kind::integer) {
                i128 s = v.as_signed();
                bool neg = v.type->is_signed && s < 0;
                if (to->is_signed) {
                    if (neg && s < signed_min(to->bits)) return integer(static_cast<u128>(signed_min(to->bits)), to);
                    if (!neg && v.integer > static_cast<u128>(signed_max(to->bits))) return integer(static_cast<u128>(signed_max(to->bits)), to);
                } else {
                    if (neg) return integer(0, to);
                    if (v.integer > unsigned_max(to->bits)) return integer(unsigned_max(to->bits), to);
                }
            }
        }
        std::optional<Value> r = convert(v, to, /*exact=*/false);
        if (!r) unknown();
        return *r;
    }

    /// Numeric conversion; with `exact`, nullopt when the value changes.
    std::optional<Value> convert(const Value& v, const Type* to, bool exact) {
        if (v.kind == Value::Kind::integer && to->is_integer()) {
            bool neg = v.type->is_signed && v.as_signed() < 0;
            Value r  = integer(neg ? static_cast<u128>(v.as_signed()) : v.integer, to);
            if (exact) {
                bool r_neg = to->is_signed && r.as_signed() < 0;
                if (neg != r_neg || (neg ? r.as_signed() != v.as_signed() : r.integer != v.integer)) return std::nullopt;
            }
            return r;
        }
        if (v.kind == Value::Kind::integer && to->is_float()) {
            Value r;
            r.kind     = Value::Kind::floating;
            r.type     = to;
            r.floating = v.type->is_signed ? static_cast<double>(v.as_signed()) : static_cast<double>(v.integer);
            if (to->bits == 32) r.floating = round_f32(r.floating);
            if (exact) {
                std::optional<Value> back = convert(r, v.type, true);
                if (!back || !equal(*back, v)) return std::nullopt;
            }
            return r;
        }
        if (v.kind == Value::Kind::floating && to->is_integer()) {
            double f = std::trunc(v.floating);
            if (std::isnan(v.floating)) return std::nullopt;
            double lo = to->is_signed ? std::ldexp(-1.0, to->bits - 1) : 0.0;
            double hi = std::ldexp(1.0, to->is_signed ? to->bits - 1 : to->bits);
            if (f < lo || f >= hi || (exact && f != v.floating)) return std::nullopt;
            u128 bits = f < 0 ? static_cast<u128>(static_cast<i128>(f)) : static_cast<u128>(f);
            return integer(bits, to);
        }
        if (v.kind == Value::Kind::floating && to->is_float()) {
            Value r = v;
            r.type  = to;
            if (to->bits == 32) r.floating = round_f32(v.floating);
            if (exact && !std::isnan(v.floating) && r.floating != v.floating) return std::nullopt;
            return r;
        }
        if (v.kind == Value::Kind::character && to->is_integer()) return integer(v.character, to);
        if (v.kind == Value::Kind::integer && to->is(Kind::char_)) {
            Value r;
            r.kind      = Value::Kind::character;
            r.type      = to;
            r.character = static_cast<char32_t>(v.integer);
            return r;
        }
        if (v.kind == Value::Kind::enumeration && to->is_integer() && v.symbol && v.symbol->alternative) {
            return integer(tag_of(v.symbol), to);
        }
        return std::nullopt;
    }

    u128 tag_of(const Symbol* alt) {
        u128 next = 0;
        for (const Symbol* a : alt->parent->member_order) {
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
                }
            }
            if (a == alt || a->name == alt->name) return next;
            ++next;
        }
        return next;
    }

    std::vector<const Symbol*> fields_of(const Type* t) {
        std::vector<const Symbol*> out;
        for (const Symbol* f : t->symbol->member_order) {
            if (f->kind == SymbolKind::field && !f->is_static) out.push_back(f);
        }
        return out;
    }

    Value default_of(const Type* t, Span span) {
        if (t->is_integer()) return integer(0, t);
        if (t->is_float()) {
            Value v;
            v.kind = Value::Kind::floating;
            v.type = t;
            return v;
        }
        if (t->is(Kind::bool_)) return Value::of_bool(false, t);
        if (t->is(Kind::char_)) {
            Value v;
            v.kind = Value::Kind::character;
            v.type = t;
            return v;
        }
        if (t->is(Kind::static_str)) return string_value("");
        if (types::is_std_instance(t, "option")) {
            Value v;
            v.kind   = Value::Kind::enumeration;
            v.type   = t;
            v.symbol = t->symbol->members.at("none").front();
            return v;
        }
        if (t->is(Kind::user) && (t->symbol->kind == SymbolKind::struct_ || t->symbol->kind == SymbolKind::class_)) {
            return build_aggregate(t, {}, span);
        }
        not_constant(span, std::format("the default of {} is not known at compile time", types::to_string(t)));
    }

    Value build_aggregate(const Type* t, const std::map<std::string, Value>& given, Span span) {
        Value v;
        v.kind = Value::Kind::aggregate;
        v.type = t;
        for (const Symbol* f : fields_of(t)) {
            if (auto it = given.find(f->name); it != given.end()) {
                v.elements.push_back(it->second);
                continue;
            }
            const auto& d = f->decl->as<VariableDecl>();
            if (d.init) {
                if (!d.init->type) not_constant(span, std::format("the default of '{}::{}' is not checked yet", t->symbol->name, f->name),
                                                "declare the type before the compile-time code that uses it");
                v.elements.push_back(eval(*d.init));
            } else {
                v.elements.push_back(default_of(tc.lower(*d.type, sema::substitution_of(f)), span));
            }
        }
        return v;
    }

    Value struct_literal(const StructLiteralExpr& sl) {
        const Type* t = type_of(sl);
        if (!t->is(Kind::user)) unknown();
        std::map<std::string, Value> given;
        for (const FieldInit& f : sl.fields) given[f.name] = eval(*f.value);
        return build_aggregate(t, given, sl.span);
    }

    // --- members and calls -------------------------------------------------------------

    std::size_t field_index(const Type* t, const std::string& name, Span span) {
        std::vector<const Symbol*> fs = fields_of(t);
        for (std::size_t i = 0; i < fs.size(); ++i) {
            if (fs[i]->name == name) return i;
        }
        not_constant(span, std::format("'{}' is not a field that is known at compile time", name));
    }

    Value member(const MemberExpr& me) {
        Value              o    = eval(*me.object);
        const std::string& name = me.name.segments.back().name;
        if (o.kind == Value::Kind::field) {
            const Symbol* f = o.symbol;
            if (name == "name") return string_value(f->name);
            if (name == "type") return Value::of_type(tc.lower(*f->decl->as<VariableDecl>().type, sema::substitution_of(f)), tc.type_value());
            std::vector<const Symbol*> all = fields_of(o.type_value);
            std::size_t index = 0;
            while (index < all.size() && all[index] != f) ++index;
            if (name == "index") return integer(index, usize());
            if (name == "offset") return integer(tc.field_offsets(o.type_value).at(index), usize());
        }
        if (o.kind == Value::Kind::alternative) {
            const Symbol* a = o.symbol;
            if (name == "name") return string_value(a->name);
            if (name == "has_payload") return Value::of_bool(a->alternative->payload != nullptr, tc.bool_());
            if (name == "payload") {
                return Value::of_type(a->alternative->payload ? tc.lower(*a->alternative->payload, sema::substitution_of(a)) : tc.void_(),
                                      tc.type_value());
            }
            if (name == "index") {
                std::size_t index = 0;
                for (const Symbol* x : a->parent->member_order) {
                    if (x == a) break;
                    if (x->alternative) ++index;
                }
                return integer(index, usize());
            }
        }
        if (o.kind == Value::Kind::aggregate) return o.elements.at(field_index(o.type, name, me.span));
        not_constant(me.span, std::format("'.{}' is not known at compile time", name));
    }

    Value call(const CallExpr& c) {
        if (std::string b = builtin_name(c); !b.empty()) return builtin(c, b);
        const Expr& callee = *c.callee;
        if (callee.kind == ExprKind::path) {
            const auto& pe = callee.as<PathExpr>();
            const Symbol* s = pe.symbol;
            if (!s) not_constant(c.span, std::format("'{}' cannot run at compile time", ast::to_string(pe.path)),
                                 "I/O and other library functions only exist at run time");
            if (s->kind == SymbolKind::alternative) {
                Value v;
                v.kind   = Value::Kind::enumeration;
                v.type   = type_of(c);
                v.symbol = s;
                if (c.args.size() == 1) v.elements.push_back(eval(*c.args[0]));
                return v;
            }
            if (s->kind == SymbolKind::function || s->kind == SymbolKind::method) return run(s, std::nullopt, c);
        }
        if (callee.kind == ExprKind::member) {
            const auto&        me   = callee.as<MemberExpr>();
            const std::string& name = me.name.segments.back().name;
            if (me.symbol && me.symbol->kind == SymbolKind::method) return run(me.symbol, eval(*me.object), c);
            Value o = eval(*me.object);
            return method(o, name, c);
        }
        not_constant(c.span, "this call cannot run at compile time");
    }

    Value method(const Value& o, const std::string& name, const CallExpr& c) {
        if ((o.kind == Value::Kind::list || o.kind == Value::Kind::string) && c.args.empty()) {
            std::size_t n = o.kind == Value::Kind::list ? o.elements.size() : o.string.size();
            if (name == "len") return integer(n, usize());
            if (name == "is_empty") return Value::of_bool(n == 0, tc.bool_());
        }
        if (o.kind == Value::Kind::field && name == "get" && c.args.size() == 1) {
            Value v = eval(*c.args[0]);
            if (v.kind != Value::Kind::aggregate) unknown();
            return v.elements.at(field_index(v.type, o.symbol->name, c.span));
        }
        if (o.kind == Value::Kind::alternative) {
            if (name == "is" && c.args.size() == 1) {
                Value v = eval(*c.args[0]);
                return Value::of_bool(v.kind == Value::Kind::enumeration && v.symbol && v.symbol->name == o.symbol->name, tc.bool_());
            }
            if (name == "make") {
                Value v;
                v.kind   = Value::Kind::enumeration;
                v.type   = o.type_value;
                v.symbol = o.symbol;
                if (c.args.size() == 1) v.elements.push_back(eval(*c.args[0]));
                return v;
            }
        }
        if (o.kind == Value::Kind::enumeration && (types::is_std_instance(o.type, "option") || types::is_std_instance(o.type, "result"))) {
            bool good = o.symbol->name == "some" || o.symbol->name == "ok";
            if (name == "value_or" && c.args.size() == 1) return good ? o.elements.at(0) : eval(*c.args[0]);
            if (name == "expect" && c.args.size() == 1) {
                if (good) return o.elements.at(0);
                Value m = eval(*c.args[0]);
                failed(c.span, std::format("compile-time evaluation panicked: {}", m.string), "'expect' failed");
            }
        }
        not_constant(c.span, std::format("the method '{}' cannot run at compile time", name));
    }

    Value builtin(const CallExpr& c, const std::string& name) {
        const auto& seg = c.callee->as<PathExpr>().path.segments.back();
        auto arg = [&](std::size_t i) -> const Type* {
            if (i >= seg.template_args.size() || !seg.template_args[i].type) unknown();
            return lower(*seg.template_args[i].type);
        };
        const Type* t = arg(0);
        if (name == "is_integer") return Value::of_bool(t->is_integer(), tc.bool_());
        if (name == "is_float") return Value::of_bool(t->is_float(), tc.bool_());
        if (name == "is_same") return Value::of_bool(t == arg(1), tc.bool_());
        if (name == "type_name") return string_value(types::to_string(t));
        if (name == "size_of" || name == "align_of") {
            types::Layout l = tc.layout(t);
            if (l.size == 0 && !t->is(Kind::void_)) not_constant(c.span, std::format("the size of {} is not known", types::to_string(t)));
            return integer(name == "size_of" ? l.size : l.align, usize());
        }
        bool is_user = t->is(Kind::user);
        if (name == "meta::is_struct") return Value::of_bool(is_user && t->symbol->kind == SymbolKind::struct_, tc.bool_());
        if (name == "meta::is_class") return Value::of_bool(is_user && t->symbol->kind == SymbolKind::class_, tc.bool_());
        if (name == "meta::is_enum") return Value::of_bool(is_user && t->symbol->kind == SymbolKind::enum_, tc.bool_());
        Value list;
        list.kind = Value::Kind::list;
        list.type = c.type;
        if (name == "meta::fields") {
            if (!is_user || (t->symbol->kind != SymbolKind::struct_ && t->symbol->kind != SymbolKind::class_)) {
                failed(c.span, std::format("std::meta::fields needs a struct or class, not {}", types::to_string(t)), "not a struct or class");
            }
            for (const Symbol* f : hooks.visible_fields(t)) {
                Value v;
                v.kind       = Value::Kind::field;
                v.type       = tc.meta_field();
                v.symbol     = f;
                v.type_value = t;
                list.elements.push_back(v);
            }
            return list;
        }
        if (name == "meta::alternatives") {
            if (!is_user || t->symbol->kind != SymbolKind::enum_) {
                failed(c.span, std::format("std::meta::alternatives needs an enum, not {}", types::to_string(t)), "not an enum");
            }
            for (const Symbol* a : t->symbol->member_order) {
                if (!a->alternative) continue;
                Value v;
                v.kind       = Value::Kind::alternative;
                v.type       = tc.meta_alternative();
                v.symbol     = a;
                v.type_value = t;
                list.elements.push_back(v);
            }
            return list;
        }
        unknown();
    }

    /// Runs a compile_time function.
    Value run(const Symbol* s, std::optional<Value> self, const CallExpr& c) {
        const FunctionDecl* f = body_decl(s);
        if (!f || !f->body) not_constant(c.span, std::format("'{}' has no body to run at compile time", s->name));
        if (!f->is_compile_time) {
            not_constant(c.span, std::format("'{}' is not a compile_time function", s->name),
                         std::format("mark it 'compile_time' (or 'compile_time run_time') to call it at compile time"));
        }
        if (!hooks.check_function(s)) unknown();
        if (frames.size() >= max_depth) {
            failed(c.span, std::format("compile-time calls are nested more than {} deep", max_depth), "too deep");
        }
        std::vector<Value> args;
        for (const ExprPtr& a : c.args) args.push_back(eval(*a));
        Frame frame;
        frame.function  = s;
        frame.call_site = c.span;
        frame.self      = std::move(self);
        // Type-valued parameters are read through their symbol when a type is lowered.
        std::vector<std::pair<const Symbol*, std::shared_ptr<const Value>>> saved;
        for (std::size_t i = 0; i < f->params.size() && i < args.size(); ++i) {
            const Symbol* p = f->params[i].symbol;
            if (!p) continue;
            frame.locals[p] = args[i];
            if (p->is_type_value) {
                saved.emplace_back(p, p->ct_value);
                const_cast<Symbol*>(p)->ct_value = std::make_shared<const Value>(args[i]);
            }
        }
        frames.push_back(std::move(frame));
        auto restore = [&] {
            frames.pop_back();
            for (auto& [p, v] : saved) const_cast<Symbol*>(p)->ct_value = v;
        };
        Value result;
        try {
            frames.back().result = f->result ? lower(*f->result) : tc.void_();
            Flow flow = run_block(*f->body);
            if (flow == Flow::normal && f->body->tail) result = returned;
            if (flow == Flow::return_) result = returned;
        } catch (...) {
            restore();
            throw;
        }
        restore();
        return result;
    }

    // --- blocks and control flow -------------------------------------------------------

    /// Runs a block as a statement. A tail value is left in `returned` only for function bodies.
    Flow run_block(const BlockExpr& b) {
        try {
            for (const StmtPtr& s : b.statements) statement(*s);
            if (b.tail) returned = eval(*b.tail);
        } catch (Flow f) {
            return f;
        }
        return Flow::normal;
    }

    Value block(const BlockExpr& b) {
        for (const StmtPtr& s : b.statements) statement(*s);
        return b.tail ? eval(*b.tail) : Value{};
    }

    void bind(const Symbol* s, Value v) {
        if (frames.empty()) {
            frames.emplace_back();   // top-level code: a frame without a function
        }
        frames.back().locals[s] = std::move(v);
    }

    void statement(const Stmt& s) {
        step(s.span);
        switch (s.kind) {
        case StmtKind::variable: {
            const auto& v = s.as<VariableStmt>();
            if (!v.symbol) unknown();
            if (v.storage == Storage::compile_time && v.symbol->ct_value) {
                bind(v.symbol, *v.symbol->ct_value);
                return;
            }
            bind(v.symbol, v.init ? eval(*v.init) : default_of(lower(*v.type), v.span));
            return;
        }
        case StmtKind::assign: {
            const auto& a      = s.as<AssignStmt>();
            Value       value  = eval(*a.value);
            Value&      target = place(*a.target);
            if (a.op == TokenKind::eq) {
                target = std::move(value);
            } else {
                // compound_base is in check.cpp; map the few compound operators here.
                TokenKind op = TokenKind::eq;
                switch (a.op) {
                case TokenKind::plus_eq: op = TokenKind::plus; break;
                case TokenKind::minus_eq: op = TokenKind::minus; break;
                case TokenKind::star_eq: op = TokenKind::star; break;
                case TokenKind::slash_eq: op = TokenKind::slash; break;
                case TokenKind::percent_eq: op = TokenKind::percent; break;
                case TokenKind::amp_eq: op = TokenKind::amp; break;
                case TokenKind::pipe_eq: op = TokenKind::pipe; break;
                case TokenKind::caret_eq: op = TokenKind::caret; break;
                case TokenKind::shl_eq: op = TokenKind::shl; break;
                case TokenKind::shr_eq: op = TokenKind::shr; break;
                case TokenKind::plus_percent_eq: op = TokenKind::plus_percent; break;
                case TokenKind::minus_percent_eq: op = TokenKind::minus_percent; break;
                case TokenKind::star_percent_eq: op = TokenKind::star_percent; break;
                default: unknown();
                }
                target = apply(op, target, value, a.span, a.value->span);
            }
            return;
        }
        case StmtKind::discard: eval(*s.as<DiscardStmt>().value); return;
        case StmtKind::expression: eval(*s.as<ExprStmt>().expr); return;
        }
    }

    /// The storage a compile-time assignment target denotes.
    Value& place(const Expr& e) {
        if (e.kind == ExprKind::path) {
            const Symbol* s = e.as<PathExpr>().symbol;
            if (!frames.empty()) {
                if (auto it = frames.back().locals.find(s); it != frames.back().locals.end()) return it->second;
            }
            not_constant(e.span, "only local variables of compile-time code can be assigned at compile time");
        }
        if (e.kind == ExprKind::member) {
            const auto& me = e.as<MemberExpr>();
            Value&      o  = place(*me.object);
            if (o.kind != Value::Kind::aggregate) unknown();
            return o.elements.at(field_index(o.type, me.name.segments.back().name, me.span));
        }
        if (e.kind == ExprKind::index) {
            const auto& ix = e.as<IndexExpr>();
            Value&      o  = place(*ix.object);
            Value       i  = eval(*ix.index);
            if (o.kind != Value::Kind::list || i.integer >= o.elements.size()) {
                failed(ix.index->span, "index out of bounds at compile time", "out of bounds");
            }
            return o.elements[static_cast<std::size_t>(i.integer)];
        }
        not_constant(e.span, "this cannot be assigned at compile time");
    }

    Value if_value(const IfExpr& i) {
        bool take_then;
        if (i.is_compile_time && i.taken >= 0) {
            take_then = i.taken == 0;
            if (i.taken == 2) return Value{};
        } else {
            take_then = truth(eval(*i.condition));
        }
        if (take_then) return eval(*i.then_block);
        return i.else_branch ? eval(*i.else_branch) : Value{};
    }

    Value for_loop(const ForExpr& f) {
        if (f.is_compile_time) {
            for (const BlockPtr& copy : f.copies) {
                Flow flow = run_block(*copy);
                if (flow == Flow::break_) break;
                if (flow == Flow::return_) throw flow;
            }
            return Value{};
        }
        std::optional<std::vector<Value>> items = items_of(*f.iterable);
        for (Value& v : *items) {
            bind(f.symbol, v);
            Flow flow = run_block(*f.body);
            if (flow == Flow::break_) break;
            if (flow == Flow::return_) throw flow;
        }
        return Value{};
    }

    std::optional<std::vector<Value>> items_of(const Expr& iterable) {
        std::vector<Value> out;
        if (iterable.kind == ExprKind::range) {
            const auto& r  = iterable.as<RangeExpr>();
            Value       lo = eval(*r.lo);
            Value       hi = eval(*r.hi);
            if (lo.kind != Value::Kind::integer) unknown();
            bool sgn = lo.type->is_signed;
            for (Value v = lo;; v = integer(v.integer + 1, v.type)) {
                bool below = sgn ? v.as_signed() < hi.as_signed() : v.integer < hi.integer;
                bool equal_hi = v.integer == hi.integer;
                if (!(below || (r.inclusive && equal_hi))) break;
                out.push_back(v);
                step(iterable.span);
                if (equal_hi) break;
            }
            return out;
        }
        Value v = eval(iterable);
        if (v.kind != Value::Kind::list) not_constant(iterable.span, "a compile-time loop needs a list or a range");
        return v.elements;
    }

    Value match_value(const MatchExpr& m) {
        Value s = eval(*m.scrutinee);
        for (const MatchArm& arm : m.arms) {
            if (!matches(*arm.pattern, s)) continue;
            if (arm.guard && !truth(eval(*arm.guard))) continue;
            return eval(*arm.body);
        }
        failed(m.span, std::format("no match arm matches {} at compile time", to_string(s)), "no arm matches");
    }

    bool matches(const Pattern& p, const Value& v) {
        switch (p.kind) {
        case PatternKind::wildcard: return true;
        case PatternKind::binding:
            if (p.as<BindingPattern>().symbol) bind(p.as<BindingPattern>().symbol, v);
            return true;
        case PatternKind::literal: return equal(eval(*p.as<LiteralPattern>().literal), v);
        case PatternKind::range: {
            const auto& r = p.as<RangePattern>();
            bool        ok = true;
            if (r.lo) ok = ok && truth(apply(TokenKind::greater_eq, v, eval(*r.lo), p.span, p.span));
            if (r.hi) ok = ok && truth(apply(r.inclusive ? TokenKind::less_eq : TokenKind::less, v, eval(*r.hi), p.span, p.span));
            return ok;
        }
        case PatternKind::enum_alternative: {
            const auto& ep = p.as<EnumPattern>();
            if (v.kind != Value::Kind::enumeration || !ep.symbol || !v.symbol || ep.symbol->name != v.symbol->name) return false;
            return !ep.payload || v.elements.empty() || matches(*ep.payload, v.elements[0]);
        }
        case PatternKind::struct_: {
            const auto& sp = p.as<StructPattern>();
            if (v.kind != Value::Kind::aggregate) unknown();
            for (const FieldPattern& fp : sp.fields) {
                if (fp.pattern && !matches(*fp.pattern, v.elements.at(field_index(v.type, fp.name, fp.span)))) return false;
            }
            return true;
        }
        case PatternKind::error: unknown();
        }
        return false;
    }
};

// --- public interface ----------------------------------------------------------------------

Evaluator::Evaluator(sema::Program& program, DiagnosticEngine& diags, Hooks hooks)
    : impl_(std::make_unique<Impl>(program, diags, std::move(hooks))) {}

Evaluator::~Evaluator() = default;

std::shared_ptr<const Value> Evaluator::evaluate(const Expr& e, std::string_view why) {
    unknown_ = false;
    std::vector<Impl::Frame> saved_frames = std::move(impl_->frames);
    impl_->frames.clear();
    std::string saved_why = std::move(impl_->why);
    impl_->why            = std::string(why);
    impl_->steps          = 0;
    std::shared_ptr<const Value> result;
    try {
        result = std::make_shared<const Value>(impl_->eval(e));
    } catch (Stop s) {
        unknown_ = s.unknown;
    } catch (Flow) {
        unknown_ = true;   // break/continue/return outside the code being evaluated
    }
    impl_->frames = std::move(saved_frames);
    impl_->why    = std::move(saved_why);
    return result;
}

std::optional<std::vector<Value>> Evaluator::elements(const Expr& iterable) {
    unknown_ = false;
    std::vector<Impl::Frame> saved_frames = std::move(impl_->frames);
    impl_->frames.clear();
    impl_->steps = 0;
    std::optional<std::vector<Value>> result;
    try {
        result = impl_->items_of(iterable);
    } catch (Stop s) {
        unknown_ = s.unknown;
    } catch (Flow) {
        unknown_ = true;
    }
    impl_->frames = std::move(saved_frames);
    return result;
}

} // namespace rift::ct

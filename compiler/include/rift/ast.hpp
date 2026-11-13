#pragma once
// Abstract syntax tree produced by the parser.
//
// The tree is purely syntactic: names are not resolved and nothing is type-checked. Every node
// owns its children through unique_ptr and carries the Span it was parsed from.
//
// Node families (each with a `kind` tag; use `as<T>()` to downcast):
//   TypeExpr  written types:  u32, std::darray<employee>, T&, R(Args)
//   Expr      expressions, including block-like ones (if, match, { }) and return/break/continue
//   Pattern   match patterns
//   Stmt      statements inside blocks
//   Decl      top-level and member declarations

#include "rift/source.hpp"
#include "rift/token.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace rift::sema {
struct Symbol;
}
namespace rift::types {
struct Type;
}
namespace rift::ct {
struct Value;
}

namespace rift::ast {

/// Filled in by name resolution (M3); null until then or when unresolved.
using SymbolRef = sema::Symbol*;

struct TypeExpr;
struct Expr;
struct Pattern;
struct Stmt;
struct Decl;
struct BlockExpr;

using TypePtr    = std::unique_ptr<TypeExpr>;
using ExprPtr    = std::unique_ptr<Expr>;
using PatternPtr = std::unique_ptr<Pattern>;
using StmtPtr    = std::unique_ptr<Stmt>;
using DeclPtr    = std::unique_ptr<Decl>;
using BlockPtr   = std::unique_ptr<BlockExpr>;

/// Base for every node family: a kind tag plus the source span.
template <typename Kind>
struct Node {
    Kind kind;
    Span span;

    explicit Node(Kind k, Span s) : kind(k), span(s) {}
    virtual ~Node() = default;
    Node(const Node&)            = delete;
    Node& operator=(const Node&) = delete;

    template <typename T> T&       as() { return static_cast<T&>(*this); }
    template <typename T> const T& as() const { return static_cast<const T&>(*this); }
};

// --- node family bases (defined first so unique_ptr members can be destroyed) ------

enum class TypeKind { named, reference, function, auto_, deduce };

struct TypeExpr : Node<TypeKind> {
    using Node::Node;
};

enum class ExprKind {
    int_literal, float_literal, char_literal, string_literal, bool_literal, nullptr_literal,
    this_, default_, path, unary, binary, range, call, index, member, try_, cast, bit,
    struct_literal, lambda, if_, match, block, while_, for_, return_, break_, continue_, error,
};

struct Expr : Node<ExprKind> {
    using Node::Node;
    const types::Type* type = nullptr;   // filled in by the type checker (M4)
    /// The value, when the checker computed it at compile time and code generation should use it
    /// as a constant (compile_time constants, `f.name` in a `for compile_time` loop, ...).
    std::shared_ptr<const ct::Value> constant;
};

enum class PatternKind { wildcard, literal, range, binding, enum_alternative, struct_, error };

struct Pattern : Node<PatternKind> {
    using Node::Node;
};

enum class StmtKind { variable, assign, discard, expression };

struct Stmt : Node<StmtKind> {
    using Node::Node;
};

// --- paths and template arguments ---------------------------------------------------------

/// One argument in `<...>`: a type (`employee`, `mut T`) or a value (`16`, `N + 1`).
/// A bare name like `buffer_size` is parsed as a type; name resolution decides later.
struct TemplateArg {
    TypePtr type;
    ExprPtr value;
    bool    is_mut = false;   // `std::raw_ptr<mut T>`
    Span    span;
};

struct PathSegment {
    std::string              name;
    Span                     span;
    bool                     has_template_args = false;
    std::vector<TemplateArg> template_args;
};

/// `std::darray<employee>::make`, `u32`, `parse_error::bad_line`.
struct Path {
    std::vector<PathSegment> segments;
    Span                     span;
};

// --- types --------------------------------------------------------------------------------

struct NamedType : TypeExpr {
    Path path;
    SymbolRef symbol = nullptr;   // the type (struct/class/enum/interface/template parameter)
    NamedType(Span s, Path p) : TypeExpr(TypeKind::named, s), path(std::move(p)) {}
};

/// `T&` or `mut T&`.
struct ReferenceType : TypeExpr {
    TypePtr inner;
    bool    is_mut = false;
    ReferenceType(Span s, TypePtr i) : TypeExpr(TypeKind::reference, s), inner(std::move(i)) {}
};

/// `R(A, B)` inside template arguments: `std::function<u32(u32)>`.
struct FunctionType : TypeExpr {
    TypePtr              result;
    std::vector<TypePtr> params;
    FunctionType(Span s, TypePtr r) : TypeExpr(TypeKind::function, s), result(std::move(r)) {}
};

/// `auto` (variables) and `deduce` (template return types) carry no data.
struct PlaceholderType : TypeExpr {
    using TypeExpr::TypeExpr;
};

// --- expressions --------------------------------------------------------------------------

/// `{expr:spec}` in an f-string (spec 19). Everything is optional except the expression.
struct FormatSpec {
    char align     = 0;    // 0, '<', '>' or '^'
    bool plus      = false;
    int  width     = -1;
    int  precision = -1;
    char radix     = 0;    // 0, 'x', 'X', 'b' or 'o'
    Span span;             // the spec text after ':' (empty span when there is none)
    bool present   = false;
};

/// One piece of an f-string: literal text, or a hole with an expression.
struct FormatPart {
    std::string text;   // literal text (escapes decoded); empty for holes
    ExprPtr     expr;   // the hole's expression; null for literal text
    FormatSpec  spec;
    Span        span;
};

struct LiteralExpr : Expr {
    TokenKind   token_kind;   // which literal token produced it
    std::string text;         // source text
    std::string value;        // decoded string/char contents
    std::vector<FormatPart> format_parts;   // f-strings only: text and holes in order
    u128        int_value = 0;
    std::string suffix;
    bool        bool_value = false;
    LiteralExpr(ExprKind k, Span s, TokenKind tk) : Expr(k, s), token_kind(tk) {}
};

/// `{ stmts; tail }`. The tail is the last expression without `;` (the block's value).
struct BlockExpr : Expr {
    bool                 is_unsafe = false;
    std::vector<StmtPtr> statements;
    ExprPtr              tail;
    BlockExpr(Span s) : Expr(ExprKind::block, s) {}
};

/// `this`, `default`, `break`, `continue`, error placeholders.
struct SimpleExpr : Expr {
    using Expr::Expr;
};

struct PathExpr : Expr {
    Path path;
    SymbolRef symbol = nullptr;   // what the path names (local, function, global, alternative, ...)
    PathExpr(Span s, Path p) : Expr(ExprKind::path, s), path(std::move(p)) {}
};

enum class UnaryOp {
    negate,        // -x
    logical_not,   // !x
    bit_not,       // ~x
    deref,         // *p
    address_of,    // &x
    address_of_mut,// mut &x
    move,          // move x
    mut_marker,    // mut x   (call-site / match marker: f(mut v), match (mut v))
};

struct UnaryExpr : Expr {
    UnaryOp op;
    ExprPtr operand;
    UnaryExpr(Span s, UnaryOp o, ExprPtr e) : Expr(ExprKind::unary, s), op(o), operand(std::move(e)) {}
};

struct BinaryExpr : Expr {
    TokenKind op;   // the operator token: plus, amp_amp, eq_eq, ...
    ExprPtr   lhs;
    ExprPtr   rhs;
    BinaryExpr(Span s, TokenKind o, ExprPtr l, ExprPtr r)
        : Expr(ExprKind::binary, s), op(o), lhs(std::move(l)), rhs(std::move(r)) {}
};

/// `a..b` and `a..=b`.
struct RangeExpr : Expr {
    ExprPtr lo;
    ExprPtr hi;
    bool    inclusive = false;
    RangeExpr(Span s, ExprPtr l, ExprPtr h, bool inc)
        : Expr(ExprKind::range, s), lo(std::move(l)), hi(std::move(h)), inclusive(inc) {}
};

struct CallExpr : Expr {
    ExprPtr              callee;
    std::vector<ExprPtr> args;
    CallExpr(Span s, ExprPtr c) : Expr(ExprKind::call, s), callee(std::move(c)) {}
};

struct IndexExpr : Expr {
    ExprPtr object;
    ExprPtr index;
    IndexExpr(Span s, ExprPtr o, ExprPtr i) : Expr(ExprKind::index, s), object(std::move(o)), index(std::move(i)) {}
};

/// `obj.name`, `obj.name<T>`, `this.shape::describe`, `ptr->field`.
struct MemberExpr : Expr {
    ExprPtr object;
    Path    name;
    bool    arrow = false;
    SymbolRef symbol = nullptr;   // only resolved for `this.member`
    MemberExpr(Span s, ExprPtr o, Path n, bool a)
        : Expr(ExprKind::member, s), object(std::move(o)), name(std::move(n)), arrow(a) {}
};

struct TryExpr : Expr {
    ExprPtr operand;
    TryExpr(Span s, ExprPtr e) : Expr(ExprKind::try_, s), operand(std::move(e)) {}
};

/// `cast<T>(x)`, `cast::truncate<T>(x)`, ...; `variant` is empty for the plain lossless cast.
struct CastExpr : Expr {
    std::string variant;
    TypePtr     target;
    ExprPtr     operand;
    CastExpr(Span s) : Expr(ExprKind::cast, s) {}
};

/// `bit(n)` or `bit<T>(n)`.
struct BitExpr : Expr {
    TypePtr type;   // may be null
    ExprPtr operand;
    BitExpr(Span s) : Expr(ExprKind::bit, s) {}
};

struct FieldInit {
    std::string name;
    Span        span;
    ExprPtr     value;
};

/// `T{ .field = value, ... }`.
struct StructLiteralExpr : Expr {
    TypePtr                type;
    std::vector<FieldInit> fields;
    StructLiteralExpr(Span s, TypePtr t) : Expr(ExprKind::struct_literal, s), type(std::move(t)) {}
};

enum class CaptureKind { copy, move, ref, mut_ref, init };

struct Capture {
    CaptureKind kind;
    std::string name;
    Span        span;
    ExprPtr     init;   // CaptureKind::init only
    SymbolRef   symbol = nullptr;   // the local the capture introduces inside the lambda
    SymbolRef   captured = nullptr; // the enclosing function's local it captures (not for init-captures)
};

struct Param {
    bool        is_mut = false;   // `mut T x` (by-value mutable); `mut T&` sets ReferenceType::is_mut
    TypePtr     type;
    std::string name;
    Span        span;
    SymbolRef   symbol = nullptr;
};

struct LambdaExpr : Expr {
    std::vector<Capture> captures;
    std::vector<Param>   params;
    TypePtr              result;   // may be null (deduced)
    BlockPtr             body;
    LambdaExpr(Span s) : Expr(ExprKind::lambda, s) {}
};

struct IfExpr : Expr {
    bool     is_compile_time = false;
    int      taken = -1;              // `if compile_time`: 0 = then, 1 = else, 2 = neither (set by the checker)
    ExprPtr  condition;
    BlockPtr then_block;
    ExprPtr  else_branch;   // BlockExpr or IfExpr, may be null
    IfExpr(Span s) : Expr(ExprKind::if_, s) {}
};

struct MatchArm {
    PatternPtr pattern;
    ExprPtr    guard;   // may be null
    ExprPtr    body;
    Span       span;
};

struct MatchExpr : Expr {
    ExprPtr               scrutinee;
    std::vector<MatchArm> arms;
    MatchExpr(Span s) : Expr(ExprKind::match, s) {}
};

struct ReturnExpr : Expr {
    ExprPtr value;   // may be null
    ReturnExpr(Span s, ExprPtr v) : Expr(ExprKind::return_, s), value(std::move(v)) {}
};

// --- patterns -----------------------------------------------------------------------------

struct LiteralPattern : Pattern {
    ExprPtr literal;   // LiteralExpr, or unary negate of one
    LiteralPattern(Span s, ExprPtr l) : Pattern(PatternKind::literal, s), literal(std::move(l)) {}
};

struct RangePattern : Pattern {
    ExprPtr lo;
    ExprPtr hi;
    bool    inclusive = false;
    RangePattern(Span s) : Pattern(PatternKind::range, s) {}
};

struct BindingPattern : Pattern {
    std::string name;
    SymbolRef   symbol = nullptr;
    BindingPattern(Span s, std::string n) : Pattern(PatternKind::binding, s), name(std::move(n)) {}
};

/// `token::plus`, `token::number(n)`, `std::ok(e)`.
struct EnumPattern : Pattern {
    Path       path;
    PatternPtr payload;   // may be null
    SymbolRef  symbol = nullptr;   // the alternative
    EnumPattern(Span s, Path p) : Pattern(PatternKind::enum_alternative, s), path(std::move(p)) {}
};

struct FieldPattern {
    std::string name;
    Span        span;
    PatternPtr  pattern;
};

/// `employee{ .age = a, .. }`.
struct StructPattern : Pattern {
    Path                      path;
    std::vector<FieldPattern> fields;
    bool                      has_rest = false;
    StructPattern(Span s, Path p) : Pattern(PatternKind::struct_, s), path(std::move(p)) {}
};

struct SimplePattern : Pattern {   // wildcard, error
    using Pattern::Pattern;
};

// --- statements ---------------------------------------------------------------------------

enum class Storage { local, compile_time, static_ };

/// `[mut] T name = init;`, `compile_time T name = init;`
struct VariableStmt : Stmt {
    Storage     storage = Storage::local;
    bool        is_mut  = false;
    TypePtr     type;
    std::string name;
    Span        name_span;
    ExprPtr     init;
    SymbolRef   symbol = nullptr;
    VariableStmt(Span s) : Stmt(StmtKind::variable, s) {}
};

/// `target op value;` where op is `=`, `+=`, `<<=`, `+%=`, ...
struct AssignStmt : Stmt {
    TokenKind op;
    ExprPtr   target;
    ExprPtr   value;
    AssignStmt(Span s, TokenKind o, ExprPtr t, ExprPtr v)
        : Stmt(StmtKind::assign, s), op(o), target(std::move(t)), value(std::move(v)) {}
};

/// `_ = expr;`
struct DiscardStmt : Stmt {
    ExprPtr value;
    DiscardStmt(Span s, ExprPtr v) : Stmt(StmtKind::discard, s), value(std::move(v)) {}
};

struct ExprStmt : Stmt {
    ExprPtr expr;
    ExprStmt(Span s, ExprPtr e) : Stmt(StmtKind::expression, s), expr(std::move(e)) {}
};

// --- loops (expressions of type void, always used as statements) -------------------------

struct WhileExpr : Expr {
    ExprPtr  condition;
    BlockPtr body;
    WhileExpr(Span s) : Expr(ExprKind::while_, s) {}
};

/// `for ([mut] T name : iterable) { }`
struct ForExpr : Expr {
    bool        is_compile_time = false;   // `for compile_time`: the body is copied per element (spec 26)
    std::vector<BlockPtr>  copies;          // the checked copies of the body, in order (set by the checker)
    std::vector<SymbolRef> copy_symbols;    // each copy's loop variable
    bool        unrolled = false;           // the checker made `copies` (else the body was checked as is)
    bool        is_mut = false;
    TypePtr     type;
    std::string name;
    Span        name_span;
    ExprPtr     iterable;
    BlockPtr    body;
    SymbolRef   symbol = nullptr;   // the loop variable
    ForExpr(Span s) : Expr(ExprKind::for_, s) {}
};

// --- declarations -------------------------------------------------------------------------

enum class DeclKind { import, function, variable, field, struct_, class_, interface, enum_ };

enum class Access { none, public_, protected_, private_ };

struct TemplateParam {
    bool                 is_type = true;   // `type T` vs `usize N`
    std::string          name;
    Span                 span;
    std::vector<TypePtr> constraints;      // `type T : a & b`
    TypePtr              value_type;       // `usize N`
};

struct Decl : Node<DeclKind> {
    std::string                name;
    Span                       name_span;
    std::string                doc;          // from `///` comments
    bool                       is_private = false;   // file-scope `private`
    Access                     access     = Access::none;   // class member section
    bool                       is_template = false;
    std::vector<TemplateParam> template_params;
    /// The parameters' symbols from the latest resolution of this declaration (name resolution).
    mutable std::vector<SymbolRef> template_param_symbols;
    using Node::Node;
};

struct ImportDecl : Decl {
    Path path;
    ImportDecl(Span s) : Decl(DeclKind::import, s) {}
};

struct FunctionDecl : Decl {
    TypePtr            result;          // null for destructors
    std::vector<Param> params;
    bool               is_static       = false;
    bool               is_virtual      = false;
    bool               is_override     = false;
    bool               is_abstract     = false;
    bool               is_mut          = false;   // trailing `mut`
    bool               is_compile_time = false;
    bool               is_run_time     = false;
    bool               is_unsafe       = false;
    bool               is_extern       = false;
    std::string        abi;                       // `extern "C"`
    bool               is_destructor   = false;
    bool               is_operator     = false;   // name is the operator spelling, e.g. "=="
    bool               is_defaulted    = false;   // `= default;`
    BlockPtr           body;                      // null for declarations
    FunctionDecl(Span s) : Decl(DeclKind::function, s) {}
};

/// File-scope variable (`static` / `compile_time`) or a struct/class field.
struct VariableDecl : Decl {
    Storage storage = Storage::local;
    bool    is_mut  = false;
    TypePtr type;
    ExprPtr init;   // fields: default value, may be null
    VariableDecl(DeclKind k, Span s) : Decl(k, s) {}
};

/// struct, class and interface share one node.
struct TypeDecl : Decl {
    bool                 is_open = false;
    bool                 is_view = false;
    std::vector<TypePtr> bases;
    std::vector<DeclPtr> members;
    TypeDecl(DeclKind k, Span s) : Decl(k, s) {}
};

struct EnumAlternative {
    std::string name;
    Span        span;
    TypePtr     payload;   // may be null
    ExprPtr     value;     // explicit tag, may be null
};

struct EnumDecl : Decl {
    TypePtr                      tag_type;
    std::vector<EnumAlternative> alternatives;
    EnumDecl(Span s) : Decl(DeclKind::enum_, s) {}
};

/// One parsed source file.
struct File {
    FileId               file = 0;
    std::vector<DeclPtr> decls;
};

} // namespace rift::ast

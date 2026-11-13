#pragma once
// Compile-time evaluation (spec 15, 23, 26).
//
// The evaluator runs checked code inside the compiler: the initializers of `compile_time`
// constants, the conditions of `if compile_time`, the ranges of `for compile_time`, and every
// `compile_time` function they call. It works directly on the checked AST, so it sees exactly
// the types the checker computed.
//
// Values are plain data (integers, floats, bools, chars, strings, types, structs, enums, lists)
// plus the reflection handles of `std::meta` (a field or an alternative of a reflected type).
//
// Errors:
//   E4200  a value that is not known at compile time (a run-time variable, a non-compile_time
//          function, I/O)
//   E4201  the evaluation itself failed (overflow, division by zero, out-of-bounds index, a
//          panic, too many steps or too deep recursion)
// Code that depends on a template parameter that is not known yet (the generic check of a
// template) gives "unknown" without an error; the instance check evaluates it again.

#include "rift/diagnostics.hpp"
#include "rift/symbols.hpp"
#include "rift/types.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rift::ct {

__extension__ typedef __int128 i128;

struct Value {
    enum class Kind { void_, integer, floating, boolean, character, string, type, aggregate, enumeration, list, field, alternative };

    Kind               kind      = Kind::void_;
    const types::Type* type      = nullptr;   // the Rift type of the value
    u128               integer   = 0;         // integers: two's complement, masked to the type's width
    double             floating  = 0.0;
    bool               boolean   = false;
    char32_t           character = 0;
    std::string        string;                 // strings: the bytes
    const types::Type* type_value = nullptr;   // type: the type it denotes; field/alternative: the reflected type
    const sema::Symbol* symbol   = nullptr;   // enumeration: its alternative; field: the field; alternative: the alternative
    std::vector<Value> elements;               // aggregate: fields in declaration order; list; enumeration: 0 or 1 payload

    static Value of_integer(u128 bits, const types::Type* t);
    static Value of_bool(bool b, const types::Type* t);
    static Value of_type(const types::Type* denoted, const types::Type* type_type);

    /// The integer as a signed 128-bit number (sign-extended from the type's width when signed).
    [[nodiscard]] i128 as_signed() const;
};

/// Source-like text of a value, for messages: `42`, `"abc"`, `u32`, `point{ .x = 1.0 }`.
std::string to_string(const Value& v);

/// Equality of two values of the same type (spec 26: also for type values).
bool equal(const Value& a, const Value& b);

/// How the checker connects the evaluator to code it has not checked yet.
struct Hooks {
    /// Makes sure a function's body is checked (on demand: it may be declared later). False if
    /// it cannot be (for example because checking it is in progress: a cycle).
    std::function<bool(const sema::Symbol* function)> check_function;
    /// The value of a `compile_time` constant (global or local), evaluating it if needed. Null
    /// when it has no value (an error was reported, or it depends on a template parameter).
    std::function<std::shared_ptr<const Value>(const sema::Symbol* constant)> constant_value;
    /// Lowers a type written in the code being checked (template parameters substituted).
    std::function<const types::Type*(const ast::TypeExpr& t)> lower;
    /// Whether a compile-time value depends on a template parameter that is not known yet (the
    /// generic check of a template): then the evaluation gives "unknown" without an error.
    std::function<bool(const sema::Symbol* s)> depends_on_template;
    /// What a template parameter stands for in the code being checked (null when unknown).
    std::function<const types::Type*(const sema::Symbol* param)> substitute;
    /// The fields of a struct/class that are visible to code in the checker's current module
    /// (spec 26: reflection respects privacy).
    std::function<std::vector<const sema::Symbol*>(const types::Type* t)> visible_fields;
};

class Evaluator {
public:
    Evaluator(sema::Program& program, DiagnosticEngine& diags, Hooks hooks);
    ~Evaluator();

    /// Evaluates a checked expression. Returns null when it could not be evaluated: then either an
    /// error was reported, or `unknown()` is true (it depends on a template parameter).
    std::shared_ptr<const Value> evaluate(const ast::Expr& e, std::string_view why);
    [[nodiscard]] bool unknown() const { return unknown_; }

    /// The values a `for compile_time` iterates over: the elements of a list, or the integers of
    /// a range. Null as for `evaluate`.
    std::optional<std::vector<Value>> elements(const ast::Expr& iterable);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool                  unknown_ = false;
};

/// The `std::meta` function a call names (`fields`, `alternatives`, `is_struct`, ...) or the trait
/// (`is_integer`, `size_of`, ...), or empty: `std::meta::fields<T>()` -> "meta::fields".
std::string builtin_name(const ast::CallExpr& c);

} // namespace rift::ct

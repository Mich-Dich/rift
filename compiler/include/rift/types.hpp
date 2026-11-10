#pragma once
// Types (M4). Every Type is interned in a TypeContext, so two types are equal exactly when
// their pointers are equal.
//
// Besides the spec's types there are a few checker-internal kinds:
//   error         an expression whose type could not be determined; it never causes further
//                 errors (keeps one mistake from producing a cascade)
//   untyped_int   an unsuffixed integer literal (or arithmetic on only such literals) that has
//   untyped_float not received its type from context yet (spec 02)
//   external      a type from the standard library, which does not exist yet: `std::string`.
//                 Compared by spelling; conversions between external types are not checked.
//   type_param    `T` inside a template; checked only at instantiation, so treated like error

#include "rift/symbols.hpp"
#include "rift/token.hpp"

#include <deque>
#include <map>
#include <string>
#include <tuple>

namespace rift::types {

enum class Kind {
    error, untyped_int, untyped_float, untyped_default,   // untyped_default: `default` without context
    int_, float_, bool_, char_, void_, never, nullptr_, static_str,
    user,        // struct / class / enum / interface (non-template)
    reference,   // T& / mut T&
    pointer,     // std::raw_ptr<T> / std::raw_ptr<mut T>
    range,       // a..b
    external,    // std:: types
    type_param,
};

struct Type {
    Kind                kind      = Kind::error;
    int                 bits      = 0;       // int_ / float_
    bool                is_signed = false;   // int_
    bool                is_size   = false;   // isize / usize
    const sema::Symbol* symbol    = nullptr; // user / type_param
    const Type*         inner     = nullptr; // reference / pointer / range
    bool                is_mut    = false;   // reference / pointer
    std::string         name;                // spelling (int names, external spelling)

    [[nodiscard]] bool is(Kind k) const { return kind == k; }
    [[nodiscard]] bool is_integer() const { return kind == Kind::int_; }
    [[nodiscard]] bool is_float() const { return kind == Kind::float_; }
    [[nodiscard]] bool is_numeric() const { return kind == Kind::int_ || kind == Kind::float_; }
    [[nodiscard]] bool is_untyped() const {
        return kind == Kind::untyped_int || kind == Kind::untyped_float || kind == Kind::untyped_default;
    }
    /// Types the checker cannot reason about: anything goes, nothing is reported.
    [[nodiscard]] bool is_unknown() const { return kind == Kind::error || kind == Kind::type_param; }
    [[nodiscard]] bool is_user(sema::SymbolKind k) const;
    /// `std::result<...>` and friends: the external spelling up to the first '<'.
    [[nodiscard]] std::string_view external_base() const;
};

class TypeContext {
public:
    TypeContext();

    const Type* error() const { return error_; }
    const Type* untyped_int() const { return untyped_int_; }
    const Type* untyped_float() const { return untyped_float_; }
    const Type* untyped_default() const { return untyped_default_; }
    const Type* bool_() const { return bool_t_; }
    const Type* char_() const { return char_t_; }
    const Type* void_() const { return void_t_; }
    const Type* never() const { return never_; }
    const Type* nullptr_() const { return nullptr_t_; }
    const Type* static_str() const { return static_str_; }

    /// Built-in type by keyword spelling ("u32", "f64", "bool", ...), or null.
    const Type* builtin(std::string_view name) const;
    const Type* reference(const Type* inner, bool is_mut);
    const Type* pointer(const Type* inner, bool is_mut);
    const Type* range(const Type* element);
    const Type* user(const sema::Symbol* symbol);
    const Type* type_param(const sema::Symbol* symbol);
    const Type* external(const std::string& spelling);

private:
    const Type* make(Type t);

    std::deque<Type>                                            storage_;
    std::map<std::string, const Type*>                          builtins_;
    std::map<std::tuple<int, const Type*, bool>, const Type*>   composites_;   // (kind, inner, mut)
    std::map<const sema::Symbol*, const Type*>                  users_;
    std::map<const sema::Symbol*, const Type*>                  params_;
    std::map<std::string, const Type*>                          externals_;
    const Type* error_;
    const Type* untyped_int_;
    const Type* untyped_float_;
    const Type* untyped_default_;
    const Type* bool_t_;
    const Type* char_t_;
    const Type* void_t_;
    const Type* never_;
    const Type* nullptr_t_;
    const Type* static_str_;
};

/// Source-like spelling: `u32`, `mut employee&`, `std::raw_ptr<u8>`, `{integer}`.
std::string to_string(const Type* type);

/// Removes any reference: `T&` -> `T`.
const Type* strip_ref(const Type* type);

/// Whether an integer literal with this magnitude (negated if `negative`) fits in `type`.
bool literal_fits(u128 magnitude, bool negative, const Type* type);

/// Smallest and largest value of an integer type, as text (for messages).
std::string integer_range(const Type* type);

} // namespace rift::types

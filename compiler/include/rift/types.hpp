#pragma once
// Types (M4). Every Type is interned in a TypeContext, so two types are equal exactly when
// their pointers are equal.
//
// Besides the spec's types there are a few checker-internal kinds:
//   error         an expression whose type could not be determined; it never causes further
//                 errors (keeps one mistake from producing a cascade)
//   untyped_int   an unsuffixed integer literal (or arithmetic on only such literals) that has
//   untyped_float not received its type from context yet (spec 02)
//   external      a standard-library type that library/std does not declare yet: `std::string`.
//                 Compared by spelling; conversions between external types are not checked.
//                 (`std::option`, `std::result` and `std::exit_code` are real enums of module std.)
//   type_param    `T` inside a template; checked only at instantiation, so treated like error
//   type_value    `type`: the type of types used as compile-time values (spec 26)
//   meta          `std::meta::field` / `std::meta::alternative`: compile-time-only reflection data
//   pending       `std::none`, `std::ok(x)`: an alternative of a template enum whose template
//                 arguments are not all known yet. Like a literal it takes them from context
//                 (spec 08); `args` holds the known ones and null for the others.

#include "rift/symbols.hpp"
#include "rift/token.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <vector>

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
    pending,     // template enum with arguments still to come from context (symbol = template)
    type_value,  // `type` (spec 26)
    meta,        // std::meta::field, std::meta::alternative (name says which)
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
    std::vector<const Type*> args;           // pending: known template arguments, null = unknown

    [[nodiscard]] bool is(Kind k) const { return kind == k; }
    [[nodiscard]] bool is_integer() const { return kind == Kind::int_; }
    [[nodiscard]] bool is_float() const { return kind == Kind::float_; }
    [[nodiscard]] bool is_numeric() const { return kind == Kind::int_ || kind == Kind::float_; }
    [[nodiscard]] bool is_untyped() const {
        return kind == Kind::untyped_int || kind == Kind::untyped_float || kind == Kind::untyped_default ||
               kind == Kind::pending;
    }
    /// Types whose values exist only at compile time (spec 26).
    [[nodiscard]] bool is_compile_time_only() const { return kind == Kind::type_value || kind == Kind::meta; }
    /// Types the checker cannot reason about: anything goes, nothing is reported.
    [[nodiscard]] bool is_unknown() const { return kind == Kind::error || kind == Kind::type_param; }
    [[nodiscard]] bool is_user(sema::SymbolKind k) const;
    /// `std::result<...>` and friends: the external spelling up to the first '<'.
    [[nodiscard]] std::string_view external_base() const;
};

/// Size and alignment in bytes, following the backend's layout (the interop rules of 13).
struct Layout {
    std::uint64_t size  = 0;
    std::uint64_t align = 1;
};

/// The type a compile_time `type` variable or parameter holds (spec 26). Set by the checker.
using TypeValueResolver = std::function<const Type*(const sema::Symbol* symbol)>;

/// Makes the instance of a template type for concrete arguments (spec 14). Set by the checker.
using TypeInstantiator = std::function<const Type*(const sema::Symbol* templ, const std::vector<const Type*>& args)>;

class TypeContext {
public:
    TypeContext();

    /// Without an instantiator, template types lower to `error` (nothing is instantiated).
    void set_instantiator(TypeInstantiator instantiator) { instantiator_ = std::move(instantiator); }
    void set_type_value_resolver(TypeValueResolver resolver) { type_values_ = std::move(resolver); }

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
    const Type* type_value() const { return type_value_; }
    const Type* meta_field() const { return meta_field_; }
    const Type* meta_alternative() const { return meta_alternative_; }

    /// Size and alignment of a sized type (64-bit host). Null-safe; unknown types give {0, 1}.
    Layout layout(const Type* t);
    /// Byte offset of each non-static field of a struct/class type, in declaration order.
    std::vector<std::uint64_t> field_offsets(const Type* t);

    /// Built-in type by keyword spelling ("u32", "f64", "bool", ...), or null.
    const Type* builtin(std::string_view name) const;
    const Type* reference(const Type* inner, bool is_mut);
    const Type* pointer(const Type* inner, bool is_mut);
    const Type* range(const Type* element);
    const Type* user(const sema::Symbol* symbol);
    const Type* type_param(const sema::Symbol* symbol);
    const Type* external(const std::string& spelling);
    /// The instance of the template type `templ` for `args` (spec 14). Error when the arguments
    /// do not fit, are unknown, or no instantiator is set.
    const Type* instance(const sema::Symbol* templ, const std::vector<const Type*>& args);
    /// A value of the template enum `templ` whose null arguments are not known yet.
    const Type* pending(const sema::Symbol* templ, const std::vector<const Type*>& args);

    /// The type a written TypeExpr denotes (after name resolution). Template parameters found in
    /// `subst` are replaced; template user types are not instantiated yet (error).
    const Type* lower(const ast::TypeExpr& t, const std::map<const sema::Symbol*, const Type*>* subst = nullptr);
    /// Template arguments of an external type as written, e.g. `std::result<u32, e>` -> {u32, e}.
    const std::vector<const Type*>* external_args(const Type* t) const;
    void set_external_args(const Type* t, std::vector<const Type*> args) { external_args_[t] = std::move(args); }

private:
    const Type* make(Type t);

    TypeInstantiator                                            instantiator_;
    TypeValueResolver                                           type_values_;
    std::map<std::pair<const sema::Symbol*, std::vector<const Type*>>, const Type*> instances_;
    std::deque<Type>                                            storage_;
    std::map<std::string, const Type*>                          builtins_;
    std::map<std::tuple<int, const Type*, bool>, const Type*>   composites_;   // (kind, inner, mut)
    std::map<const sema::Symbol*, const Type*>                  users_;
    std::map<const sema::Symbol*, const Type*>                  params_;
    std::map<std::string, const Type*>                          externals_;
    std::map<const Type*, std::vector<const Type*>>             external_args_;
    std::map<std::pair<const sema::Symbol*, std::vector<const Type*>>, const Type*> pendings_;
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
    const Type* type_value_;
    const Type* meta_field_;
    const Type* meta_alternative_;
};

/// Source-like spelling: `u32`, `mut employee&`, `std::raw_ptr<u8>`, `{integer}`.
std::string to_string(const Type* type);

/// Whether `t` (references removed) is an instance of the std template `name` ("option",
/// "result"): `std::option<u32>`.
bool is_std_instance(const Type* t, std::string_view name);

/// The built-in methods of std::option and std::result (spec 11), provided by the compiler.
bool is_std_builtin_method(const Type* receiver, std::string_view method);

/// Removes any reference: `T&` -> `T`.
const Type* strip_ref(const Type* type);

/// Whether an integer literal with this magnitude (negated if `negative`) fits in `type`.
bool literal_fits(u128 magnitude, bool negative, const Type* type);

/// Methods of standard-library containers known to change their receiver (`push`, `clear`, ...).
/// Until the library is written in Rift, this table stands in for their trailing `mut`.
bool is_mutating_library_method(const Type* receiver, std::string_view method);

/// Smallest and largest value of an integer type, as text (for messages).
std::string integer_range(const Type* type);

} // namespace rift::types

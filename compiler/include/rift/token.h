
#pragma once

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string_view>
#include <vector>  

#include "rift/span.h"



// FORWARD DECLARATIONS ================================================================================================

namespace rift {

    // CONSTANTS =======================================================================================================

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    // Every token kind the lexer can produce. Primitive types are their own
    // kinds (rather than identifiers) so the parser never has to string-
    // compare — the design doc treats them as reserved.
    //
    // Naming convention: `kw_*` for keywords, `prim_*` for primitive types,
    // `*_eq` for `op=`, `kw_compile_time` mirrors the design doc; the
    // design doc is internally inconsistent on `run_time` vs `runtime`
    // (§13.2 vs §22); we standardize on `run_time` matching the example.
    enum class token_kind : std::uint8_t {
        // Special
        end_of_file,
        invalid,

        // Identifiers & literals
        identifier,
        integer_literal,
        float_literal,
        string_literal,
        char_literal,

        // Keywords — declarations
        kw_import,
        kw_struct,
        kw_class,
        kw_open,
        kw_enum,
        kw_interface,
        kw_impl,

        // Keywords — modifiers
        kw_virtual,
        kw_override,
        kw_mut,
        kw_private,
        kw_static,

        // Keywords — compile-time
        kw_compile_time,
        kw_run_time,
        kw_deduce,

        // Keywords — control flow
        kw_if,
        kw_else,
        kw_match,
        kw_for,
        kw_in,
        kw_return,

        // Keywords — literals
        kw_true,
        kw_false,
        kw_null,

        // Keywords — expressions / type-system
        kw_cast,
        kw_unsafe,
        kw_auto,
        kw_default,
        kw_bit,

        // Primitive types
        prim_u8, prim_u16, prim_u32, prim_u64, prim_u128,
        prim_i8, prim_i16, prim_i32, prim_i64, prim_i128,
        prim_f32, prim_f64,
        prim_bool, prim_void,

        // Punctuation / structural
        l_brace, r_brace,
        l_paren, r_paren,
        l_bracket, r_bracket,
        semicolon,
        comma,
        dot,
        dot_dot,
        dot_dot_eq,
        colon,
        double_colon,
        arrow,       // ->
        fat_arrow,   // =>
        underscore,  // _

        // Operators — arithmetic
        plus, minus, star, slash, percent,

        // Operators — comparison
        eq_eq, bang_eq, lt, lt_eq, gt, gt_eq,

        // Operators — logical
        amp_amp, pipe_pipe, bang,

        // Operators — bitwise
        amp, pipe, caret, tilde, lt_lt, gt_gt,

        // Operators — assignment
        eq,
        plus_eq, minus_eq, star_eq, slash_eq, percent_eq,
        amp_eq, pipe_eq, caret_eq, lt_lt_eq, gt_gt_eq,

        // Misc
        question,   // ?

        count
    };


    struct token {

        // For all tokens except string_literal/char_literal, this points directly into the source text (including suffixes:
        // `0u32`, `"hi"` — the whole lexeme, quotes included). For EOF it's empty. Consumers that need the *decoded* string
        // value call `decode_escapes` (future milestone) or rely on the lexer having already validated the syntax
        std::string_view                lexeme;
        token_kind                      kind = token_kind::invalid;
        source_span                     span;

        constexpr bool is_keyword() const noexcept { return kind >= token_kind::kw_import && kind <= token_kind::kw_bit; }

        constexpr bool is_primitive_type() const noexcept { return kind >= token_kind::prim_u8 && kind <= token_kind::prim_void; }

        constexpr bool is_literal() const noexcept {
            return kind == token_kind::integer_literal ||
                kind == token_kind::float_literal   ||
                kind == token_kind::string_literal  ||
                kind == token_kind::char_literal;
        }

    };

    // STATIC VARIABLES ================================================================================================

    // FUNCTION DECLARATION ============================================================================================

    // Stable human-readable name, used by `riftc lex` and diagnostics
    std::string_view token_kind_name(token_kind k) noexcept;

    inline std::ostream& operator<<(std::ostream& os, token_kind k) { return os << token_kind_name(k); }

    // TEMPLATE DECLARATION ============================================================================================

    // Because `token_kind` lives in `rift`, ADL will find this operator for `std::vector<token_kind>` (and
    // `std::vector<std::vector<token_kind>>`, etc.), even though the vector itself is in `std`. It will NOT be found for
    // `std::vector<int>` or `std::vector<std::string>` - those fall back to the "not streamable" path in the test harness
    template <typename T>
    requires requires(std::ostream& os, const T& v) { os << v; }
    std::ostream& operator<<(std::ostream& os, const std::vector<T>& v) {

        os << '[';
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (i) os << ", ";
            os << v[i];
        }
        return os << ']';
    }

    // CLASS DECLARATION ===============================================================================================

}


#pragma once

#include <cstdint>
#include <iosfwd>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "rift/source.h"
#include "rift/span.h"



// FORWARD DECLARATIONS ================================================================================================

namespace rift {

    // CONSTANTS =======================================================================================================

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    enum class severity : std::uint8_t {
        error,
        warning,
        note,
        hint,
    };


    // Stable error codes. Ranges:
    //   E0001–E0099  Lexer
    //   E0100–E0199  Parser       (M2)
    //   E0200–E0299  Sema         (M3)
    //   E0300–E0399  Codegen      (M4)
    enum class diagnostic_code : std::uint16_t {

        // Lexer -------------------------------------------------------------------------------------------------------
        unterminated_string = 1,
        unterminated_char,
        unterminated_block_comment,
        invalid_escape,
        invalid_integer_suffix,
        invalid_digit_for_base,
        missing_digits_after_prefix,
        unexpected_character,
        empty_char_literal,
        multi_char_char_literal,
        invalid_char_in_literal,
        invalid_unicode_codepoint,
        trailing_underscore_in_number,
    };


    struct diagnostic {

        severity                        level = severity::error;
        source_span                     span;
        diagnostic_code                 code = diagnostic_code::unexpected_character;
        std::string                     message;
        std::vector<std::string>        notes;
    };

    // STATIC VARIABLES ================================================================================================

    // FUNCTION DECLARATION ============================================================================================

    std::string_view severity_name(severity s);


    inline std::ostream& operator<<(std::ostream& os, severity s) { return os << severity_name(s); }


    inline std::ostream& operator<<(std::ostream& os, diagnostic_code c) { return os << "E" << static_cast<unsigned>(c); }


    // Renders the diagnostic to `os` in a clang/rust-style block:
    //
    //   error[E0001]: unterminated string literal
    //     --> examples/test.rfts:3:10
    //      |
    //    3 |     let s = "hello
    //      |             ^^^^^^
    //
    void print_diagnostic(std::ostream& os, const diagnostic& d, const source_manager& sm);

    // TEMPLATE DECLARATION ============================================================================================

    // CLASS DECLARATION ===============================================================================================

}

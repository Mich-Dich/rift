
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "rift/diagnostic_engine.h"
#include "rift/source.h"
#include "rift/token.h"



// FORWARD DECLARATIONS ================================================================================================

namespace rift {

    // CONSTANTS =======================================================================================================

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    // STATIC VARIABLES ================================================================================================

    // FUNCTION DECLARATION ============================================================================================

    // TEMPLATE DECLARATION ============================================================================================

    // CLASS DECLARATION ===============================================================================================

    // One-shot lexer. Construct with a source_manager (for path/line resolution), a file_id (from `source_manager::add`),
    // and a diagnostic_engine (for errors). Call `tokenize()` once; the lexer is stateful during the call but inert afterwards
    //
    // The returned vector always ends with an `end_of_file` token. Tokens carry `std::string_view` lexemes pointing
    // into the source file's content, so the source_manager must outlive the tokens
    class lexer {
    public:

        lexer(source_manager& sm, file_id file, diagnostic_engine& diag);

        lexer(const lexer&) = delete;
        lexer& operator=(const lexer&) = delete;

        std::vector<token> tokenize();

    private:

        // primitives --------------------------------------------------------------------------------------------------

        bool at_end() const noexcept;

        char peek(std::uint32_t offset = 0) const noexcept;

        char at(std::uint32_t offset) const noexcept;

        void advance(std::uint32_t n = 1) noexcept;

        // token factory -----------------------------------------------------------------------------------------------

        token make_token(token_kind kind, std::uint32_t start, std::uint32_t end) const;

        // recovery-friendly emitters ----------------------------------------------------------------------------------

        token make_invalid(std::uint32_t start, std::uint32_t end) const;

        void report(diagnostic_code code, std::uint32_t begin, std::uint32_t end, std::string message);

        // sub-lexers --------------------------------------------------------------------------------------------------

        void skip_whitespace_and_comments();
        token lex_identifier_or_keyword();
        token lex_number();
        token lex_string();
        token lex_char();
        token lex_punctuation();

        // helpers -----------------------------------------------------------------------------------------------------
        static bool is_identifier_start(char c) noexcept;
        static bool is_identifier_continue(char c) noexcept;
        static bool is_digit(char c) noexcept;
        static bool is_hex_digit(char c) noexcept;
        static bool is_oct_digit(char c) noexcept;
        static bool is_bin_digit(char c) noexcept;


        source_manager&             m_sm;
        const source_file&          m_file;
        diagnostic_engine&          m_diag;
        std::string_view            m_content;
        std::uint32_t               m_pos = 0;

    };

}

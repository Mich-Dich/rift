
#pragma once

#include "rift/ast.h"
#include "rift/diagnostic_engine.h"
#include "rift/source.h"
#include "rift/token.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>



// FORWARD DECLARATIONS ================================================================================================

namespace rift {

    // CONSTANTS =======================================================================================================

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    // STATIC VARIABLES ================================================================================================

    // FUNCTION DECLARATION ============================================================================================

    // TEMPLATE DECLARATION ============================================================================================

    // CLASS DECLARATION ===============================================================================================

    // Recursive-descent parser for declarations/statements, Pratt parser for expressions. The parser takes ownership of
    // the token stream; the source file (and thus each token's string_view lexeme) must outlive the parser
    //
    // Call `parse_program()` once. Even on error the returned `node_id` points to a valid (possibly partial) `program_node`.
    // Diagnostics accumulate in the supplied diagnostic_engine
    class parser {
    public:

        parser(source_manager& sm, file_id file, diagnostic_engine& diag, std::vector<token> tokens);

        parser(const parser&) = delete;
        parser& operator=(const parser&) = delete;

        node_id parse_program();

        const ast_arena& arena() const noexcept { return m_arena; }

    private:

        // Token navigation -----------------------------------------------------------------------------------------

        const token& peek(std::size_t ahead = 0) const;

        const token& current() const { return peek(0); }

        const token& advance();

        bool check(token_kind k) const { return current().kind == k; }

        bool check_any(std::initializer_list<token_kind> ks) const;

        bool match(token_kind k);

        bool expect(token_kind k, diagnostic_code code, std::string message);

        // Diagnostics ----------------------------------------------------------------------------------------------

        void error(diagnostic_code code, source_span span, std::string message);

        void error_expected(std::string_view what, source_span span = {});

        // Helpers --------------------------------------------------------------------------------------------------

        source_span make_span(std::uint32_t begin, std::uint32_t end) const;
        std::uint32_t consume_semicolon_or_error(source_span anchor, std::string message);

        // Recursive descent ----------------------------------------------------------------------------------------

        node_id parse_declaration();
        node_id parse_function_decl();
        node_id parse_parameter();
        node_id parse_block();
        node_id parse_statement();
        node_id parse_return_stmt();
        node_id parse_var_decl();
        node_id parse_type();
        node_id parse_if_stmt();
        node_id parse_while_stmt();
        node_id parse_assign_stmt();

        // Expressions (Pratt) --------------------------------------------------------------------------------------

        node_id parse_expression(int min_bp = 0);
        node_id parse_prefix();
        node_id parse_primary();

        // State ----------------------------------------------------------------------------------------------------

        source_manager&                     m_sm;
        const source_file&                  m_file;
        diagnostic_engine&                  m_diag;
        std::vector<token>                  m_tokens;
        std::size_t                         m_pos = 0;
        ast_arena                           m_arena;
    };

}

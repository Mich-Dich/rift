
#include "rift/parser.h"
#include "rift/diagnostic.h"

#include <format>
#include <utility>



// FORWARD DECLARATIONS ================================================================================================

namespace rift {

    // CONSTANTS =======================================================================================================

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    // STATIC VARIABLES ================================================================================================

    // INTERNAL TEMPLATE DECLARATION ===================================================================================

    // INTERNAL FUNCTION DECLARATION ===================================================================================

    std::pair<int, int> infix_bp(token_kind k);


    int prefix_bp(token_kind k);


    token_kind extract_int_suffix(std::string_view lexeme);

    // INTERNAL TEMPLATE IMPLEMENTATION ================================================================================

    // INTERNAL FUNCTION IMPLEMENTATION ================================================================================

    // Left/right binding powers for binary operators. Left-associative:
    // (p, p+1). Right-associative: (p, p). (0, 0) means "not an infix operator".
    std::pair<int, int> infix_bp(token_kind k) {

        switch (k) {
            case token_kind::star:              [[fallthrough]];
            case token_kind::slash:             [[fallthrough]];
            case token_kind::percent:           return { 60, 61 };
            case token_kind::plus:              [[fallthrough]];
            case token_kind::minus:             return { 50, 51 };
            case token_kind::lt_lt:             [[fallthrough]];
            case token_kind::gt_gt:             return { 45, 46 };
            case token_kind::lt:                [[fallthrough]];
            case token_kind::lt_eq:             [[fallthrough]];
            case token_kind::gt:                [[fallthrough]];
            case token_kind::gt_eq:             return { 40, 41 };
            case token_kind::eq_eq:             [[fallthrough]];
            case token_kind::bang_eq:           return { 35, 36 };
            case token_kind::amp:               return { 30, 31 };
            case token_kind::caret:             return { 29, 30 };
            case token_kind::pipe:              return { 28, 29 };
            case token_kind::amp_amp:           return { 20, 21 };
            case token_kind::pipe_pipe:         return { 15, 16 };
            default:                            return { 0, 0 };
        }
    }


    int prefix_bp(token_kind k) {

        switch (k) {
            case token_kind::minus:             [[fallthrough]];
            case token_kind::bang:              [[fallthrough]];
            case token_kind::tilde:             [[fallthrough]];
            case token_kind::plus:              return 70;
            default:                            return 0;
        }
    }


    // The lexer validated the suffix at lex time, so we only need to detect
    // which one (if any) terminates the lexeme. Order: longest first.
    token_kind extract_int_suffix(std::string_view lexeme) {

        static constexpr struct { std::string_view suffix; token_kind kind; } suffixes[] = {
            { "u128",       token_kind::prim_u128 },
            { "u64",        token_kind::prim_u64 },
            { "u32",        token_kind::prim_u32 },
            { "u16",        token_kind::prim_u16 },
            { "u8",         token_kind::prim_u8 },
            { "i128",       token_kind::prim_i128 },
            { "i64",        token_kind::prim_i64 },
            { "i32",        token_kind::prim_i32 },
            { "i16",        token_kind::prim_i16 },
            { "i8",         token_kind::prim_i8 },
        };

        for (const auto& s : suffixes) {
            if (lexeme.size() > s.suffix.size() &&
                lexeme.substr(lexeme.size() - s.suffix.size()) == s.suffix) {
                return s.kind;
            }
        }
        return token_kind::invalid;
    }

    // TEMPLATE IMPLEMENTATION =========================================================================================

    // FUNCTION IMPLEMENTATION =========================================================================================

    // CLASS IMPLEMENTATION ============================================================================================

    parser::parser(source_manager& sm, file_id file, diagnostic_engine& diag, std::vector<token> tokens)
        : m_sm(sm), m_file(sm.get(file)), m_diag(diag), m_tokens(std::move(tokens)) {}

    // CLASS PUBLIC ====================================================================================================

    node_id parser::parse_program() {

        std::vector<node_id> decls;

        while (!check(token_kind::end_of_file)) {
            const std::size_t before = m_pos;
            const node_id decl = parse_declaration();

            if (decl.is_valid())
                decls.push_back(decl);

            else if (m_pos == before)
                advance();          // No progress — force one token of advance so we can't loop

            else {
                // Best-effort recovery: skip to the next plausible decl start.
                while (!check(token_kind::end_of_file) && !current().is_primitive_type() && !check(token_kind::identifier))
                    advance();
            }
        }

        program_node p;
        p.declarations = std::move(decls);
        p.span = make_span(0, static_cast<std::uint32_t>(m_file.size()));
        return m_arena.add(std::move(p));
    }

    // CLASS PRIVATE ===================================================================================================

    // Token navigation ------------------------------------------------------------------------------------------------

    const token& parser::peek(std::size_t ahead) const {

        const std::size_t idx = m_pos + ahead;
        return idx < m_tokens.size() ? m_tokens[idx] : m_tokens.back();
    }


    const token& parser::advance() {

        const std::size_t old = m_pos;
        if (m_pos + 1 < m_tokens.size())
            m_pos++;
        return m_tokens[old];
    }


    bool parser::check_any(std::initializer_list<token_kind> ks) const {

        for (const auto k : ks)
            if (current().kind == k)
                return true;
        return false;
    }


    bool parser::match(token_kind k) {

        if (!check(k))
            return false;
        advance();
        return true;
    }


    bool parser::expect(token_kind k, diagnostic_code code, std::string message) {

        if (check(k)) {
            advance();
            return true;
        }
        error(code, current().span, std::move(message));
        return false;
    }

    // Diagnostics -----------------------------------------------------------------------------------------------------

    void parser::error(diagnostic_code code, source_span span, std::string message) { m_diag.error(code, span, std::move(message)); }


    void parser::error_expected(std::string_view what, source_span span) {

        if (span.file == invalid_file)
            span = current().span;
        error(diagnostic_code::expected_token, span, std::format("expected {}, found '{}'", what, token_kind_name(current().kind)));
    }

    // Helpers ---------------------------------------------------------------------------------------------------------

    source_span parser::make_span(std::uint32_t begin, std::uint32_t end) const { return source_span{ m_file.id(), begin, end }; }


    std::uint32_t parser::consume_semicolon_or_error(source_span anchor, std::string message) {

        if (check(token_kind::semicolon)) {
            advance();
            return m_tokens[m_pos - 1].span.end;
        }

        const source_span err_span = (anchor.file != invalid_file) ? source_span{ anchor.file, anchor.end, anchor.end } : current().span;

        diagnostic d;
        d.level = severity::error;
        d.code = diagnostic_code::expected_token;
        d.span = err_span;
        d.message = std::move(message);
        d.help = "add `;` here";
        m_diag.report(std::move(d));
        return current().span.begin;
    }

    // Recursive descent -----------------------------------------------------------------------------------------------

    node_id parser::parse_declaration() {

        if (current().is_primitive_type() || check(token_kind::identifier))
            return parse_function_decl();

        error(diagnostic_code::expected_declaration, current().span, std::format("expected a declaration, found '{}'",
            token_kind_name(current().kind)));
        return node_id{};
    }


    node_id parser::parse_function_decl() {

        const std::uint32_t start_off = current().span.begin;

        const node_id return_type = parse_type();
        if (!return_type.is_valid())
            return node_id{};

        if (!check(token_kind::identifier)) {
            error_expected("function name");
            return node_id{};
        }
        const token name_tok = advance();

        if (!expect(token_kind::l_paren, diagnostic_code::expected_token, "expected '(' after function name"))
            return node_id{};

        std::vector<node_id> params;
        while (!check(token_kind::r_paren) && !check(token_kind::end_of_file)) {
            const node_id p = parse_parameter();
            if (p.is_valid())
                params.push_back(p);
            if (!match(token_kind::comma))
                break;
        }

        if (!expect(token_kind::r_paren, diagnostic_code::expected_token, "expected ')' after parameters"))
            return node_id{};

        const node_id body = parse_block();
        if (!body.is_valid())
            return node_id{};

        function_node fn;
        fn.return_type = return_type;
        fn.name = name_tok.lexeme;
        fn.params = std::move(params);
        fn.body = body;
        fn.span = make_span(start_off, m_arena.span_of(body).end);
        return m_arena.add(std::move(fn));
    }


    node_id parser::parse_parameter() {

        const std::uint32_t start_off = current().span.begin;

        bool is_mut = false;
        if (match(token_kind::kw_mut))
            is_mut = true;

        const node_id type = parse_type();
        if (!type.is_valid())
            return node_id{};

        if (!check(token_kind::identifier)) {
            error_expected("parameter name");
            return node_id{};
        }
        const token name_tok = advance();

        parameter_node p;
        p.type = type;
        p.name = name_tok.lexeme;
        p.is_mut = is_mut;
        p.span = make_span(start_off, name_tok.span.end);
        return m_arena.add(std::move(p));
    }


    node_id parser::parse_block() {

        if (!expect(token_kind::l_brace, diagnostic_code::expected_block, "expected '{' to start block"))
            return node_id{};
        const std::uint32_t start_off = m_tokens[m_pos - 1].span.begin;

        std::vector<node_id> stmts;
        while (!check(token_kind::r_brace) && !check(token_kind::end_of_file)) {
            const std::size_t before = m_pos;
            const node_id s = parse_statement();
            if (s.is_valid())
                stmts.push_back(s);

            else if (m_pos == before)
                advance();  // force progress

            else {
                // Statement-level recovery: skip to ';' or '}'.
                while (!check(token_kind::end_of_file) &&
                       !check(token_kind::semicolon) &&
                       !check(token_kind::r_brace)) {
                    advance();
                }
                if (check(token_kind::semicolon)) advance();
            }
        }

        std::uint32_t end_off = m_tokens[m_pos > 0 ? m_pos - 1 : 0].span.end;
        // Span covers whatever we did consume
        if (!expect(token_kind::r_brace, diagnostic_code::unterminated_block, "expected '}' to close block"))
            end_off = current().span.begin;
        else
            end_off = m_tokens[m_pos - 1].span.end;

        block_node b;
        b.statements = std::move(stmts);
        b.span = make_span(start_off, end_off);
        return m_arena.add(std::move(b));
    }


    node_id parser::parse_statement() {

        if (check(token_kind::kw_return))
            return parse_return_stmt();

        if (check(token_kind::kw_if))
            return parse_if_stmt();

        if (check(token_kind::kw_while))
            return parse_while_stmt();

        if (check(token_kind::kw_mut))
            return parse_var_decl();

        if (current().is_primitive_type())
            return parse_var_decl();

        // `<Type> <name> ...` — named-type var decl.
        if (check(token_kind::identifier) && peek(1).kind == token_kind::identifier)
            return parse_var_decl();

        // `<name> = <expr> ;` — assignment.
        if (check(token_kind::identifier) && peek(1).kind == token_kind::eq)
            return parse_assign_stmt();

        error(diagnostic_code::expected_declaration, current().span,
            std::format("expected a statement, found '{}'", token_kind_name(current().kind)));
        return node_id{};
    }


    node_id parser::parse_return_stmt() {

        const std::uint32_t start_off = current().span.begin;
        advance();  // 'return'

        // M2 scope: only `<return> <expr> ';'` is supported. Bare `return;` will be re-enabled in M5 once `void` functions exist
        const node_id value = parse_expression();

        const source_span value_span = value.is_valid() ? m_arena.span_of(value) : source_span{};
        const std::uint32_t end_off = consume_semicolon_or_error(value_span, "expected ';' after return statement");

        return_stmt_node r;
        r.value = value;
        r.span = make_span(start_off, end_off);
        return m_arena.add(std::move(r));
    }


    node_id parser::parse_var_decl() {

        const std::uint32_t start_off = current().span.begin;

        bool is_mut = false;
        if (match(token_kind::kw_mut))
            is_mut = true;

        const node_id type = parse_type();
        if (!type.is_valid())
            return node_id{};

        if (!check(token_kind::identifier)) {
            error_expected("variable name");
            return node_id{};
        }
        const token name_tok = advance();

        if (!expect(token_kind::eq, diagnostic_code::expected_token, "expected '=' in variable declaration"))
            return node_id{};

        node_id init;
        if (check(token_kind::kw_default)) {
            const token d = advance();
            default_init_node n;
            n.span = d.span;
            init = m_arena.add(std::move(n));
        } else
            init = parse_expression();

        if (!init.is_valid())
            return node_id{};

        const source_span init_span = m_arena.span_of(init);
        const std::uint32_t end_off = consume_semicolon_or_error(init_span, "expected ';' after variable declaration");

        var_decl_node v;
        v.type = init.is_valid() ? type : type;  // type unchanged; kept for clarity
        v.name = name_tok.lexeme;
        v.is_mut = is_mut;
        v.initializer = init;
        v.span = make_span(start_off, end_off);
        return m_arena.add(std::move(v));
    }


    node_id parser::parse_type() {

        if (current().is_primitive_type()) {
            const token t = advance();
            primitive_type_node n;
            n.type_kind = t.kind;
            n.span = t.span;
            return m_arena.add(std::move(n));
        }

        if (check(token_kind::identifier)) {
            const token t = advance();
            named_type_node n;
            n.name = t.lexeme;
            n.span = t.span;
            return m_arena.add(std::move(n));
        }

        error_expected("a type");
        return node_id{};
    }


    node_id parser::parse_assign_stmt() {

        const std::uint32_t start_off = current().span.begin;
        const token name_tok = advance();   // identifier

        advance();                          // '='

        identifier_node target;
        target.name = name_tok.lexeme;
        target.span = name_tok.span;
        const node_id target_id = m_arena.add(std::move(target));

        const node_id value = parse_expression();
        if (!value.is_valid())
            return node_id{};

        const source_span val_span = m_arena.span_of(value);
        const std::uint32_t end_off = consume_semicolon_or_error(val_span, "expected ';' after assignment");

        assign_stmt_node n;
        n.target = target_id;
        n.value = value;
        n.span = make_span(start_off, end_off);
        return m_arena.add(std::move(n));
    }


    node_id parser::parse_if_stmt() {

        const std::uint32_t start_off = current().span.begin;
        advance();  // 'if'

        if (!expect(token_kind::l_paren, diagnostic_code::expected_token, "expected '(' after 'if'"))
            return node_id{};

        const node_id cond = parse_expression();
        if (!cond.is_valid())
            return node_id{};

        if (!expect(token_kind::r_paren, diagnostic_code::expected_token, "expected ')' after if condition"))
            return node_id{};

        const node_id then_block = parse_block();
        if (!then_block.is_valid())
            return node_id{};

        node_id else_block;
        if (match(token_kind::kw_else)) {
            if (check(token_kind::kw_if))
                else_block = parse_if_stmt();     // `else if`
            else
                else_block = parse_block();       // `else { ... }`
            if (!else_block.is_valid())
                return node_id{};
        }

        if_stmt_node n;
        n.condition = cond;
        n.then_block = then_block;
        n.else_block = else_block;
        n.span = make_span(start_off, else_block.is_valid() ? m_arena.span_of(else_block).end : m_arena.span_of(then_block).end);
        return m_arena.add(std::move(n));
    }


    node_id parser::parse_while_stmt() {

        const std::uint32_t start_off = current().span.begin;
        advance();  // 'while'

        if (!expect(token_kind::l_paren, diagnostic_code::expected_token, "expected '(' after 'while'"))
            return node_id{};

        const node_id cond = parse_expression();
        if (!cond.is_valid())
            return node_id{};

        if (!expect(token_kind::r_paren, diagnostic_code::expected_token, "expected ')' after while condition"))
            return node_id{};

        const node_id body = parse_block();
        if (!body.is_valid())
            return node_id{};

        while_stmt_node n;
        n.condition = cond;
        n.body = body;
        n.span = make_span(start_off, m_arena.span_of(body).end);
        return m_arena.add(std::move(n));
    }

    // Expressions (Pratt) ---------------------------------------------------------------------------------------------

    node_id parser::parse_expression(int min_bp) {

        node_id lhs = parse_prefix();
        if (!lhs.is_valid())
            return lhs;

        while (true) {
            const auto [lbp, rbp] = infix_bp(current().kind);
            if (lbp == 0 || lbp < min_bp)
                break;

            const token op = advance();
            const node_id rhs = parse_expression(rbp);
            if (!rhs.is_valid())
                return lhs;

            binary_expr_node bin;
            bin.op = op.kind;
            bin.lhs = lhs;
            bin.rhs = rhs;
            bin.span = make_span(m_arena.span_of(lhs).begin, m_arena.span_of(rhs).end);
            lhs = m_arena.add(std::move(bin));
        }

        return lhs;
    }


    node_id parser::parse_prefix() {

        if (const int pbp = prefix_bp(current().kind); pbp > 0) {

            const token op = advance();
            const node_id operand = parse_expression(pbp);
            if (!operand.is_valid())
                return node_id{};

            unary_expr_node un;
            un.op = op.kind;
            un.operand = operand;
            un.span = make_span(op.span.begin, m_arena.span_of(operand).end);
            return m_arena.add(std::move(un));
        }

        return parse_primary();
    }


    node_id parser::parse_primary() {

        const token& t = current();

        if (t.kind == token_kind::integer_literal) {
            advance();
            int_literal_node lit;
            lit.lexeme = t.lexeme;
            lit.suffix_kind = extract_int_suffix(t.lexeme);
            lit.span = t.span;
            return m_arena.add(std::move(lit));
        }

        if (t.kind == token_kind::kw_true || t.kind == token_kind::kw_false) {
            advance();
            bool_literal_node n;
            n.value = (t.kind == token_kind::kw_true);
            n.span = t.span;
            return m_arena.add(std::move(n));
        }

        if (t.kind == token_kind::kw_bit) {
            const token kw = advance();
            if (!expect(token_kind::l_paren, diagnostic_code::expected_token, "expected '(' after 'bit'"))
                return node_id{};
            const node_id operand = parse_expression(0);
            if (!operand.is_valid())
                return node_id{};
            if (!expect(token_kind::r_paren, diagnostic_code::expected_token, "expected ')' after bit index"))
                return node_id{};

            bit_expr_node n;
            n.operand = operand;
            n.span = make_span(kw.span.begin, m_tokens[m_pos - 1].span.end);
            return m_arena.add(std::move(n));
        }

        if (t.kind == token_kind::identifier) {
            const token name_tok = advance();

            if (check(token_kind::l_paren)) {
                advance();  // '('

                std::vector<node_id> args;
                while (!check(token_kind::r_paren) && !check(token_kind::end_of_file)) {
                    const node_id a = parse_expression(0);
                    if (a.is_valid())
                        args.push_back(a);
                    if (!match(token_kind::comma))
                        break;
                }

                if (!expect(token_kind::r_paren, diagnostic_code::expected_token, "expected ')' after call arguments"))
                    return node_id{};

                identifier_node callee;
                callee.name = name_tok.lexeme;
                callee.span = name_tok.span;
                const node_id callee_id = m_arena.add(std::move(callee));

                call_expr_node call;
                call.callee = callee_id;
                call.args = std::move(args);
                call.span = make_span(name_tok.span.begin, m_tokens[m_pos - 1].span.end);
                return m_arena.add(std::move(call));
            }

            identifier_node id;
            id.name = name_tok.lexeme;
            id.span = name_tok.span;
            return m_arena.add(std::move(id));
        }

        if (t.kind == token_kind::l_paren) {
            advance();
            const node_id inner = parse_expression(0);
            if (!expect(token_kind::r_paren, diagnostic_code::expected_token, "expected ')' after expression"))
                return inner;
            return inner;
        }

        error_expected("an expression");
        return node_id{};
    }

    // CLASS PROTECTED =================================================================================================

    // CLASS PRIVATE ===================================================================================================

}


#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <unordered_map>

#include "rift/lexer.h"
#include "rift/token.h"
// NOTE: #include "rift/util.h" is intentionally absent for M1. util.h carries reflection-based helpers that are not needed by
// the lexer and will not compile without the reflection flags. It can come back when a milestone actually needs it



// FORWARD DECLARATIONS ================================================================================================

namespace rift {

    // CONSTANTS =======================================================================================================

    // Keyword → token kind. Populated once; lookups during lexing use it to retag identifiers
    const std::unordered_map<std::string_view, token_kind>& keyword_table() {
        static const std::unordered_map<std::string_view, token_kind> table = {
            // Declarations
            {"import",                  token_kind::kw_import},
            {"struct",                  token_kind::kw_struct},
            {"class",                   token_kind::kw_class},
            {"open",                    token_kind::kw_open},
            {"enum",                    token_kind::kw_enum},
            {"interface",               token_kind::kw_interface},
            {"impl",                    token_kind::kw_impl},
            // Modifiers
            {"virtual",                 token_kind::kw_virtual},
            {"override",                token_kind::kw_override},
            {"mut",                     token_kind::kw_mut},
            {"private",                 token_kind::kw_private},
            {"static",                  token_kind::kw_static},
            // Compile-time
            {"compile_time",            token_kind::kw_compile_time},
            {"run_time",                token_kind::kw_run_time},
            {"deduce",                  token_kind::kw_deduce},
            // Control flow
            {"if",                      token_kind::kw_if},
            {"else",                    token_kind::kw_else},
            {"match",                   token_kind::kw_match},
            {"for",                     token_kind::kw_for},
            {"in",                      token_kind::kw_in},
            {"return",                  token_kind::kw_return},
            // Literals
            {"true",                    token_kind::kw_true},
            {"false",                   token_kind::kw_false},
            {"null",                    token_kind::kw_null},
            // Expressions / types
            {"cast",                    token_kind::kw_cast},
            {"unsafe",                  token_kind::kw_unsafe},
            {"auto",                    token_kind::kw_auto},
            {"default",                 token_kind::kw_default},
            {"bit",                     token_kind::kw_bit},
            // Primitive types
            {"u8",                      token_kind::prim_u8},
            {"u16",                     token_kind::prim_u16},
            {"u32",                     token_kind::prim_u32},
            {"u64",                     token_kind::prim_u64},
            {"u128",                    token_kind::prim_u128},
            {"i8",                      token_kind::prim_i8},
            {"i16",                     token_kind::prim_i16},
            {"i32",                     token_kind::prim_i32},
            {"i64",                     token_kind::prim_i64},
            {"i128",                    token_kind::prim_i128},
            {"f32",                     token_kind::prim_f32},
            {"f64",                     token_kind::prim_f64},
            {"bool",                    token_kind::prim_bool},
            {"void",                    token_kind::prim_void},
        };
        return table;
    }

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    // Valid integer suffixes — map a suffix to whether it's a float type
    // `true`  → float (f32/f64)
    // `false` → integer (u*/i*)
    struct suffix_info {
        bool is_float;
        bool is_valid;
    };

    // STATIC VARIABLES ================================================================================================

    // INTERNAL TEMPLATE DECLARATION ===================================================================================

    // INTERNAL FUNCTION DECLARATION ===================================================================================

    suffix_info classify_suffix(std::string_view suffix);


    bool is_codepoint_valid(std::uint32_t cp);


    void append_utf8(std::string& out, std::uint32_t cp);

    // INTERNAL TEMPLATE IMPLEMENTATION ================================================================================

    // INTERNAL FUNCTION IMPLEMENTATION ================================================================================

    suffix_info classify_suffix(std::string_view suffix) {

        if (suffix.empty())
            return { false, true };
        if (suffix == "f32" || suffix == "f64")
            return { true, true };
        if (suffix.size() >= 2 && (suffix[0] == 'u' || suffix[0] == 'i')) {
            const std::string_view bits = suffix.substr(1);
            if (bits == "8" || bits == "16" || bits == "32" ||
                bits == "64" || bits == "128") {
                return { false, true };
            }
        }
        return { false, false };
    }


    bool is_codepoint_valid(std::uint32_t cp) {

        if (cp > 0x10FFFF)
            return false;
        if (cp >= 0xD800 && cp <= 0xDFFF)
            return false;  // surrogates
        return true;
    }


    void append_utf8(std::string& out, std::uint32_t cp) {

        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    // TEMPLATE IMPLEMENTATION =========================================================================================

    // FUNCTION IMPLEMENTATION =========================================================================================

    // CLASS IMPLEMENTATION ============================================================================================

    lexer::lexer(source_manager& sm, file_id file, diagnostic_engine& diag)
        : m_sm(sm), m_file(sm.get(file)), m_diag(diag), m_content(m_file.content()) {}

    // CLASS PUBLIC ====================================================================================================

    std::vector<token> lexer::tokenize() {

        std::vector<token> out;
        while (true) {
            skip_whitespace_and_comments();
            if (at_end()) {
                out.push_back(make_token(token_kind::end_of_file, m_pos, m_pos));
                break;
            }
            const char c = peek();
            if (is_identifier_start(c))
                out.push_back(lex_identifier_or_keyword());
            else if (is_digit(c))
                out.push_back(lex_number());
            else if (c == '"')
                out.push_back(lex_string());
            else if (c == '\'')
                out.push_back(lex_char());
            else
                out.push_back(lex_punctuation());
        }
        return out;
    }

    // CLASS PROTECTED =================================================================================================

    // CLASS PRIVATE ===================================================================================================

    // primitives --------------------------------------------------------------------------------------------------

    bool lexer::at_end() const noexcept { return m_pos >= m_content.size(); }


    char lexer::peek(std::uint32_t offset) const noexcept {

        const std::uint32_t p = m_pos + offset;
        return p < m_content.size() ? m_content[p] : '\0';
    }


    char lexer::at(std::uint32_t offset) const noexcept { return offset < m_content.size() ? m_content[offset] : '\0'; }


    void lexer::advance(std::uint32_t n) noexcept {

        m_pos = m_pos + n < m_content.size() ? m_pos + n : static_cast<std::uint32_t>(m_content.size());
    }

    // token factory -----------------------------------------------------------------------------------------------

    token lexer::make_token(token_kind kind, std::uint32_t start, std::uint32_t end) const {

        return token{
            .lexeme = m_content.substr(start, end - start),
            .kind = kind,
            .span = source_span{ m_file.id(), start, end },
        };
    }

    // recovery-friendly emitters ----------------------------------------------------------------------------------

    token lexer::make_invalid(std::uint32_t start, std::uint32_t end) const { return make_token(token_kind::invalid, start, end); }

    // error emission --------------------------------------------------------------------------------------------------

    void lexer::report(diagnostic_code code, std::uint32_t begin, std::uint32_t end, std::string message) {

        m_diag.error(code, source_span{ m_file.id(), begin, end }, std::move(message));
    }

    // sub-lexer -------------------------------------------------------------------------------------------------------

    void lexer::skip_whitespace_and_comments() {

        while (!at_end()) {
            const char c = peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') {
                advance();
                continue;
            }
            if (c == '/' && peek(1) == '/') {
                advance(2);
                while (!at_end() && peek() != '\n')
                    advance();
                continue;
            }
            if (c == '/' && peek(1) == '*') {

                const std::uint32_t start = m_pos;
                advance(2);
                std::uint32_t depth = 1;
                while (!at_end() && depth > 0) {
                    if (peek() == '/' && peek(1) == '*') {

                        advance(2);
                        depth++;

                    } else if (peek() == '*' && peek(1) == '/') {

                        advance(2);
                        depth--;
                    } else advance();
                }
                if (depth > 0)
                    report(diagnostic_code::unterminated_block_comment, start, m_pos, "unterminated block comment");

                continue;
            }
            break;
        }
    }


    token lexer::lex_identifier_or_keyword() {

        const std::uint32_t start = m_pos;
        while (!at_end() && is_identifier_continue(peek()))
            advance();
        const std::string_view lexeme = m_content.substr(start, m_pos - start);

        // A single `_` is the wildcard; `_foo` and `_1` are identifiers.
        if (lexeme == "_")
            return make_token(token_kind::underscore, start, m_pos);

        const auto& kw = keyword_table();
        auto it = kw.find(lexeme);
        if (it != kw.end()) {
            return make_token(it->second, start, m_pos);
        }
        return make_token(token_kind::identifier, start, m_pos);
    }


    token lexer::lex_number() {
        
        const std::uint32_t start = m_pos;
        enum class base_t { dec, hex, oct, bin } base = base_t::dec;

        if (peek() == '0') {
            const char p1 = peek(1);
            if (p1 == 'x' || p1 == 'X') {
                base = base_t::hex; advance(2);

            } else if (p1 == 'o' || p1 == 'O') {
                base = base_t::oct; advance(2);

            } else if (p1 == 'b' || p1 == 'B') {
                base = base_t::bin; advance(2);
            }
        }

        if (base != base_t::dec) {
            const std::uint32_t digits_begin = m_pos;
            bool last_was_digit = false;
            while (!at_end()) {
                const char c = peek();
                bool ok = false;
                if (base == base_t::hex)
                    ok = is_hex_digit(c) || c == '_';
                else if (base == base_t::oct)
                    ok = is_oct_digit(c) || c == '_';
                else
                    ok = is_bin_digit(c) || c == '_';

                if (!ok)
                    break;
                if (c == '_') {
                    if (!last_was_digit) {
                        report(diagnostic_code::trailing_underscore_in_number, m_pos, m_pos + 1, "underscore must appear between digits");
                    }
                    last_was_digit = false;
                } else {
                    last_was_digit = true;
                }
                advance();
            }
            if (m_pos == digits_begin) {
                report(diagnostic_code::missing_digits_after_prefix, start, m_pos, "expected at least one digit after numeric base prefix");
                return make_invalid(start, m_pos);
            }
            if (!last_was_digit) {
                report(diagnostic_code::trailing_underscore_in_number, m_pos - 1, m_pos, "underscore must appear between digits");
                return make_invalid(start, m_pos);
            }

        } else {

            while (!at_end()) {
                const char c = peek();
                if (is_digit(c))
                    advance();

                else if (c == '_')
                    advance();

                else
                    break;
            }

            // Fractional part: `.` MUST be followed by a digit; otherwise the
            // `.` is a separate token (so `0..=9` and `1.foo` work correctly).
            bool has_fraction = false;
            if (peek() == '.' && is_digit(peek(1))) {
                has_fraction = true;
                advance();  // consume '.'
                while (!at_end() && (is_digit(peek()) || peek() == '_'))
                    advance();
            }

            // Exponent: `e`/`E` followed by an optional sign and digits.
            bool has_exponent = false;
            if (peek() == 'e' || peek() == 'E') {
                const char n1 = peek(1);
                const bool sign_then_digit = (n1 == '+' || n1 == '-') && is_digit(peek(2));
                if (is_digit(n1) || sign_then_digit) {
                    has_exponent = true;
                    advance();                                      // consume 'e'/'E'
                    if (peek() == '+' || peek() == '-')
                        advance();                                  // consume optional sign
                    while (!at_end() && (is_digit(peek()) || peek() == '_'))
                        advance();
                }
            }

            // Suffix
            std::string_view suffix;
            {
                const std::uint32_t suffix_begin = m_pos;
                while (!at_end() && is_identifier_continue(peek()))
                    advance();
                suffix = m_content.substr(suffix_begin, m_pos - suffix_begin);
            }

            const auto info = classify_suffix(suffix);
            if (!info.is_valid) {
                report(diagnostic_code::invalid_integer_suffix,
                    m_pos - static_cast<std::uint32_t>(suffix.size()), m_pos,
                    std::format("invalid numeric suffix '{}'", suffix));
                return make_invalid(start, m_pos);
            }

            const bool is_float = has_fraction || has_exponent || info.is_float;

            if (has_fraction || has_exponent) {
                if (!suffix.empty() && !info.is_float) {
                    report(diagnostic_code::invalid_integer_suffix,
                        m_pos - static_cast<std::uint32_t>(suffix.size()), m_pos,
                        std::format("integer suffix '{}' is not valid on a float literal", suffix));
                    return make_invalid(start, m_pos);
                }
            }

            return make_token(is_float ? token_kind::float_literal : token_kind::integer_literal, start, m_pos);
        }

        // Non-decimal: suffix must be an integer type or absent.
        std::string_view suffix;
        {
            const std::uint32_t suffix_begin = m_pos;
            while (!at_end() && is_identifier_continue(peek())) advance();
            suffix = m_content.substr(suffix_begin, m_pos - suffix_begin);
        }
        const auto info = classify_suffix(suffix);
        if (!info.is_valid) {
            report(diagnostic_code::invalid_integer_suffix,
                m_pos - static_cast<std::uint32_t>(suffix.size()), m_pos,
                std::format("invalid numeric suffix '{}'", suffix));
            return make_invalid(start, m_pos);
        }
        if (info.is_float) {
            report(diagnostic_code::invalid_integer_suffix,
                m_pos - static_cast<std::uint32_t>(suffix.size()), m_pos,
                std::format("float suffix '{}' is not valid on a hex/oct/bin literal",
                            suffix));
            return make_invalid(start, m_pos);
        }
        return make_token(token_kind::integer_literal, start, m_pos);
    }


    token lexer::lex_string() {

        const std::uint32_t start = m_pos;
        advance();  // consume opening "

        bool ok = true;
        while (!at_end()) {
            const char c = peek();
            if (c == '"') {
                advance();
                return make_token(token_kind::string_literal, start, m_pos);

            } if (c == '\n') {
                report(diagnostic_code::unterminated_string, start, m_pos, "unterminated string literal");
                return make_invalid(start, m_pos);
            
            } if (c == '\\') {
                // Validate the escape; consume it.
                advance();  // backslash
                if (at_end() || peek() == '\n') {
                    ok = false;
                    break;
                }
                const char e = peek();
                if (e == 'n' || e == 't' || e == 'r' || e == '\\' ||
                    e == '"'  || e == '\'' || e == '0') {
                    advance();
                } else if (e == 'x') {
                    advance();
                    if (!is_hex_digit(peek()) || !is_hex_digit(peek(1))) {
                        report(diagnostic_code::invalid_escape, m_pos - 2, m_pos, "\\x escape requires exactly two hex digits");
                        ok = false;
                        advance(2);
                        continue;
                    }
                    advance(2);
                } else if (e == 'u') {
                    advance();
                    if (peek() != '{') {
                        report(diagnostic_code::invalid_escape, m_pos - 2, m_pos, "\\u escape requires '{', e.g. \\u{1F600}");
                        ok = false;
                        continue;
                    }
                    advance();
                    std::uint32_t cp = 0;
                    int digits = 0;
                    while (!at_end() && is_hex_digit(peek()) && digits < 6) {
                        cp = cp * 16 + static_cast<std::uint32_t>(
                            peek() >= '0' && peek() <= '9' ? peek() - '0'
                            : peek() >= 'a' && peek() <= 'f' ? peek() - 'a' + 10
                                                            : peek() - 'A' + 10);
                        advance();
                        ++digits;
                    }
                    if (digits == 0 || peek() != '}') {
                        report(diagnostic_code::invalid_escape, m_pos - 3, m_pos, "\\u escape requires 1-6 hex digits enclosed in '{...}'");
                        ok = false;
                        continue;
                    }
                    advance();  // consume '}'
                    if (!is_codepoint_valid(cp)) {
                        report(diagnostic_code::invalid_unicode_codepoint, m_pos - 4, m_pos, std::format("invalid Unicode codepoint U+{:X}", cp));
                        ok = false;
                    }
                } else {
                    report(diagnostic_code::invalid_escape, m_pos - 1, m_pos + 1, std::format("unknown escape sequence '\\{}'", e));
                    ok = false;
                    advance();
                }
                continue;
            }
            advance();
        }

        if (ok) {
            report(diagnostic_code::unterminated_string, start, m_pos, "unterminated string literal");
        }
        return make_invalid(start, m_pos);
    }


    token lexer::lex_char() {
        const std::uint32_t start = m_pos;
        advance();  // consume opening '

        if (at_end() || peek() == '\n' || peek() == '\'') {
            report(diagnostic_code::empty_char_literal, start, m_pos, "empty character literal");
            if (peek() == '\'') advance();
            return make_invalid(start, m_pos);
        }

        // One element: either an escape or a single non-quote byte.
        if (peek() == '\\') {
            advance();
            if (at_end() || peek() == '\n') {
                report(diagnostic_code::unterminated_char, start, m_pos, "unterminated character literal");
                return make_invalid(start, m_pos);
            }
            const char e = peek();
            if (e == 'n' || e == 't' || e == 'r' || e == '\\' || e == '"'  || e == '\'' || e == '0') {
                advance();

            } else if (e == 'x') {
                advance();
                if (!is_hex_digit(peek()) || !is_hex_digit(peek(1))) {
                    report(diagnostic_code::invalid_escape,
                        start, m_pos,
                        "\\x escape requires exactly two hex digits");
                    return make_invalid(start, m_pos);
                }
                advance(2);

            } else if (e == 'u') {
                advance();
                if (peek() != '{') {
                    report(diagnostic_code::invalid_escape,
                        start, m_pos,
                        "\\u escape requires '{'");
                    return make_invalid(start, m_pos);
                }
                advance();
                std::uint32_t cp = 0;
                int digits = 0;
                while (!at_end() && is_hex_digit(peek()) && digits < 6) {
                    cp = cp * 16 + static_cast<std::uint32_t>(
                        peek() >= '0' && peek() <= '9' ? peek() - '0'
                        : peek() >= 'a' && peek() <= 'f' ? peek() - 'a' + 10
                                                        : peek() - 'A' + 10);
                    advance();
                    ++digits;
                }
                if (digits == 0 || peek() != '}') {
                    report(diagnostic_code::invalid_escape,
                        start, m_pos,
                        "\\u escape requires 1-6 hex digits enclosed in '{...}'");
                    return make_invalid(start, m_pos);
                }
                advance();
                if (!is_codepoint_valid(cp)) {
                    report(diagnostic_code::invalid_unicode_codepoint,
                        start, m_pos,
                        std::format("invalid Unicode codepoint U+{:X}", cp));
                    return make_invalid(start, m_pos);
                }

            } else {
                report(diagnostic_code::invalid_escape, start, m_pos + 1, std::format("unknown escape sequence '\\{}'", e));
                return make_invalid(start, m_pos);
            }
        } else {
            // Non-ASCII in a char is deferred; ASCII-only for M1.
            const char c = peek();
            if (static_cast<unsigned char>(c) >= 0x80) {
                report(diagnostic_code::invalid_char_in_literal, m_pos, m_pos + 1,
                    "non-ASCII character literals are not supported yet; use a \\u escape");
                advance();
                return make_invalid(start, m_pos);
            }
            advance();
        }

        if (peek() != '\'') {
            report(diagnostic_code::multi_char_char_literal, start, m_pos, "character literal contains more than one character");
            // Consume to closing quote or end of line.
            while (!at_end() && peek() != '\'' && peek() != '\n')
                advance();
            if (peek() == '\'')
                advance();
            return make_invalid(start, m_pos);
        }
        advance();  // consume closing '
        return make_token(token_kind::char_literal, start, m_pos);
    }


    token lexer::lex_punctuation() {

        const std::uint32_t start = m_pos;
        const char c = peek();

        auto single = [&](token_kind k) {
            advance();
            return make_token(k, start, m_pos);
        };
        auto pair = [&](token_kind k) {
            advance(2);
            return make_token(k, start, m_pos);
        };
        auto triple = [&](token_kind k) {
            advance(3);
            return make_token(k, start, m_pos);
        };

        switch (c) {
            case '{':                                           return single(token_kind::l_brace);
            case '}':                                           return single(token_kind::r_brace);
            case '(':                                           return single(token_kind::l_paren);
            case ')':                                           return single(token_kind::r_paren);
            case '[':                                           return single(token_kind::l_bracket);
            case ']':                                           return single(token_kind::r_bracket);
            case ';':                                           return single(token_kind::semicolon);
            case ',':                                           return single(token_kind::comma);
            case '?':                                           return single(token_kind::question);

            case '.':
                if (peek(1) == '.' && peek(2) == '=')           return triple(token_kind::dot_dot_eq);
                if (peek(1) == '.')                             return pair(token_kind::dot_dot);
                return single(token_kind::dot);

            case ':':
                if (peek(1) == ':')                             return pair(token_kind::double_colon);
                return single(token_kind::colon);

            case '+':
                if (peek(1) == '=')                             return pair(token_kind::plus_eq);
                return single(token_kind::plus);

            case '-':
                if (peek(1) == '>')                             return pair(token_kind::arrow);
                if (peek(1) == '=')                             return pair(token_kind::minus_eq);
                return single(token_kind::minus);

            case '*':
                if (peek(1) == '=')                             return pair(token_kind::star_eq);
                return single(token_kind::star);

            case '/':
                // Comments already consumed in skip_whitespace_and_comments().
                if (peek(1) == '=')                             return pair(token_kind::slash_eq);
                return single(token_kind::slash);

            case '%':
                if (peek(1) == '=')                             return pair(token_kind::percent_eq);
                return single(token_kind::percent);

            case '=':
                if (peek(1) == '=')                             return pair(token_kind::eq_eq);
                if (peek(1) == '>')                             return pair(token_kind::fat_arrow);
                return single(token_kind::eq);

            case '!':
                if (peek(1) == '=')                             return pair(token_kind::bang_eq);
                return single(token_kind::bang);

            case '<':
                if (peek(1) == '<' && peek(2) == '=')           return triple(token_kind::lt_lt_eq);
                if (peek(1) == '<')                             return pair(token_kind::lt_lt);
                if (peek(1) == '=')                             return pair(token_kind::lt_eq);
                return single(token_kind::lt);

            case '>':
                if (peek(1) == '>' && peek(2) == '=')           return triple(token_kind::gt_gt_eq);
                if (peek(1) == '>')                             return pair(token_kind::gt_gt);
                if (peek(1) == '=')                             return pair(token_kind::gt_eq);
                return single(token_kind::gt);

            case '&':
                if (peek(1) == '&')                             return pair(token_kind::amp_amp);
                if (peek(1) == '=')                             return pair(token_kind::amp_eq);
                return single(token_kind::amp);

            case '|':
                if (peek(1) == '|')                             return pair(token_kind::pipe_pipe);
                if (peek(1) == '=')                             return pair(token_kind::pipe_eq);
                return single(token_kind::pipe);

            case '^':
                if (peek(1) == '=')                             return pair(token_kind::caret_eq);
                return single(token_kind::caret);

            case '~':
                return single(token_kind::tilde);

            default: {
                report(diagnostic_code::unexpected_character, start, start + 1, std::format("unexpected character '{}'", static_cast<char>(c)));
                advance();
                return make_invalid(start, m_pos);
            }
        }
    }

    // classification helpers ------------------------------------------------------------------------------------------

    bool lexer::is_identifier_start(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }


    bool lexer::is_identifier_continue(char c) noexcept { return is_identifier_start(c) || (c >= '0' && c <= '9'); }


    bool lexer::is_digit(char c) noexcept { return c >= '0' && c <= '9'; }


    bool lexer::is_hex_digit(char c) noexcept { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }


    bool lexer::is_oct_digit(char c) noexcept { return c >= '0' && c <= '7'; }


    bool lexer::is_bin_digit(char c) noexcept { return c == '0' || c == '1'; }

}

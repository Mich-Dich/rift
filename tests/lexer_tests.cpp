
#include "test_framework.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "rift/diagnostic_engine.h"
#include "rift/lexer.h"
#include "rift/source.h"
#include "rift/token.h"



using namespace rift;

// FORWARD DECLARATIONS ================================================================================================

namespace {

    // CONSTANTS =======================================================================================================

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    struct lex_result {

        std::unique_ptr<source_manager>             sm;
        diagnostic_engine                           diag;
        std::vector<token>                          tokens;
    };

    // STATIC VARIABLES ================================================================================================

    // INTERNAL TEMPLATE DECLARATION ===================================================================================

    // INTERNAL FUNCTION DECLARATION ===================================================================================

    lex_result lex(std::string_view source);


    std::vector<token> without_eof(std::vector<token> v);


    std::vector<token_kind> kinds_of(const std::vector<token>& toks);

    // INTERNAL TEMPLATE IMPLEMENTATION ================================================================================

    // INTERNAL FUNCTION IMPLEMENTATION ================================================================================

    // Lexes `source` and keeps the source_manager alive. Returned by value is fine — the tokens' string_views point into
    // `sm`'s deque, which is stable across moves of `sm`
    lex_result lex(std::string_view source) {

        lex_result r;
        r.sm = std::make_unique<source_manager>();
        const file_id f = r.sm->add("<test>", std::string(source));
        lexer lx(*r.sm, f, r.diag);
        r.tokens = lx.tokenize();
        return r;
    }


    // Drops the final `end_of_file` token for convenience
    std::vector<token> without_eof(std::vector<token> v) {

        if (!v.empty() && v.back().kind == token_kind::end_of_file)
            v.pop_back();
        return v;
    }


    std::vector<token_kind> kinds_of(const std::vector<token>& toks) {

        std::vector<token_kind> out;
        out.reserve(toks.size());
        for (const auto& t : toks) out.push_back(t.kind);
        return out;
    }

    // TEMPLATE IMPLEMENTATION =========================================================================================

    // FUNCTION IMPLEMENTATION =========================================================================================

    // CLASS IMPLEMENTATION ============================================================================================

    // CLASS PUBLIC ====================================================================================================

    // CLASS PROTECTED =================================================================================================

    // CLASS PRIVATE ===================================================================================================

}


// TESTS ===============================================================================================================

TEST(lexer_empty_source) {
    auto r = lex("");
    EXPECT_FALSE(r.diag.has_errors());
    EXPECT_EQ(r.tokens.size(), 1u);
    EXPECT_EQ(r.tokens[0].kind, token_kind::end_of_file);
}

TEST(lexer_compile_check) {
    auto r = lex("u32 main() {\n    return 0u32;\n}\n");
    EXPECT_FALSE(r.diag.has_errors());
    const auto toks = without_eof(r.tokens);
    EXPECT_EQ(kinds_of(toks), (std::vector<token_kind>{
        token_kind::prim_u32,
        token_kind::identifier,
        token_kind::l_paren,
        token_kind::r_paren,
        token_kind::l_brace,
        token_kind::kw_return,
        token_kind::integer_literal,
        token_kind::semicolon,
        token_kind::r_brace,
    }));
    EXPECT_EQ(toks[6].lexeme, "0u32");
}

TEST(lexer_keywords) {
    auto r = lex(
        "import struct class open enum interface impl "
        "virtual override mut private static "
        "compile_time run_time deduce "
        "if else match for in return "
        "true false null "
        "cast unsafe auto default bit");
    EXPECT_FALSE(r.diag.has_errors());
    const auto toks = without_eof(r.tokens);
    EXPECT_EQ(kinds_of(toks), (std::vector<token_kind>{
        token_kind::kw_import,     token_kind::kw_struct,  token_kind::kw_class,
        token_kind::kw_open,       token_kind::kw_enum,    token_kind::kw_interface,
        token_kind::kw_impl,       token_kind::kw_virtual, token_kind::kw_override,
        token_kind::kw_mut,        token_kind::kw_private, token_kind::kw_static,
        token_kind::kw_compile_time, token_kind::kw_run_time, token_kind::kw_deduce,
        token_kind::kw_if,   token_kind::kw_else,   token_kind::kw_match,
        token_kind::kw_for,  token_kind::kw_in,     token_kind::kw_return,
        token_kind::kw_true, token_kind::kw_false,  token_kind::kw_null,
        token_kind::kw_cast, token_kind::kw_unsafe, token_kind::kw_auto,
        token_kind::kw_default, token_kind::kw_bit,
    }));
}

TEST(lexer_primitives) {
    auto r = lex("u8 u16 u32 u64 u128 i8 i16 i32 i64 i128 f32 f64 bool void");
    EXPECT_FALSE(r.diag.has_errors());
    EXPECT_EQ(kinds_of(without_eof(r.tokens)), (std::vector<token_kind>{
        token_kind::prim_u8,  token_kind::prim_u16,  token_kind::prim_u32,
        token_kind::prim_u64, token_kind::prim_u128, token_kind::prim_i8,
        token_kind::prim_i16, token_kind::prim_i32,  token_kind::prim_i64,
        token_kind::prim_i128, token_kind::prim_f32, token_kind::prim_f64,
        token_kind::prim_bool, token_kind::prim_void,
    }));
}

TEST(lexer_punctuation) {
    auto r = lex("{ } ( ) [ ] ; , . .. ..= : :: -> => _");
    EXPECT_FALSE(r.diag.has_errors());
    EXPECT_EQ(kinds_of(without_eof(r.tokens)), (std::vector<token_kind>{
        token_kind::l_brace, token_kind::r_brace,
        token_kind::l_paren, token_kind::r_paren,
        token_kind::l_bracket, token_kind::r_bracket,
        token_kind::semicolon, token_kind::comma,
        token_kind::dot, token_kind::dot_dot, token_kind::dot_dot_eq,
        token_kind::colon, token_kind::double_colon,
        token_kind::arrow, token_kind::fat_arrow,
        token_kind::underscore,
    }));
}

TEST(lexer_operators) {
    auto r = lex("+ - * / % == != < <= > >= && || ! & | ^ ~ << >> "
                 "= += -= *= /= %= &= |= ^= <<= >>= ?");
    EXPECT_FALSE(r.diag.has_errors());
    EXPECT_EQ(kinds_of(without_eof(r.tokens)), (std::vector<token_kind>{
        token_kind::plus, token_kind::minus, token_kind::star,
        token_kind::slash, token_kind::percent,
        token_kind::eq_eq, token_kind::bang_eq,
        token_kind::lt, token_kind::lt_eq, token_kind::gt, token_kind::gt_eq,
        token_kind::amp_amp, token_kind::pipe_pipe, token_kind::bang,
        token_kind::amp, token_kind::pipe, token_kind::caret, token_kind::tilde,
        token_kind::lt_lt, token_kind::gt_gt,
        token_kind::eq,
        token_kind::plus_eq, token_kind::minus_eq, token_kind::star_eq,
        token_kind::slash_eq, token_kind::percent_eq,
        token_kind::amp_eq, token_kind::pipe_eq, token_kind::caret_eq,
        token_kind::lt_lt_eq, token_kind::gt_gt_eq,
        token_kind::question,
    }));
}

TEST(lexer_longest_match) {
    auto r = lex("<<= << < <= << >>= >> > >= >>>");
    EXPECT_FALSE(r.diag.has_errors());
    EXPECT_EQ(kinds_of(without_eof(r.tokens)), (std::vector<token_kind>{
        token_kind::lt_lt_eq, token_kind::lt_lt, token_kind::lt,
        token_kind::lt_eq, token_kind::lt_lt,
        token_kind::gt_gt_eq, token_kind::gt_gt, token_kind::gt,
        token_kind::gt_eq,
        // `>>>` is not a token; it's `>>` followed by `>`
        token_kind::gt_gt, token_kind::gt,
    }));
}

TEST(lexer_line_comment) {
    auto r = lex("u32 // this is a comment\nx");
    EXPECT_FALSE(r.diag.has_errors());
    EXPECT_EQ(kinds_of(without_eof(r.tokens)), (std::vector<token_kind>{ token_kind::prim_u32, token_kind::identifier }));
}

TEST(lexer_block_comment) {
    auto r = lex("a /* one\ntwo */ b");
    EXPECT_FALSE(r.diag.has_errors());
    EXPECT_EQ(kinds_of(without_eof(r.tokens)), (std::vector<token_kind>{ token_kind::identifier, token_kind::identifier }));
}

TEST(lexer_nested_block_comment) {
    auto r = lex("a /* outer /* inner */ still outer */ b");
    EXPECT_FALSE(r.diag.has_errors());
    EXPECT_EQ(kinds_of(without_eof(r.tokens)), (std::vector<token_kind>{ token_kind::identifier, token_kind::identifier }));
}

TEST(lexer_unterminated_block_comment) {
    auto r = lex("a /* no end");
    EXPECT_TRUE(r.diag.has_errors());
    EXPECT_EQ(r.diag.diagnostics()[0].code, diagnostic_code::unterminated_block_comment);
}

TEST(lexer_string_basic) {
    auto r = lex(R"("hello world")");
    EXPECT_FALSE(r.diag.has_errors());
    EXPECT_EQ(r.tokens[0].kind, token_kind::string_literal);
    EXPECT_EQ(r.tokens[0].lexeme, "\"hello world\"");
}

TEST(lexer_string_escapes) {
    auto r = lex(R"("a\n\t\\\"\'\0\x41\u{1F600}")");
    EXPECT_FALSE(r.diag.has_errors());
    EXPECT_EQ(r.tokens[0].kind, token_kind::string_literal);
}

TEST(lexer_string_unterminated) {
    auto r = lex(R"("hello)");
    EXPECT_TRUE(r.diag.has_errors());
    EXPECT_EQ(r.diag.diagnostics()[0].code, diagnostic_code::unterminated_string);
}

TEST(lexer_string_bad_escape) {
    auto r = lex(R"("a\q")");
    EXPECT_TRUE(r.diag.has_errors());
    EXPECT_EQ(r.diag.diagnostics()[0].code, diagnostic_code::invalid_escape);
}

TEST(lexer_char_basic) {
    auto r = lex("'a' 'z'");
    EXPECT_FALSE(r.diag.has_errors());
    EXPECT_EQ(kinds_of(without_eof(r.tokens)), (std::vector<token_kind>{token_kind::char_literal, token_kind::char_literal }));
}

TEST(lexer_char_escape) {
    auto r = lex(R"('\n' '\'' '\\' '\0' '\x41')");
    EXPECT_FALSE(r.diag.has_errors());
    EXPECT_EQ(kinds_of(without_eof(r.tokens)), (std::vector<token_kind>{
        token_kind::char_literal, token_kind::char_literal,
        token_kind::char_literal, token_kind::char_literal,
        token_kind::char_literal,
    }));
}

TEST(lexer_char_empty) {
    auto r = lex("''");
    EXPECT_TRUE(r.diag.has_errors());
    EXPECT_EQ(r.diag.diagnostics()[0].code, diagnostic_code::empty_char_literal);
}

TEST(lexer_char_multiple) {
    auto r = lex("'ab'");
    EXPECT_TRUE(r.diag.has_errors());
    EXPECT_EQ(r.diag.diagnostics()[0].code, diagnostic_code::multi_char_char_literal);
}

TEST(lexer_int_suffixes) {
    auto r = lex("0u8 42u16 7u32 9u64 1u128 0i8 42i16 7i32 9i64 1i128");
    EXPECT_FALSE(r.diag.has_errors());
    EXPECT_EQ(kinds_of(without_eof(r.tokens)), std::vector<token_kind>(10, token_kind::integer_literal));
}

TEST(lexer_int_no_suffix) {
    auto r = lex("0 42 1000000 1_000_000");
    EXPECT_FALSE(r.diag.has_errors());
    EXPECT_EQ(kinds_of(without_eof(r.tokens)), std::vector<token_kind>(4, token_kind::integer_literal));
}

TEST(lexer_int_hex_bin_oct) {
    auto r = lex("0xFF 0xdead_beef 0b1010 0o777 0xFFu32 0b1i8");
    EXPECT_FALSE(r.diag.has_errors());
    EXPECT_EQ(kinds_of(without_eof(r.tokens)), std::vector<token_kind>(6, token_kind::integer_literal));
}

TEST(lexer_int_bad_suffix) {
    auto r = lex("1u7");
    EXPECT_TRUE(r.diag.has_errors());
    EXPECT_EQ(r.diag.diagnostics()[0].code, diagnostic_code::invalid_integer_suffix);
}

TEST(lexer_hex_missing_digits) {
    auto r = lex("0x");
    EXPECT_TRUE(r.diag.has_errors());
    EXPECT_EQ(r.diag.diagnostics()[0].code, diagnostic_code::missing_digits_after_prefix);
}

TEST(lexer_float_basic) {
    auto r = lex("3.14 0.5 100.0");
    EXPECT_FALSE(r.diag.has_errors());
    EXPECT_EQ(kinds_of(without_eof(r.tokens)), std::vector<token_kind>(3, token_kind::float_literal));
}

TEST(lexer_float_exponent) {
    auto r = lex("1e10 1.5e-3 2E+8");
    EXPECT_FALSE(r.diag.has_errors());
    EXPECT_EQ(kinds_of(without_eof(r.tokens)), std::vector<token_kind>(3, token_kind::float_literal));
}

TEST(lexer_float_suffix) {
    auto r = lex("1.0f32 3.14f64 1f32 0f64");
    EXPECT_FALSE(r.diag.has_errors());
    EXPECT_EQ(kinds_of(without_eof(r.tokens)), std::vector<token_kind>(4, token_kind::float_literal));
}

TEST(lexer_float_bad_int_suffix) {
    auto r = lex("1.0u32");
    EXPECT_TRUE(r.diag.has_errors());
    EXPECT_EQ(r.diag.diagnostics()[0].code, diagnostic_code::invalid_integer_suffix);
}

TEST(lexer_range_not_float) {
    // `0..=9` MUST lex as `0`, `..=`, `9`, not `0.` + `.=9`.
    auto r = lex("0..=9");
    EXPECT_FALSE(r.diag.has_errors());
    EXPECT_EQ(kinds_of(without_eof(r.tokens)), (std::vector<token_kind>{
        token_kind::integer_literal,
        token_kind::dot_dot_eq,
        token_kind::integer_literal,
    }));
}

TEST(lexer_int_then_dot) {
    // `1.foo()` MUST NOT lex `1.` as a float. `.` requires a following digit.
    auto r = lex("1.foo");
    EXPECT_FALSE(r.diag.has_errors());
    EXPECT_EQ(kinds_of(without_eof(r.tokens)), (std::vector<token_kind>{
        token_kind::integer_literal,
        token_kind::dot,
        token_kind::identifier,
    }));
}

TEST(lexer_underscore_wildcard) {
    auto r = lex("_");
    EXPECT_FALSE(r.diag.has_errors());
    EXPECT_EQ(r.tokens[0].kind, token_kind::underscore);
}

TEST(lexer_underscore_identifiers) {
    auto r = lex("_foo _1 _bar_baz");
    EXPECT_FALSE(r.diag.has_errors());
    EXPECT_EQ(kinds_of(without_eof(r.tokens)), std::vector<token_kind>(3, token_kind::identifier));
}

TEST(lexer_eof_position) {
    auto r = lex("a");
    EXPECT_EQ(r.tokens.size(), 2u);
    EXPECT_EQ(r.tokens.back().kind, token_kind::end_of_file);
    EXPECT_EQ(r.tokens.back().span.begin, 1u);
    EXPECT_EQ(r.tokens.back().span.end, 1u);
}

TEST(lexer_unexpected_character) {
    auto r = lex("a $ b");
    EXPECT_TRUE(r.diag.has_errors());
    EXPECT_EQ(r.diag.diagnostics()[0].code, diagnostic_code::unexpected_character);
}

int main() {
    return test::run_all();
}

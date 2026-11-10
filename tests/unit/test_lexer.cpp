#include "rift/lexer.hpp"
#include "rift_test.hpp"

#include <string>
#include <vector>

using namespace rift;
using namespace std::string_literals;

namespace {

struct Lexed {
    SourceManager      sources;
    DiagnosticEngine   diags;
    std::vector<Token> tokens;

    explicit Lexed(std::string text) {
        FileId id = sources.add("t.rifts", std::move(text));
        tokens    = tokenize(sources.file(id), id, diags);
    }

    /// Codes of all diagnostics, space separated: "E1003 W1001".
    std::string codes() const {
        std::string out;
        for (const Diagnostic& d : diags.all()) {
            out += (out.empty() ? "" : " ") + std::string(code_name(d.code));
        }
        return out;
    }

    const Token& first() const { return tokens.at(0); }
};

/// Token kinds without the trailing eof, space separated: "kw_u32 identifier eq".
std::string kinds(std::string text) {
    Lexed       l(std::move(text));
    std::string out;
    for (const Token& t : l.tokens) {
        if (!t.is(TokenKind::eof)) {
            out += (out.empty() ? "" : " ") + std::string(token_kind_name(t.kind));
        }
    }
    return out;
}

} // namespace

// --- basics -------------------------------------------------------------------------------

TEST_CASE("lexer: empty input is just eof") {
    Lexed l("");
    REQUIRE_EQ(l.tokens.size(), 1u);
    CHECK_EQ(l.first().kind, TokenKind::eof);
    CHECK_EQ(l.codes(), std::string());
}

TEST_CASE("lexer: next() keeps returning eof") {
    SourceManager    sm;
    DiagnosticEngine diags;
    FileId           id = sm.add("t.rifts", "x");
    Lexer            lexer(sm.file(id), id, diags);
    CHECK_EQ(lexer.next().kind, TokenKind::identifier);
    CHECK_EQ(lexer.next().kind, TokenKind::eof);
    CHECK_EQ(lexer.next().kind, TokenKind::eof);
}

TEST_CASE("lexer: a simple declaration") {
    CHECK_EQ(kinds("mut u32 count = 0;"), std::string("kw_mut kw_u32 identifier eq int_literal semicolon"));
}

TEST_CASE("lexer: token spans and text") {
    Lexed l("  foo");
    CHECK_EQ(l.first().span.begin, 2u);
    CHECK_EQ(l.first().span.end, 5u);
    CHECK_EQ(l.first().text, std::string_view("foo"));
}

TEST_CASE("lexer: byte order mark is skipped") {
    CHECK_EQ(kinds("\xEF\xBB\xBFu32"), std::string("kw_u32"));
}

// --- identifiers and keywords -------------------------------------------------------------

TEST_CASE("lexer: every keyword and built-in type is recognized") {
    for (std::string_view word : {"import", "struct", "class", "interface", "enum", "template",
                                  "type", "open", "virtual", "override", "abstract", "operator",
                                  "public", "protected", "private", "static", "extern", "mut",
                                  "move", "this", "view", "compile_time", "run_time", "deduce",
                                  "if", "else", "for", "while", "break", "continue", "return",
                                  "match", "true", "false", "nullptr", "default", "auto", "cast",
                                  "bit", "unsafe", "bool", "char", "void", "never", "i8", "i16",
                                  "i32", "i64", "i128", "isize", "u8", "u16", "u32", "u64",
                                  "u128", "usize", "f32", "f64"}) {
        Lexed l{std::string(word)};
        CHECK(is_keyword(l.first().kind));
        CHECK_EQ(token_kind_spelling(l.first().kind), word);
    }
}

TEST_CASE("lexer: removed keywords are plain identifiers") {
    // Spec 01: const, fn, consteval, impl, in, null, runtime were removed.
    CHECK_EQ(kinds("const fn consteval impl in null runtime"),
             std::string("identifier identifier identifier identifier identifier identifier identifier"));
}

TEST_CASE("lexer: underscore alone is the wildcard") {
    CHECK_EQ(kinds("_ _x x_ __"), std::string("underscore identifier identifier identifier"));
}

TEST_CASE("lexer: keyword prefixes stay identifiers") {
    CHECK_EQ(kinds("iff u32x mutable"), std::string("identifier identifier identifier"));
}

// --- comments -----------------------------------------------------------------------------

TEST_CASE("lexer: line and block comments are skipped") {
    CHECK_EQ(kinds("a // comment\n/* block\n comment */ b"), std::string("identifier identifier"));
}

TEST_CASE("lexer: doc comments become tokens") {
    Lexed l("/// adds one\nu32 f();");
    CHECK_EQ(l.first().kind, TokenKind::doc_comment);
    CHECK_EQ(l.first().value, std::string(" adds one"));
    CHECK_EQ(kinds("//// not a doc comment\nx"), std::string("identifier"));
}

TEST_CASE("lexer: unterminated block comment") {
    Lexed l("x /* never closed");
    CHECK_EQ(l.codes(), std::string("E1002"));
}

TEST_CASE("lexer: block comments do not nest (warning)") {
    Lexed l("/* a /* b */ x */");
    CHECK_EQ(l.codes(), std::string("W1001"));
    CHECK_EQ(kinds("/* a /* b */ x */"), std::string("identifier star slash"));
}

// --- integer literals ---------------------------------------------------------------------

TEST_CASE("lexer: integer literal values in every base") {
    struct Case { const char* text; std::uint64_t value; };
    for (Case c : {Case{"42", 42}, Case{"0xFF", 255}, Case{"0b1010", 10}, Case{"0o17", 15},
                   Case{"1_000_000", 1000000}, Case{"0", 0}, Case{"0x_ff_ff", 0xFFFF}}) {
        Lexed l(c.text);
        CHECK_EQ(l.first().kind, TokenKind::int_literal);
        CHECK_EQ(to_string(l.first().int_value), std::to_string(c.value));
        CHECK_EQ(l.codes(), std::string());
    }
}

TEST_CASE("lexer: integer suffixes") {
    Lexed l("42u32 0xFFu8 7usize");
    CHECK_EQ(l.tokens[0].suffix, std::string_view("u32"));
    CHECK_EQ(l.tokens[1].suffix, std::string_view("u8"));
    CHECK_EQ(to_string(l.tokens[1].int_value), std::string("255"));
    CHECK_EQ(l.tokens[2].suffix, std::string_view("usize"));
    CHECK_EQ(l.codes(), std::string());
}

TEST_CASE("lexer: largest u128 fits, one more does not") {
    Lexed ok("340282366920938463463374607431768211455");
    CHECK_EQ(ok.codes(), std::string());
    CHECK_EQ(to_string(ok.first().int_value), std::string("340282366920938463463374607431768211455"));
    Lexed too_big("340282366920938463463374607431768211456");
    CHECK_EQ(too_big.codes(), std::string("E1012"));
}

TEST_CASE("lexer: integer literal errors") {
    CHECK_EQ(Lexed("0x").codes(), std::string("E1009"));
    CHECK_EQ(Lexed("0b102").codes(), std::string("E1010"));
    CHECK_EQ(Lexed("0o78").codes(), std::string("E1010"));
    CHECK_EQ(Lexed("42u7").codes(), std::string("E1011"));
    CHECK_EQ(Lexed("1f32").codes(), std::string("E1013"));
}

// --- float literals -----------------------------------------------------------------------

TEST_CASE("lexer: float literals") {
    for (const char* text : {"1.0", "0.5", "1e-3", "2.5e10", "1E+3", "1.0f32", "3.14f64", "1_000.5"}) {
        Lexed l(text);
        CHECK_EQ(l.first().kind, TokenKind::float_literal);
        CHECK_EQ(l.first().text, std::string_view(text));
        CHECK_EQ(l.codes(), std::string());
    }
}

TEST_CASE("lexer: float literal errors") {
    CHECK_EQ(Lexed("1.0u32").codes(), std::string("E1011"));
    CHECK_EQ(Lexed("1e").codes(), std::string("E1014"));
    CHECK_EQ(Lexed("1e+").codes(), std::string("E1014"));
}

TEST_CASE("lexer: ranges and method calls are not floats") {
    CHECK_EQ(kinds("0..10"), std::string("int_literal dot_dot int_literal"));
    CHECK_EQ(kinds("0..=255"), std::string("int_literal dot_dot_eq int_literal"));
    CHECK_EQ(kinds("5.abs()"), std::string("int_literal dot identifier l_paren r_paren"));
}

// --- char and string literals -------------------------------------------------------------

TEST_CASE("lexer: char literals decode") {
    CHECK_EQ(Lexed("'a'").first().value, std::string("a"));
    CHECK_EQ(Lexed("'\\n'").first().value, std::string("\n"));
    CHECK_EQ(Lexed("'\\u{1F600}'").first().value, std::string("😀"));
    CHECK_EQ(Lexed("'é'").first().value, std::string("é"));
    CHECK_EQ(Lexed("'\\''").first().value, std::string("'"));
}

TEST_CASE("lexer: char literal errors") {
    CHECK_EQ(Lexed("''").codes(), std::string("E1005"));
    CHECK_EQ(Lexed("'ab'").codes(), std::string("E1006"));
    CHECK_EQ(Lexed("'a").codes(), std::string("E1004"));
    CHECK_EQ(Lexed("'\\q'").codes(), std::string("E1007"));
}

TEST_CASE("lexer: string escapes") {
    Lexed l(R"("tab\there\n\"q\" \\ \0 \u{48}")");
    CHECK_EQ(l.first().kind, TokenKind::string_literal);
    CHECK_EQ(l.first().value, "tab\there\n\"q\" \\ \0 H"s);
    CHECK_EQ(l.codes(), std::string());
}

TEST_CASE("lexer: plain strings are never interpolated") {
    Lexed l(R"("{x}")");
    CHECK_EQ(l.first().kind, TokenKind::string_literal);
    CHECK_EQ(l.first().value, std::string("{x}"));
}

TEST_CASE("lexer: raw strings keep backslashes") {
    Lexed l(R"(r"C:\path\no\escapes")");
    CHECK_EQ(l.first().kind, TokenKind::raw_string_literal);
    CHECK_EQ(l.first().value, std::string(R"(C:\path\no\escapes)"));
    CHECK_EQ(l.codes(), std::string());
}

TEST_CASE("lexer: format strings") {
    Lexed l(R"(f"x = {x}\n")");
    CHECK_EQ(l.first().kind, TokenKind::format_string_literal);
    CHECK_EQ(l.first().value, std::string("x = {x}\n"));
}

TEST_CASE("lexer: r and f are identifiers when not followed by a quote") {
    CHECK_EQ(kinds("r f rf"), std::string("identifier identifier identifier"));
}

TEST_CASE("lexer: string literal errors") {
    CHECK_EQ(Lexed("\"abc").codes(), std::string("E1003"));
    CHECK_EQ(Lexed("\"abc\ndef\"").codes(), std::string("E1003 E1003"));
    CHECK_EQ(Lexed(R"("\x41")").codes(), std::string("E1007"));
    CHECK_EQ(Lexed(R"("\u{D800}")").codes(), std::string("E1008"));
    CHECK_EQ(Lexed(R"("\u{110000}")").codes(), std::string("E1008"));
    CHECK_EQ(Lexed(R"("\u41")").codes(), std::string("E1008"));
    CHECK_EQ(Lexed(R"("\u{}")").codes(), std::string("E1008"));
}

// --- punctuation --------------------------------------------------------------------------

TEST_CASE("lexer: longest punctuation wins") {
    CHECK_EQ(kinds("<=> <<= >>= ..= +%= -%= *%="),
             std::string("spaceship shl_eq shr_eq dot_dot_eq plus_percent_eq minus_percent_eq star_percent_eq"));
    CHECK_EQ(kinds(":: -> => +% -% *% && || == != <= >="),
             std::string("colon_colon arrow fat_arrow plus_percent minus_percent star_percent amp_amp "
                         "pipe_pipe eq_eq bang_eq less_eq greater_eq"));
}

TEST_CASE("lexer: no ++ or -- tokens") {
    CHECK_EQ(kinds("x++"), std::string("identifier plus plus"));
}

TEST_CASE("lexer: >> is one token (parser splits it for templates)") {
    CHECK_EQ(kinds("a<b<c>>"), std::string("identifier less identifier less identifier shr"));
}

// --- invalid input ------------------------------------------------------------------------

TEST_CASE("lexer: invalid characters are reported and skipped") {
    Lexed l("a $ b");
    CHECK_EQ(l.codes(), std::string("E1001"));
    CHECK_EQ(kinds("a $ b"), std::string("identifier error identifier"));
}

TEST_CASE("lexer: non-ASCII identifiers are rejected") {
    CHECK_EQ(Lexed("é").codes(), std::string("E1001"));
}

TEST_CASE("lexer: invalid UTF-8 is reported once per run") {
    CHECK_EQ(Lexed("a \xFF\xFE b").codes(), std::string("E1015"));
    CHECK_EQ(Lexed("// \xFF\n").codes(), std::string("E1015"));
    CHECK_EQ(Lexed("\"\xFF\"").codes(), std::string("E1015"));
}

TEST_CASE("lexer: keeps going after errors") {
    Lexed l("u32 a = 0x; u32 b = 'xy'; u32 c = 1;");
    CHECK_EQ(l.codes(), std::string("E1009 E1006"));
    CHECK_EQ(l.tokens.size(), 16u);   // 15 tokens + eof
}

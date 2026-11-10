#include "rift/lexer.hpp"

#include <array>
#include <format>

namespace rift {

namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_ident_start(char c) { return is_alpha(c) || c == '_'; }
bool is_ident_continue(char c) { return is_ident_start(c) || is_digit(c); }
bool is_hex_digit(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

unsigned digit_value(char c) {
    if (is_digit(c)) {
        return static_cast<unsigned>(c - '0');
    }
    if (c >= 'a' && c <= 'z') {
        return static_cast<unsigned>(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'Z') {
        return static_cast<unsigned>(c - 'A' + 10);
    }
    return 99;
}

bool is_integer_type_name(std::string_view s) {
    constexpr std::array names = {"i8", "i16", "i32", "i64", "i128", "isize",
                                  "u8", "u16", "u32", "u64", "u128", "usize"};
    for (std::string_view n : names) {
        if (n == s) {
            return true;
        }
    }
    return false;
}

bool is_float_type_name(std::string_view s) { return s == "f32" || s == "f64"; }

struct Punct {
    std::string_view spelling;
    TokenKind        kind;
};

constexpr std::array punctuation = {
#define RIFT_TOKEN(name, spelling)
#define RIFT_KEYWORD(word)
#define RIFT_PUNCT(name, spelling) Punct{spelling, TokenKind::name},
#include "rift/token_kinds.def"
};

std::string describe_char(char32_t cp) {
    std::string printable;
    encode_utf8(cp, printable);
    if (cp < 0x20 || cp == 0x7F) {
        return std::format("U+{:04X}", static_cast<std::uint32_t>(cp));
    }
    return std::format("'{}' (U+{:04X})", printable, static_cast<std::uint32_t>(cp));
}

} // namespace

Lexer::Lexer(const SourceFile& file, FileId id, DiagnosticEngine& diags)
    : file_(file), id_(id), text_(file.text()), diags_(diags) {
    if (text_.starts_with("\xEF\xBB\xBF")) {   // UTF-8 byte order mark
        pos_ = 3;
    }
}

bool Lexer::at_end(std::size_t ahead) const { return pos_ + ahead >= text_.size(); }
char Lexer::peek(std::size_t ahead) const { return at_end(ahead) ? '\0' : text_[pos_ + ahead]; }

Span Lexer::span(std::size_t begin, std::size_t end) const {
    return Span{id_, static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(end)};
}

Token Lexer::make(TokenKind kind, std::size_t begin) const {
    Token token;
    token.kind = kind;
    token.span = span(begin, pos_);
    token.text = text_.substr(begin, pos_ - begin);
    return token;
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    do {
        tokens.push_back(next());
    } while (!tokens.back().is(TokenKind::eof));
    return tokens;
}

std::vector<Token> tokenize(const SourceFile& file, FileId id, DiagnosticEngine& diags) {
    return Lexer(file, id, diags).tokenize();
}

// --- trivia ---------------------------------------------------------------------------------

void Lexer::check_utf8_at_cursor() {
    char32_t    cp  = 0;
    std::size_t len = decode_utf8(text_, pos_, cp);
    if (len == 0) {
        diags_.report(DiagCode::E1015, span(pos_, pos_ + 1), "invalid UTF-8 byte in source",
                      "not valid UTF-8");
        ++pos_;
    } else {
        pos_ += len;
    }
}

bool Lexer::skip_trivia(Token& doc_comment) {
    while (!at_end()) {
        char c = peek();
        if (is_space(c)) {
            ++pos_;
        } else if (c == '/' && peek(1) == '/') {
            std::size_t begin = pos_;
            bool is_doc = peek(2) == '/' && peek(3) != '/';
            while (!at_end() && peek() != '\n') {
                if (static_cast<unsigned char>(peek()) >= 0x80) {
                    check_utf8_at_cursor();
                } else {
                    ++pos_;
                }
            }
            if (is_doc) {
                doc_comment = make(TokenKind::doc_comment, begin);
                std::string_view body = doc_comment.text.substr(3);
                if (body.ends_with('\r')) {
                    body.remove_suffix(1);
                }
                doc_comment.value = std::string(body);
                return true;
            }
        } else if (c == '/' && peek(1) == '*') {
            skip_block_comment();
        } else {
            break;
        }
    }
    return false;
}

void Lexer::skip_block_comment() {
    std::size_t begin = pos_;
    pos_ += 2;
    while (!at_end()) {
        if (peek() == '*' && peek(1) == '/') {
            pos_ += 2;
            return;
        }
        if (peek() == '/' && peek(1) == '*') {
            diags_.report(DiagCode::W1001, span(pos_, pos_ + 2),
                          "'/*' inside a block comment; block comments do not nest",
                          "this does not start a nested comment")
                .notes.push_back(Note{std::format("the comment that starts at {}:{} ends at the first '*/'",
                                                  file_.line_col(static_cast<std::uint32_t>(begin)).line,
                                                  file_.line_col(static_cast<std::uint32_t>(begin)).column),
                                      std::nullopt});
            pos_ += 2;
            continue;
        }
        if (static_cast<unsigned char>(peek()) >= 0x80) {
            check_utf8_at_cursor();
        } else {
            ++pos_;
        }
    }
    diags_.report(DiagCode::E1002, span(begin, begin + 2), "unterminated block comment",
                  "comment starts here; no closing '*/' before end of file");
}

// --- main dispatch --------------------------------------------------------------------------

Token Lexer::next() {
    Token doc;
    if (skip_trivia(doc)) {
        return doc;
    }
    std::size_t begin = pos_;
    if (at_end()) {
        return make(TokenKind::eof, begin);
    }

    char c = peek();
    if (c == 'r' && peek(1) == '"') {
        return lex_string(begin, TokenKind::raw_string_literal);
    }
    if (c == 'f' && peek(1) == '"') {
        return lex_string(begin, TokenKind::format_string_literal);
    }
    if (is_ident_start(c)) {
        return lex_identifier(begin);
    }
    if (is_digit(c)) {
        return lex_number(begin);
    }
    if (c == '\'') {
        return lex_char(begin);
    }
    if (c == '"') {
        return lex_string(begin, TokenKind::string_literal);
    }
    return lex_punctuation(begin);
}

// --- identifiers ----------------------------------------------------------------------------

Token Lexer::lex_identifier(std::size_t begin) {
    while (is_ident_continue(peek())) {
        ++pos_;
    }
    std::string_view word = text_.substr(begin, pos_ - begin);
    if (word == "_") {
        return make(TokenKind::underscore, begin);
    }
    return make(keyword_kind(word).value_or(TokenKind::identifier), begin);
}

// --- numbers --------------------------------------------------------------------------------

Token Lexer::lex_number(std::size_t begin) {
    unsigned radix = 10;
    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'b' || peek(1) == 'o')) {
        radix = peek(1) == 'x' ? 16 : peek(1) == 'b' ? 2 : 8;
        pos_ += 2;
    }

    // Digits. For binary/octal we also swallow 0-9 so `0b102` reports the '2' instead of
    // splitting into two tokens.
    std::size_t digit_count  = 0;
    u128        value        = 0;
    bool        overflow     = false;
    bool        bad_digit    = false;
    auto        accepts      = [&](char ch) { return radix == 16 ? is_hex_digit(ch) : is_digit(ch); };
    while (accepts(peek()) || peek() == '_') {
        char ch = peek();
        if (ch != '_') {
            unsigned d = digit_value(ch);
            if (d >= radix) {
                if (!bad_digit) {
                    diags_.report(DiagCode::E1010, span(pos_, pos_ + 1),
                                  std::format("invalid digit '{}' in base-{} literal", ch, radix),
                                  "not a valid digit");
                }
                bad_digit = true;
            } else {
                constexpr u128 max = ~u128{0};
                if (value > (max - d) / radix) {
                    overflow = true;
                }
                value = value * radix + d;
            }
            ++digit_count;
        }
        ++pos_;
    }

    if (radix != 10 && digit_count == 0) {
        diags_.report(DiagCode::E1009, span(begin, pos_),
                      std::format("expected {} digits after '{}'",
                                  radix == 16 ? "hexadecimal" : radix == 2 ? "binary" : "octal",
                                  text_.substr(begin, 2)),
                      "no digits");
    }

    // Fraction and exponent (decimal only).
    bool is_float = false;
    if (radix == 10) {
        if (peek() == '.' && is_digit(peek(1))) {
            is_float = true;
            ++pos_;
            while (is_digit(peek()) || peek() == '_') {
                ++pos_;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            char after = peek(1);
            if (is_digit(after) || ((after == '+' || after == '-') && is_digit(peek(2)))) {
                is_float = true;
                pos_ += (after == '+' || after == '-') ? 2 : 1;
                while (is_digit(peek()) || peek() == '_') {
                    ++pos_;
                }
            } else if (after == '+' || after == '-' || !is_ident_continue(after)) {
                std::size_t e_begin = pos_;
                pos_ += (after == '+' || after == '-') ? 2 : 1;
                diags_.report(DiagCode::E1014, span(e_begin, pos_),
                              "expected digits after the exponent marker", "exponent has no digits");
                Token t = make(TokenKind::float_literal, begin);
                return t;
            }
        }
    }

    // Suffix.
    std::string_view suffix;
    if (is_ident_start(peek())) {
        std::size_t suffix_begin = pos_;
        while (is_ident_continue(peek())) {
            ++pos_;
        }
        suffix = text_.substr(suffix_begin, pos_ - suffix_begin);
        Span suffix_span = span(suffix_begin, pos_);
        if (is_float) {
            if (!is_float_type_name(suffix)) {
                diags_.report(DiagCode::E1011, suffix_span,
                              std::format("invalid suffix '{}' on float literal", suffix),
                              "expected 'f32' or 'f64'");
            }
        } else if (is_float_type_name(suffix)) {
            std::string_view digits = text_.substr(begin, suffix_begin - begin);
            diags_.report(DiagCode::E1013, suffix_span,
                          std::format("integer literal has float suffix '{}'", suffix),
                          "float suffix")
                .notes.push_back(Note{
                    std::format("a float literal must contain '.' or an exponent; write {}.0{}",
                                digits, suffix),
                    std::nullopt});
        } else if (!is_integer_type_name(suffix)) {
            diags_.report(DiagCode::E1011, suffix_span,
                          std::format("invalid suffix '{}' on integer literal", suffix),
                          "expected an integer type such as 'u32'");
        }
    }

    if (is_float) {
        Token t  = make(TokenKind::float_literal, begin);
        t.suffix = suffix;
        return t;
    }
    if (overflow) {
        diags_.report(DiagCode::E1012, span(begin, pos_),
                      "integer literal does not fit in 128 bits", "too large");
    }
    Token t     = make(TokenKind::int_literal, begin);
    t.int_value = overflow ? 0 : value;
    t.suffix    = suffix;
    return t;
}

// --- escapes, chars, strings ----------------------------------------------------------------

void Lexer::lex_escape(std::string& out) {
    std::size_t begin = pos_;
    ++pos_;   // backslash
    char c = peek();
    switch (c) {
    case 'n': out.push_back('\n'); ++pos_; return;
    case 't': out.push_back('\t'); ++pos_; return;
    case 'r': out.push_back('\r'); ++pos_; return;
    case '0': out.push_back('\0'); ++pos_; return;
    case '\\': out.push_back('\\'); ++pos_; return;
    case '"': out.push_back('"'); ++pos_; return;
    case '\'': out.push_back('\''); ++pos_; return;
    case 'u': {
        ++pos_;
        if (peek() != '{') {
            diags_.report(DiagCode::E1008, span(begin, pos_),
                          "unicode escape must have the form \\u{XXXX}", "expected '{'");
            return;
        }
        ++pos_;
        char32_t    cp     = 0;
        std::size_t digits = 0;
        while (is_hex_digit(peek())) {
            cp = cp * 16 + digit_value(peek());
            ++digits;
            ++pos_;
        }
        if (peek() != '}') {
            diags_.report(DiagCode::E1008, span(begin, pos_),
                          "unicode escape must have the form \\u{XXXX}", "expected '}'");
            return;
        }
        ++pos_;
        if (digits == 0 || digits > 6) {
            diags_.report(DiagCode::E1008, span(begin, pos_),
                          "unicode escape must have 1 to 6 hex digits", "invalid escape");
            return;
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            diags_.report(DiagCode::E1008, span(begin, pos_),
                          std::format("U+{:X} is not a Unicode scalar value",
                                      static_cast<std::uint32_t>(cp)),
                          "invalid code point");
            return;
        }
        encode_utf8(cp, out);
        return;
    }
    default: {
        if (at_end() || c == '\n') {
            return;   // the caller reports the unterminated literal
        }
        char32_t    cp  = 0;
        std::size_t len = decode_utf8(text_, pos_, cp);
        pos_ += len == 0 ? 1 : len;
        diags_.report(DiagCode::E1007, span(begin, pos_),
                      std::format("unknown escape sequence '{}'", text_.substr(begin, pos_ - begin)),
                      "unknown escape")
            .notes.push_back(Note{"valid escapes are \\n \\t \\r \\0 \\\\ \\\" \\' and \\u{XXXX}",
                                  std::nullopt});
        return;
    }
    }
}

Token Lexer::lex_char(std::size_t begin) {
    ++pos_;   // opening quote
    if (peek() == '\'') {
        ++pos_;
        diags_.report(DiagCode::E1005, span(begin, pos_), "empty character literal",
                      "a character literal holds exactly one character");
        return make(TokenKind::error, begin);
    }
    std::string value;
    std::size_t chars = 0;
    while (!at_end() && peek() != '\'' && peek() != '\n') {
        if (peek() == '\\') {
            lex_escape(value);
        } else {
            char32_t    cp  = 0;
            std::size_t len = decode_utf8(text_, pos_, cp);
            if (len == 0) {
                check_utf8_at_cursor();
                continue;
            }
            value.append(text_.substr(pos_, len));
            pos_ += len;
        }
        ++chars;
    }
    if (peek() != '\'') {
        diags_.report(DiagCode::E1004, span(begin, pos_), "unterminated character literal",
                      "missing closing '");
        return make(TokenKind::error, begin);
    }
    ++pos_;
    if (chars > 1) {
        diags_.report(DiagCode::E1006, span(begin, pos_),
                      "character literal contains more than one character",
                      std::format("{} characters", chars))
            .notes.push_back(Note{"use a string literal \"...\" for text", std::nullopt});
        return make(TokenKind::error, begin);
    }
    Token t = make(TokenKind::char_literal, begin);
    t.value = std::move(value);
    return t;
}

Token Lexer::lex_string(std::size_t begin, TokenKind kind) {
    pos_ += kind == TokenKind::string_literal ? 1 : 2;   // `"` or `r"` / `f"`
    bool        raw = kind == TokenKind::raw_string_literal;
    std::string value;
    while (!at_end() && peek() != '"' && peek() != '\n') {
        if (peek() == '\\' && !raw) {
            lex_escape(value);
            continue;
        }
        char32_t    cp  = 0;
        std::size_t len = decode_utf8(text_, pos_, cp);
        if (len == 0) {
            check_utf8_at_cursor();
            continue;
        }
        value.append(text_.substr(pos_, len));
        pos_ += len;
    }
    if (peek() != '"') {
        std::size_t end = pos_;
        if (end > begin && text_[end - 1] == '\r') {
            --end;
        }
        diags_.report(DiagCode::E1003, span(begin, end),
                      std::format("unterminated {}", token_kind_spelling(kind)),
                      "missing closing '\"' before end of line");
        return make(TokenKind::error, begin);
    }
    ++pos_;
    Token t = make(kind, begin);
    t.value = std::move(value);
    return t;
}

// --- punctuation and garbage ----------------------------------------------------------------

Token Lexer::lex_punctuation(std::size_t begin) {
    for (std::size_t len = 3; len >= 1; --len) {
        if (at_end(len - 1)) {
            continue;
        }
        std::string_view candidate = text_.substr(pos_, len);
        for (const Punct& p : punctuation) {
            if (p.spelling == candidate) {
                pos_ += len;
                return make(p.kind, begin);
            }
        }
    }
    return lex_invalid(begin);
}

Token Lexer::lex_invalid(std::size_t begin) {
    char32_t    cp  = 0;
    std::size_t len = decode_utf8(text_, pos_, cp);
    if (len == 0) {
        // Group a run of invalid bytes into one diagnostic.
        while (!at_end() && decode_utf8(text_, pos_, cp) == 0) {
            ++pos_;
        }
        diags_.report(DiagCode::E1015, span(begin, pos_), "invalid UTF-8 in source",
                      "not valid UTF-8");
        return make(TokenKind::error, begin);
    }
    pos_ += len;
    Diagnostic& d = diags_.report(DiagCode::E1001, span(begin, pos_),
                                  std::format("invalid character {} in source", describe_char(cp)),
                                  "not part of Rift's syntax");
    if (cp >= 0x80) {
        d.notes.push_back(Note{"identifiers are ASCII letters, digits and '_'; other text belongs "
                               "in string literals or comments",
                               std::nullopt});
    }
    return make(TokenKind::error, begin);
}

} // namespace rift

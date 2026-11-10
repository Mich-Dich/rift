#pragma once

#include "rift/source.hpp"

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace rift {

enum class TokenKind : std::uint16_t {
#define RIFT_TOKEN(name, spelling) name,
#include "rift/token_kinds.def"
};

/// The enumerator name, e.g. "kw_u32", "l_paren", "int_literal".
std::string_view token_kind_name(TokenKind kind);
/// Human-facing spelling for messages, e.g. "u32", "(", "integer literal".
std::string_view token_kind_spelling(TokenKind kind);

bool is_keyword(TokenKind kind);
bool is_type_keyword(TokenKind kind);
/// Looks up a keyword or built-in type name.
std::optional<TokenKind> keyword_kind(std::string_view word);

__extension__ typedef unsigned __int128 u128;

struct Token {
    TokenKind        kind = TokenKind::eof;
    Span             span;
    std::string_view text;     // exact source text (lexeme)

    // Literal payloads, filled by the lexer:
    u128             int_value = 0;  // int_literal: value (suffix not applied, no range check)
    std::string_view suffix;         // int/float literal: "u32", "f64", ... or empty
    std::string      value;          // char/string/raw/format literal: decoded UTF-8 contents;
                                     // doc_comment: text after "///"

    [[nodiscard]] bool is(TokenKind k) const { return kind == k; }
};

std::ostream& operator<<(std::ostream& out, TokenKind kind);
std::string   to_string(u128 value);

} // namespace rift

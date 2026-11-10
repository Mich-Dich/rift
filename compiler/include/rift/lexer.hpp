#pragma once
// Lexer: turns one SourceFile into tokens (spec 01-lexical).
//
// The lexer never stops at an error. It reports the problem, produces a TokenKind::error token
// (or a best-effort literal) and carries on, so one run shows every lexical error in a file.

#include "rift/diagnostics.hpp"
#include "rift/source.hpp"
#include "rift/token.hpp"

#include <string>
#include <vector>

namespace rift {

class Lexer {
public:
    Lexer(const SourceFile& file, FileId id, DiagnosticEngine& diags);

    /// Next token. After the end of input it keeps returning `eof`.
    Token next();

    /// All tokens up to and including `eof`.
    std::vector<Token> tokenize();

private:
    [[nodiscard]] bool at_end(std::size_t ahead = 0) const;
    [[nodiscard]] char peek(std::size_t ahead = 0) const;
    [[nodiscard]] Span span(std::size_t begin, std::size_t end) const;
    [[nodiscard]] Token make(TokenKind kind, std::size_t begin) const;

    /// Skips whitespace and non-doc comments. Returns a doc_comment token if it meets one.
    bool  skip_trivia(Token& doc_comment);
    void  skip_block_comment();
    void  check_utf8_at_cursor();

    Token lex_identifier(std::size_t begin);
    Token lex_number(std::size_t begin);
    Token lex_char(std::size_t begin);
    Token lex_string(std::size_t begin, TokenKind kind);
    Token lex_punctuation(std::size_t begin);
    Token lex_invalid(std::size_t begin);

    /// Reads one escape sequence (cursor on the backslash), appending the decoded value to `out`.
    void lex_escape(std::string& out);

    const SourceFile& file_;
    FileId            id_;
    std::string_view  text_;
    DiagnosticEngine& diags_;
    std::size_t       pos_ = 0;
};

/// Convenience: lex a whole file.
std::vector<Token> tokenize(const SourceFile& file, FileId id, DiagnosticEngine& diags);

} // namespace rift

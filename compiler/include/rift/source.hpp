#pragma once
// Source files, byte spans and line/column lookup.
//
// Every position inside the compiler is a byte offset into a SourceFile. Lines and columns are
// only computed when something is shown to a human (diagnostics, token dumps).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rift {

using FileId = std::uint32_t;

/// Half-open byte range [begin, end) inside one file.
struct Span {
    FileId        file  = 0;
    std::uint32_t begin = 0;
    std::uint32_t end   = 0;

    [[nodiscard]] std::uint32_t size() const { return end - begin; }
};

/// 1-based line and column. Columns count Unicode code points, not bytes, so a caret under
/// `"é"` lines up in a terminal.
struct LineCol {
    std::uint32_t line   = 1;
    std::uint32_t column = 1;
};

class SourceFile {
public:
    SourceFile(std::string path, std::string text);

    [[nodiscard]] const std::string& path() const { return path_; }
    [[nodiscard]] std::string_view   text() const { return text_; }

    [[nodiscard]] LineCol         line_col(std::uint32_t offset) const;
    [[nodiscard]] std::uint32_t   line_count() const;
    /// Text of a 1-based line, without the trailing newline.
    [[nodiscard]] std::string_view line_text(std::uint32_t line) const;
    /// Byte offset where a 1-based line starts.
    [[nodiscard]] std::uint32_t   line_start(std::uint32_t line) const;

private:
    std::string                path_;
    std::string                text_;
    std::vector<std::uint32_t> line_starts_;
};

class SourceManager {
public:
    /// Registers in-memory text (used by unit tests and by `load`).
    FileId add(std::string path, std::string text);

    /// Reads a file from disk. On failure returns nullopt and puts a reason into `error`.
    std::optional<FileId> load(const std::string& path, std::string& error);

    [[nodiscard]] const SourceFile& file(FileId id) const { return files_.at(id); }

private:
    std::vector<SourceFile> files_;
};

/// Number of code points in `text` (invalid bytes count as one each).
std::uint32_t count_code_points(std::string_view text);

/// Decodes one UTF-8 sequence starting at `pos`. Returns its length in bytes (1-4), or 0 if the
/// bytes are not valid UTF-8 (overlong forms, surrogates and values above U+10FFFF are invalid).
std::size_t decode_utf8(std::string_view text, std::size_t pos, char32_t& out);

/// Appends the UTF-8 encoding of `cp` to `out`.
void encode_utf8(char32_t cp, std::string& out);

} // namespace rift

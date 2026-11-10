#include "rift/source.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace rift {

namespace {
std::uint32_t to_u32(std::size_t value) { return static_cast<std::uint32_t>(value); }
} // namespace

SourceFile::SourceFile(std::string path, std::string text)
    : path_(std::move(path)), text_(std::move(text)) {
    line_starts_.push_back(0);
    for (std::size_t i = 0; i < text_.size(); ++i) {
        if (text_[i] == '\n') {
            line_starts_.push_back(to_u32(i + 1));
        }
    }
}

std::uint32_t SourceFile::line_count() const { return to_u32(line_starts_.size()); }

std::uint32_t SourceFile::line_start(std::uint32_t line) const {
    return line_starts_.at(line - 1);
}

LineCol SourceFile::line_col(std::uint32_t offset) const {
    offset          = std::min(offset, to_u32(text_.size()));
    auto it         = std::upper_bound(line_starts_.begin(), line_starts_.end(), offset);
    auto line_index = static_cast<std::size_t>(it - line_starts_.begin()) - 1;
    std::uint32_t start = line_starts_[line_index];
    std::string_view before(text_.data() + start, offset - start);
    return LineCol{to_u32(line_index + 1), count_code_points(before) + 1};
}

std::string_view SourceFile::line_text(std::uint32_t line) const {
    std::uint32_t start = line_start(line);
    std::uint32_t end   = line < line_count() ? line_starts_[line] : to_u32(text_.size());
    std::string_view result(text_.data() + start, end - start);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.remove_suffix(1);
    }
    return result;
}

FileId SourceManager::add(std::string path, std::string text) {
    files_.emplace_back(std::move(path), std::move(text));
    return to_u32(files_.size() - 1);
}

std::optional<FileId> SourceManager::load(const std::string& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open file";
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) {
        error = "cannot read file";
        return std::nullopt;
    }
    return add(path, std::move(buffer).str());
}

std::uint32_t count_code_points(std::string_view text) {
    std::uint32_t count = 0;
    for (std::size_t i = 0; i < text.size();) {
        char32_t    cp  = 0;
        std::size_t len = decode_utf8(text, i, cp);
        i += len == 0 ? 1 : len;
        ++count;
    }
    return count;
}

std::size_t decode_utf8(std::string_view text, std::size_t pos, char32_t& out) {
    auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    unsigned char b0 = byte(pos);
    if (b0 < 0x80) {
        out = b0;
        return 1;
    }
    std::size_t len;
    char32_t    cp;
    char32_t    min;
    if ((b0 & 0xE0) == 0xC0) {
        len = 2, cp = b0 & 0x1Fu, min = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        len = 3, cp = b0 & 0x0Fu, min = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        len = 4, cp = b0 & 0x07u, min = 0x10000;
    } else {
        return 0;
    }
    if (pos + len > text.size()) {
        return 0;
    }
    for (std::size_t i = 1; i < len; ++i) {
        unsigned char b = byte(pos + i);
        if ((b & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (b & 0x3Fu);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0;
    }
    out = cp;
    return len;
}

void encode_utf8(char32_t cp, std::string& out) {
    auto push = [&](char32_t v) { out.push_back(static_cast<char>(static_cast<unsigned char>(v))); };
    if (cp < 0x80) {
        push(cp);
    } else if (cp < 0x800) {
        push(0xC0 | (cp >> 6));
        push(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        push(0xE0 | (cp >> 12));
        push(0x80 | ((cp >> 6) & 0x3F));
        push(0x80 | (cp & 0x3F));
    } else {
        push(0xF0 | (cp >> 18));
        push(0x80 | ((cp >> 12) & 0x3F));
        push(0x80 | ((cp >> 6) & 0x3F));
        push(0x80 | (cp & 0x3F));
    }
}

} // namespace rift

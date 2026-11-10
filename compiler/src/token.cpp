#include "rift/token.hpp"

#include <algorithm>
#include <array>
#include <ostream>

namespace rift {

namespace {

struct KindInfo {
    std::string_view name;
    std::string_view spelling;
    bool             keyword;
    bool             type_keyword;
};

constexpr std::array kind_table = {
#define RIFT_TOKEN(name, spelling) KindInfo{#name, spelling, false, false},
#define RIFT_KEYWORD(word) KindInfo{"kw_" #word, #word, true, false},
#define RIFT_TYPE_KEYWORD(word) KindInfo{"kw_" #word, #word, true, true},
#include "rift/token_kinds.def"
};

const KindInfo& info(TokenKind kind) { return kind_table.at(static_cast<std::size_t>(kind)); }

} // namespace

std::string_view token_kind_name(TokenKind kind) { return info(kind).name; }
std::string_view token_kind_spelling(TokenKind kind) { return info(kind).spelling; }
bool             is_keyword(TokenKind kind) { return info(kind).keyword; }
bool             is_type_keyword(TokenKind kind) { return info(kind).type_keyword; }

std::optional<TokenKind> keyword_kind(std::string_view word) {
    for (std::size_t i = 0; i < kind_table.size(); ++i) {
        if (kind_table[i].keyword && kind_table[i].spelling == word) {
            return static_cast<TokenKind>(i);
        }
    }
    return std::nullopt;
}

std::ostream& operator<<(std::ostream& out, TokenKind kind) { return out << token_kind_name(kind); }

std::string to_string(u128 value) {
    if (value == 0) {
        return "0";
    }
    std::string digits;
    while (value != 0) {
        digits.push_back(static_cast<char>('0' + static_cast<int>(value % 10)));
        value /= 10;
    }
    std::reverse(digits.begin(), digits.end());
    return digits;
}

} // namespace rift

#include "rift/source.hpp"
#include "rift_test.hpp"

using namespace rift;

TEST_CASE("source: line and column of offsets") {
    SourceFile f("a.rifts", "ab\ncd\n\nx");
    CHECK_EQ(f.line_count(), 4u);
    CHECK_EQ(f.line_col(0).line, 1u);
    CHECK_EQ(f.line_col(0).column, 1u);
    CHECK_EQ(f.line_col(1).column, 2u);
    CHECK_EQ(f.line_col(3).line, 2u);
    CHECK_EQ(f.line_col(3).column, 1u);
    CHECK_EQ(f.line_col(7).line, 4u);
}

TEST_CASE("source: line text strips newline and carriage return") {
    SourceFile f("a.rifts", "first\r\nsecond\nlast");
    CHECK_EQ(f.line_text(1), std::string_view("first"));
    CHECK_EQ(f.line_text(2), std::string_view("second"));
    CHECK_EQ(f.line_text(3), std::string_view("last"));
}

TEST_CASE("source: columns count code points, not bytes") {
    SourceFile f("a.rifts", "\"é\" x");   // é is 2 bytes
    CHECK_EQ(f.line_col(5).column, 5u);   // byte 5 is 'x'
}

TEST_CASE("source: offset past the end clamps") {
    SourceFile f("a.rifts", "abc");
    CHECK_EQ(f.line_col(100).column, 4u);
}

TEST_CASE("utf8: decode valid sequences") {
    char32_t cp = 0;
    CHECK_EQ(decode_utf8("A", 0, cp), 1u);
    CHECK_EQ(static_cast<std::uint32_t>(cp), 0x41u);
    CHECK_EQ(decode_utf8("é", 0, cp), 2u);
    CHECK_EQ(static_cast<std::uint32_t>(cp), 0xE9u);
    CHECK_EQ(decode_utf8("😀", 0, cp), 4u);
    CHECK_EQ(static_cast<std::uint32_t>(cp), 0x1F600u);
}

TEST_CASE("utf8: reject invalid sequences") {
    char32_t cp = 0;
    CHECK_EQ(decode_utf8("\xC3", 0, cp), 0u);           // truncated
    CHECK_EQ(decode_utf8("\xC0\xAF", 0, cp), 0u);       // overlong
    CHECK_EQ(decode_utf8("\xED\xA0\x80", 0, cp), 0u);   // surrogate
    CHECK_EQ(decode_utf8("\xFF", 0, cp), 0u);
}

TEST_CASE("utf8: encode round-trips") {
    for (char32_t cp : {char32_t{0x41}, char32_t{0xE9}, char32_t{0x20AC}, char32_t{0x1F600}}) {
        std::string s;
        encode_utf8(cp, s);
        char32_t back = 0;
        CHECK_EQ(decode_utf8(s, 0, back), s.size());
        CHECK_EQ(static_cast<std::uint32_t>(back), static_cast<std::uint32_t>(cp));
    }
}

TEST_CASE("source manager: load reports missing files") {
    SourceManager sm;
    std::string   error;
    CHECK(!sm.load("/definitely/not/here.rifts", error).has_value());
    CHECK(!error.empty());
}

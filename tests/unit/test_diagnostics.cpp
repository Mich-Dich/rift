#include "rift/diagnostics.hpp"
#include "rift_test.hpp"

#include <sstream>

using namespace rift;

namespace {

struct Fixture {
    SourceManager    sources;
    DiagnosticEngine diags;
    FileId           id;

    explicit Fixture(std::string text) : id(sources.add("t.rifts", std::move(text))) {}

    std::string render_with(DiagnosticFormat format) {
        std::ostringstream out;
        render_all(out, diags, sources, RenderOptions{format, false});
        return out.str();
    }
};

} // namespace

TEST_CASE("diagnostics: code registry lookups") {
    CHECK_EQ(code_name(DiagCode::E1003), std::string_view("E1003"));
    CHECK_EQ(code_severity(DiagCode::E1003), Severity::error);
    CHECK_EQ(code_severity(DiagCode::W1001), Severity::warning);
    CHECK(code_from_name("E1003") == DiagCode::E1003);
    CHECK(!code_from_name("E9999").has_value());
}

TEST_CASE("diagnostics: counts errors and warnings separately") {
    Fixture f("x");
    f.diags.report(DiagCode::E1001, Span{f.id, 0, 1}, "bad");
    f.diags.report(DiagCode::W1001, Span{f.id, 0, 1}, "hmm");
    f.diags.report(DiagCode::E1002, Span{f.id, 0, 1}, "bad again");
    CHECK_EQ(f.diags.error_count(), 2u);
    CHECK_EQ(f.diags.warning_count(), 1u);
    CHECK(f.diags.has_errors());
}

TEST_CASE("diagnostics: short format is one line per diagnostic") {
    Fixture f("u32 x = $;\n");
    f.diags.report(DiagCode::E1001, Span{f.id, 8, 9}, "invalid character");
    CHECK_EQ(f.render_with(DiagnosticFormat::short_form),
             std::string("t.rifts:1:9: error[E1001]: invalid character\n"));
}

TEST_CASE("diagnostics: human format shows the line with carets") {
    Fixture f("u32 x = \"abc;\n");
    f.diags.report(DiagCode::E1003, Span{f.id, 8, 13}, "unterminated string literal", "here");
    std::string expected = "error[E1003]: unterminated string literal\n"
                           " --> t.rifts:1:9\n"
                           "  |\n"
                           "1 | u32 x = \"abc;\n"
                           "  |         ^^^^^ here\n"
                           "\n"
                           "1 error, 0 warnings\n";
    CHECK_EQ(f.render_with(DiagnosticFormat::human), expected);
}

TEST_CASE("diagnostics: carets keep tabs aligned") {
    Fixture f("\tx = $;\n");
    f.diags.report(DiagCode::E1001, Span{f.id, 5, 6}, "bad");
    std::string out = f.render_with(DiagnosticFormat::human);
    CHECK(out.find("  | \t    ^\n") != std::string::npos);
}

TEST_CASE("diagnostics: notes are rendered in both formats") {
    Fixture f("abc\n");
    f.diags.report(DiagCode::E1001, Span{f.id, 1, 2}, "bad").notes.push_back(Note{"try this", std::nullopt});
    CHECK(f.render_with(DiagnosticFormat::human).find("= note: try this") != std::string::npos);
    CHECK(f.render_with(DiagnosticFormat::short_form).find("t.rifts:1:2: note: try this") != std::string::npos);
}

TEST_CASE("diagnostics: nothing is printed when there are no diagnostics") {
    Fixture f("abc\n");
    CHECK_EQ(f.render_with(DiagnosticFormat::human), std::string());
}

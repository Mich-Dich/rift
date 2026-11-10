#include "rift/driver.hpp"
#include "rift_test.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

using namespace rift;

namespace {

struct Run {
    ExitCode    code;
    std::string out;
    std::string err;
};

Run riftc(std::vector<std::string_view> args) {
    std::ostringstream out, err;
    ExitCode           code = run_driver(args, DriverStreams{out, err, false});
    return Run{code, out.str(), err.str()};
}

/// A temporary source file that is deleted again at the end of the test.
struct TempFile {
    std::filesystem::path path;
    TempFile(std::string_view name, std::string_view text)
        : path(std::filesystem::temp_directory_path() / name) {
        std::ofstream(path) << text;
    }
    ~TempFile() { std::filesystem::remove(path); }
    TempFile(const TempFile&)            = delete;
    TempFile& operator=(const TempFile&) = delete;
    std::string str() const { return path.string(); }
};

} // namespace

TEST_CASE("driver: --version") {
    Run r = riftc({"--version"});
    CHECK_EQ(r.code, ExitCode::success);
    CHECK_EQ(r.out, version_string() + "\n");
    CHECK(r.out.starts_with("riftc "));
}

TEST_CASE("driver: no arguments prints usage to stderr") {
    Run r = riftc({});
    CHECK_EQ(r.code, ExitCode::usage_error);
    CHECK(r.err.starts_with("usage:"));
}

TEST_CASE("driver: unknown command and option") {
    CHECK_EQ(riftc({"frobnicate"}).code, ExitCode::usage_error);
    CHECK_EQ(riftc({"check", "--nope", "a.rifts"}).code, ExitCode::usage_error);
    CHECK_EQ(riftc({"check"}).code, ExitCode::usage_error);
    CHECK_EQ(riftc({"check", "a.rifts", "b.rifts"}).code, ExitCode::usage_error);
}

TEST_CASE("driver: missing file and wrong extension") {
    Run missing = riftc({"check", "/no/such/file.rifts"});
    CHECK_EQ(missing.code, ExitCode::compile_error);
    CHECK(missing.err.find("E0001") != std::string::npos);
    Run wrong_ext = riftc({"check", "main.cpp"});
    CHECK_EQ(wrong_ext.code, ExitCode::compile_error);
    CHECK(wrong_ext.err.find("E0002") != std::string::npos);
}

TEST_CASE("driver: check on a clean file is silent") {
    TempFile f("rift_driver_ok.rifts", "u32 main() { return 0; }\n");
    Run      r = riftc({"check", f.str()});
    CHECK_EQ(r.code, ExitCode::success);
    CHECK_EQ(r.out, std::string());
    CHECK_EQ(r.err, std::string());
}

TEST_CASE("driver: check reports errors with exit code 1") {
    TempFile f("rift_driver_bad.rifts", "u32 x = 0x;\n");
    Run      r = riftc({"check", "--diagnostic-format=short", f.str()});
    CHECK_EQ(r.code, ExitCode::compile_error);
    CHECK_EQ(r.err, f.str() + ":1:9: error[E1009]: expected hexadecimal digits after '0x'\n");
}

TEST_CASE("driver: warnings alone do not fail the build") {
    TempFile f("rift_driver_warn.rifts", "/* a /* b */\n");
    Run      r = riftc({"check", f.str()});
    CHECK_EQ(r.code, ExitCode::success);
    CHECK(r.err.find("warning[W1001]") != std::string::npos);
}

TEST_CASE("driver: lex dumps tokens to stdout") {
    TempFile f("rift_driver_lex.rifts", "u32 x");
    Run      r = riftc({"lex", f.str()});
    CHECK_EQ(r.code, ExitCode::success);
    CHECK_EQ(r.out, std::string("1:1     kw_u32                  u32\n"
                                "1:5     identifier              x\n"
                                "1:6     eof\n"));
}

TEST_CASE("driver: --color=always adds ANSI escapes") {
    TempFile f("rift_driver_color.rifts", "$");
    CHECK(riftc({"check", "--color=always", f.str()}).err.find("\x1b[") != std::string::npos);
    CHECK(riftc({"check", "--color=never", f.str()}).err.find("\x1b[") == std::string::npos);
}

TEST_CASE("driver: explain") {
    Run one = riftc({"explain", "E1003"});
    CHECK_EQ(one.code, ExitCode::success);
    CHECK_EQ(one.out, std::string("E1003 (error): unterminated string literal\n"));
    CHECK_EQ(riftc({"explain", "E9999"}).code, ExitCode::usage_error);
    CHECK(riftc({"explain"}).out.find("W1001") != std::string::npos);
}

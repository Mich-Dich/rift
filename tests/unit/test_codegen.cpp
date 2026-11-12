#include "project.hpp"
#include "rift/driver.hpp"
#include "rift/llvm_backend.hpp"
#include "rift_test.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

using rift_test::Project;

namespace {

/// Generated LLVM IR for a one-file program, plus the diagnostic codes.
struct Generated {
    std::string ir;
    std::string codes;
};

Generated generate(const std::map<std::string, std::string>& files) {
    Project p(files);
    std::string ir = p.diags.has_errors() ? std::string() : rift::generate_llvm_ir(p.program, p.sources, p.diags);
    // Each Project lives in its own temporary directory; locations spell it out.
    for (std::size_t at; (at = ir.find(p.root.string())) != std::string::npos;) ir.replace(at, p.root.string().size(), "ROOT");
    return Generated{ir, p.codes()};
}

Generated generate(const std::string& main) { return generate({{"main.rifts", main}}); }

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

} // namespace

TEST_CASE("codegen: a program gets a C main that calls the Rift main") {
    Generated g = generate("u32 main() { return 0; }");
    CHECK_EQ(g.codes, std::string());
    CHECK(contains(g.ir, "define i32 @r_4main4main()"));
    CHECK(contains(g.ir, "define i32 @main()"));
    CHECK(contains(g.ir, "call i32 @rift_exit_status("));
}

TEST_CASE("codegen: only functions reachable from main are generated") {
    Generated g = generate("u32 used() { return 1; }\nu32 unused() { return 2; }\nu32 main() { return used(); }");
    CHECK(contains(g.ir, "@r_4main4used"));
    CHECK(!contains(g.ir, "@r_4main6unused"));
}

TEST_CASE("codegen: names are length-prefixed per module, type and overload") {
    Generated g = generate({{"main.rifts", "import geo::shapes;\nu32 main() { return geo::shapes::f(1u32) + geo::shapes::f(true); }\n"},
                            {"geo/shapes.rifth", "u32 f(u32 x);\nu32 f(bool b);\n"},
                            {"geo/shapes.rifts", "u32 f(u32 x) { return x; }\nu32 f(bool b) { return 0; }\n"}});
    CHECK_EQ(g.codes, std::string());
    CHECK(contains(g.ir, "@r_3geo6shapes1f_0("));
    CHECK(contains(g.ir, "@r_3geo6shapes1f_1("));
}

TEST_CASE("codegen: methods take their object by pointer") {
    Generated g = generate("struct c { u32 v = 0; void inc() mut { this.v += 1; } }\n"
                           "u32 main() { mut c x = c{}; x.inc(); return x.v; }");
    CHECK_EQ(g.codes, std::string());
    CHECK(contains(g.ir, "define void @r_4main1c3inc(ptr"));
    CHECK(contains(g.ir, "call void @r_4main1c3inc(ptr"));
}

TEST_CASE("codegen: integer arithmetic is checked, wrapping operators are not") {
    Generated g = generate("u32 main() { u32 a = 1; u32 b = a + 2; u32 c = a +% 2; return b - c; }");
    CHECK(contains(g.ir, "@llvm.uadd.with.overflow.i32"));
    CHECK(contains(g.ir, "@llvm.usub.with.overflow.i32"));
    CHECK(contains(g.ir, "add i32 "));
}

TEST_CASE("codegen: operands with side effects are evaluated left to right") {
    Generated g = generate("u32 f(u32 x) { return x; }\nu32 main() { return f(1) + f(2); }");
    // Both calls are emitted in order before the addition.
    std::size_t first  = g.ir.find("call i32 @r_4main1f(i32 1)");
    std::size_t second = g.ir.find("call i32 @r_4main1f(i32 2)");
    CHECK(first != std::string::npos);
    CHECK(second != std::string::npos);
    CHECK(first < second);
}

TEST_CASE("codegen: missing main and unsupported constructs are errors") {
    CHECK_EQ(generate("u32 helper() { return 0; }").codes, std::string("E6002"));
    CHECK_EQ(generate("void main() { auto f = [](u32 x) { return x; }; }").codes, std::string("E6001"));
    CHECK_EQ(generate("import std;\nvoid main() { std::string s = std::string::from(\"x\"); }").codes, std::string("E6001"));
    // Unsupported code that main never reaches is fine.
    CHECK_EQ(generate("void unused() { auto f = [](u32 x) { return x; }; }\nvoid main() { }").codes, std::string());
}

TEST_CASE("codegen: output is deterministic") {
    std::string src = "struct a { u32 x = 0; }\nstruct b { a inner = default; }\nenum e : u8 { one, two(u32) }\n"
                      "u32 main() { b v = b{}; e w = e::two(3); return v.inner.x; }";
    CHECK_EQ(generate(src).ir, generate(src).ir);
}

TEST_CASE("codegen: a struct type holds the types of its fields") {
    Generated g = generate("struct outer { inner i = default; }\nstruct inner { u32 x = 0; }\n"
                           "u32 main() { outer o = outer{}; return o.i.x; }");
    CHECK(contains(g.ir, "%inner = type { i32 }"));
    CHECK(contains(g.ir, "%outer = type { %inner }"));
}

// --- the driver: build and run ------------------------------------------------------------

namespace {

struct DriverRun {
    int         status;
    std::string out;
    std::string err;
};

DriverRun drive(std::vector<std::string> args) {
    std::vector<std::string_view> views(args.begin(), args.end());
    std::ostringstream            out, err;
    int status = static_cast<int>(rift::run_driver(views, rift::DriverStreams{out, err, false}));
    return DriverRun{status, out.str(), err.str()};
}

std::filesystem::path write_program(const std::string& name, const std::string& text) {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "rift_unit_codegen";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / name) << text;
    return dir / name;
}

} // namespace

TEST_CASE("driver: build writes build/<mode>/<name> next to the file") {
    std::filesystem::path src = write_program("tool.rifts", "u32 main() { return 0; }\n");
    std::filesystem::remove_all(src.parent_path() / "build");
    DriverRun r = drive({"build", src.string(), "--mode=release"});
    CHECK_EQ(r.status, 0);
    CHECK(std::filesystem::exists(src.parent_path() / "build" / "release" / "tool"));
    std::filesystem::remove_all(src.parent_path());
}

TEST_CASE("driver: run returns the program's exit status") {
    std::filesystem::path src = write_program("seven.rifts", "u32 main() { return 7; }\n");
    CHECK_EQ(drive({"run", src.string()}).status, 7);
    std::filesystem::remove_all(src.parent_path());
}

TEST_CASE("driver: emit-llvm prints the module") {
    std::filesystem::path src = write_program("tiny.rifts", "void main() { }\n");
    DriverRun r = drive({"emit-llvm", src.string()});
    CHECK_EQ(r.status, 0);
    CHECK(contains(r.out, "define void @r_4tiny4main()"));
    CHECK(contains(r.out, "define i32 @main()"));
    std::filesystem::remove_all(src.parent_path());
}

TEST_CASE("driver: a missing linker is reported") {
    std::filesystem::path src = write_program("nocc.rifts", "void main() { }\n");
    DriverRun r = drive({"run", src.string(), "--cc=/nonexistent/cc"});
    CHECK(r.status != 0);
    CHECK(contains(r.err, "linker"));
    std::filesystem::remove_all(src.parent_path());
}

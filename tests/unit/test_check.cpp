#include "project.hpp"
#include "rift/ast_printer.hpp"
#include "rift/types.hpp"
#include "rift_test.hpp"

using namespace rift;
using rift_test::Project;

namespace {

std::string codes(const std::string& main) { return Project({{"main.rifts", main}}).codes(); }

/// Type of the initializer of the first local variable named `name` in the entry's first function.
std::string type_of_local(const std::string& main, const std::string& name) {
    Project p({{"main.rifts", main}});
    if (p.diags.has_errors()) return "error " + p.codes();
    for (ast::DeclPtr& d : p.program.entry->source->ast.decls) {
        if (d->kind != ast::DeclKind::function) continue;
        for (ast::StmtPtr& s : d->as<ast::FunctionDecl>().body->statements) {
            if (s->kind == ast::StmtKind::variable && s->as<ast::VariableStmt>().name == name) {
                return types::to_string(s->as<ast::VariableStmt>().init->type);
            }
        }
    }
    return "not found";
}

} // namespace

// --- the type representation --------------------------------------------------------------

TEST_CASE("types: types are interned") {
    types::TypeContext tc;
    CHECK(tc.builtin("u32") == tc.builtin("u32"));
    CHECK(tc.builtin("u32") != tc.builtin("u64"));
    CHECK(tc.reference(tc.builtin("u8"), true) == tc.reference(tc.builtin("u8"), true));
    CHECK(tc.reference(tc.builtin("u8"), true) != tc.reference(tc.builtin("u8"), false));
    CHECK(tc.external("std::string") == tc.external("std::string"));
    CHECK(tc.builtin("int") == nullptr);   // there is no int
}

TEST_CASE("types: spelling") {
    types::TypeContext tc;
    CHECK_EQ(types::to_string(tc.reference(tc.builtin("u32"), true)), std::string("mut u32&"));
    CHECK_EQ(types::to_string(tc.pointer(tc.builtin("u8"), false)), std::string("std::raw_ptr<u8>"));
    CHECK_EQ(types::to_string(tc.untyped_int()), std::string("{integer}"));
}

TEST_CASE("types: literal ranges") {
    types::TypeContext tc;
    CHECK(types::literal_fits(255, false, tc.builtin("u8")));
    CHECK(!types::literal_fits(256, false, tc.builtin("u8")));
    CHECK(types::literal_fits(128, true, tc.builtin("i8")));
    CHECK(!types::literal_fits(128, false, tc.builtin("i8")));
    CHECK(!types::literal_fits(129, true, tc.builtin("i8")));
    CHECK(!types::literal_fits(1, true, tc.builtin("u32")));
    CHECK(types::literal_fits(0, true, tc.builtin("u32")));
    CHECK(types::literal_fits(~rift::u128{0}, false, tc.builtin("u128")));
    CHECK_EQ(types::integer_range(tc.builtin("i8")), std::string("-128 to 127"));
}

// --- literal typing (spec 02) -------------------------------------------------------------

TEST_CASE("check: literals take the type of their context") {
    CHECK_EQ(type_of_local("void f() { u8 x = 7; }", "x"), std::string("u8"));
    CHECK_EQ(type_of_local("void f(u64 a) { u64 x = a + 1; }", "x"), std::string("u64"));
    CHECK_EQ(type_of_local("void f() { u16 x = 1 + 2 * 3; }", "x"), std::string("u16"));
    CHECK_EQ(type_of_local("void f() { auto x = 5u32; }", "x"), std::string("u32"));
    CHECK_EQ(type_of_local("void f() { bool x = 1u8 < 2; }", "x"), std::string("bool"));
}

TEST_CASE("check: literals without context are errors") {
    CHECK_EQ(codes("void f() { auto x = 5; }"), std::string("E4001"));
    CHECK_EQ(codes("void f() { if (3 < 4) { } }"), std::string("E4001"));
    CHECK_EQ(codes("void f() { auto x = bit(4); }"), std::string("E4001"));
}

TEST_CASE("check: literal ranges and kinds") {
    CHECK_EQ(codes("void f() { u8 x = 256; }"), std::string("E4002"));
    CHECK_EQ(codes("void f() { i8 x = -128; i8 y = 127; }"), std::string());
    CHECK_EQ(codes("void f() { u8 x = 256u8; }"), std::string("E4002"));
    CHECK_EQ(codes("void f() { f64 x = 0; }"), std::string("E4003"));
    CHECK_EQ(codes("void f() { u32 x = 1.0; }"), std::string("E4010"));
}

// --- conversions --------------------------------------------------------------------------

TEST_CASE("check: no implicit numeric conversions") {
    CHECK_EQ(codes("void f(u32 a) { u64 b = a; }"), std::string("E4010"));
    CHECK_EQ(codes("void f(u32 a, u64 b) { u64 c = a + b; }"), std::string("E4011"));
    CHECK_EQ(codes("void f(u32 a) { u64 b = cast<u64>(a); }"), std::string());
    CHECK_EQ(codes("void f(u32 a) { bool b = a; }"), std::string("E4010"));
}

TEST_CASE("check: contextual bool") {
    CHECK_EQ(codes("void f(u32 a) { if (a) { } while (a & 4) { } }"), std::string());
    CHECK_EQ(codes("void f(f64 a) { if (a) { } }"), std::string("E4012"));
}

TEST_CASE("check: upcasts only through references") {
    std::string classes = "open class base { }\nclass derived : base { }\n";
    CHECK_EQ(codes(classes + "void f(derived& d) { base& b = d; }"), std::string());
    CHECK_EQ(codes(classes + "void f(derived d) { base b = d; }"), std::string("E4010"));
}

// --- calls --------------------------------------------------------------------------------

TEST_CASE("check: overload resolution") {
    std::string fns = "u32 f(u32 x) { return 0; }\nu32 f(u64 x) { return 1; }\n";
    CHECK_EQ(codes(fns + "void g() { u32 r = f(5); }"), std::string("E4031"));
    CHECK_EQ(codes(fns + "void g() { u32 r = f(5u64); }"), std::string());
    CHECK_EQ(codes(fns + "void g() { u32 r = f(true); }"), std::string("E4032"));
    CHECK_EQ(codes(fns + "void g() { u32 r = f(1, 2); }"), std::string("E4032"));
}

TEST_CASE("check: overloads record the chosen symbol") {
    Project p({{"main.rifts", "u32 f(u32 x) { return 0; }\nu32 f(bool x) { return 1; }\nvoid g() { u32 r = f(true); }\n"}});
    REQUIRE_EQ(p.codes(), std::string());
    auto& g    = p.program.entry->source->ast.decls[2]->as<ast::FunctionDecl>();
    auto& init = *g.body->statements[0]->as<ast::VariableStmt>().init;
    auto& path = init.as<ast::CallExpr>().callee->as<ast::PathExpr>();
    REQUIRE(path.symbol != nullptr);
    CHECK_EQ(ast::to_string(*path.symbol->function()->params[0].type), std::string("bool"));
}

TEST_CASE("check: templates deduce from typed arguments") {
    std::string larger = "template<type T> T larger(T a, T b) { return a; }\n";
    CHECK_EQ(codes(larger + "void g(u32 a) { u32 m = larger(a, a); }"), std::string());
    CHECK_EQ(codes(larger + "void g() { u32 m = larger<u32>(3, 4); }"), std::string());
    CHECK_EQ(codes(larger + "void g() { u32 m = larger(3, 4); }"), std::string("E4160"));
    CHECK_EQ(codes(larger + "void g(u32 a) { u64 m = larger(a, a); }"), std::string("E4010"));
}

TEST_CASE("check: returns") {
    CHECK_EQ(codes("u32 f() { return 1; }"), std::string());
    CHECK_EQ(codes("u32 f(bool c) { if (c) { return 1; } }"), std::string("E4042"));
    CHECK_EQ(codes("u32 f(bool c) { if (c) { return 1; } else { return 2; } }"), std::string());
    CHECK_EQ(codes("u32 f() { while (true) { } }"), std::string());
    CHECK_EQ(codes("u32 f() { while (true) { break; } }"), std::string("E4042"));
    CHECK_EQ(codes("void f() { return 1; }"), std::string("E4041"));
    CHECK_EQ(codes("u32 f() { return; }"), std::string("E4040"));
}

// --- mutability ---------------------------------------------------------------------------

TEST_CASE("check: mutability of variables and references") {
    CHECK_EQ(codes("void f() { u32 x = 1; x = 2; }"), std::string("E4020"));
    CHECK_EQ(codes("void f() { mut u32 x = 1; x = 2; }"), std::string());
    CHECK_EQ(codes("void f(u32& x) { x = 2; }"), std::string("E4020"));
    CHECK_EQ(codes("void f(mut u32& x) { x = 2; }"), std::string());
}

TEST_CASE("check: call-site mut") {
    std::string bump = "void bump(mut u32& x) { x += 1; }\n";
    CHECK_EQ(codes(bump + "void f() { mut u32 a = 0; bump(mut a); }"), std::string());
    CHECK_EQ(codes(bump + "void f() { mut u32 a = 0; bump(a); }"), std::string("E4023"));
    CHECK_EQ(codes(bump + "void f() { u32 a = 0; bump(mut a); }"), std::string("E4020"));
}

TEST_CASE("check: mut methods") {
    std::string c = "struct c { u32 v = 0; void inc() mut { this.v += 1; } u32 get() { return this.v; } }\n";
    CHECK_EQ(codes(c + "void f() { mut c x = c{}; x.inc(); }"), std::string());
    CHECK_EQ(codes(c + "void f() { c x = c{}; x.inc(); }"), std::string("E4022"));
    CHECK_EQ(codes("struct c { u32 v = 0; u32 get() { this.v = 1; return 0; } }"), std::string("E4021"));
}

// --- match --------------------------------------------------------------------------------

TEST_CASE("check: exhaustiveness") {
    std::string e = "enum e : u8 { a, b }\n";
    CHECK_EQ(codes(e + "u32 f(e x) { return match (x) { e::a => 1, e::b => 2, }; }"), std::string());
    CHECK_EQ(codes(e + "u32 f(e x) { return match (x) { e::a => 1, }; }"), std::string("E4060"));
    CHECK_EQ(codes("u32 f(u32 x) { return match (x) { 0 => 1, }; }"), std::string("E4060"));
    CHECK_EQ(codes("u32 f(bool x) { return match (x) { true => 1, false => 0, }; }"), std::string());
}

// --- operators, casts, unsafe -------------------------------------------------------------

TEST_CASE("check: operators") {
    CHECK_EQ(codes("void f(f64 a) { f64 b = a & a; }"), std::string("E4013"));
    CHECK_EQ(codes("void f(u32 a) { u32 b = a << 32; }"), std::string("E4080"));
    CHECK_EQ(codes("void f() { u8 b = bit(8); }"), std::string("E4080"));
    CHECK_EQ(codes("void f(u32 a) { u32 b = -a; }"), std::string("E4015"));
}

TEST_CASE("check: casts") {
    CHECK_EQ(codes("void f(u32 a) { u8 b = cast<u8>(a); }"), std::string("E4090"));
    CHECK_EQ(codes("void f(u32 a) { u8 b = cast::truncate<u8>(a); }"), std::string());
    CHECK_EQ(codes("void f(i32 a) { i64 b = cast<i64>(a); u64 c = cast<u64>(cast<u32>(1u8)); }"), std::string());
}

TEST_CASE("check: unsafe") {
    CHECK_EQ(codes("import std;\nvoid f(std::raw_ptr<u32> p) { u32 v = *p; }"), std::string("E4100"));
    CHECK_EQ(codes("import std;\nvoid f(std::raw_ptr<u32> p) { unsafe { u32 v = *p; } }"), std::string());
    CHECK_EQ(codes("unsafe void g() { }\nvoid f() { g(); }"), std::string("E4101"));
}

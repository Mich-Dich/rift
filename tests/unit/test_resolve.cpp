#include "project.hpp"
#include "rift_test.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

using namespace rift;
namespace fs = std::filesystem;

namespace {

using rift_test::Project;
using rift_test::Stop;

std::string codes_of(const std::string& main) { return Project({{"main.rifts", main}}, Stop::resolve).codes(); }

/// The body statements of the first function in the entry module's source.
ast::BlockExpr& first_body(Project& p) {
    for (ast::DeclPtr& d : p.program.entry->source->ast.decls) {
        if (d->kind == ast::DeclKind::function) return *d->as<ast::FunctionDecl>().body;
    }
    throw std::runtime_error("no function");
}

} // namespace

TEST_CASE("resolve: module names follow directories") {
    CHECK_EQ(module_name_for("proj/shapes/circle.rifth", "proj"), std::string("shapes::circle"));
    CHECK_EQ(module_name_for("proj/main.rifts", "proj"), std::string("main"));
    CHECK_EQ(module_name_for("./main.rifts", "."), std::string("main"));
}

TEST_CASE("resolve: a clean program has no diagnostics") {
    CHECK_EQ(codes_of("u32 f(u32 x) { u32 y = x + 1; return y; }\nu32 main() { return f(1); }\n"), std::string());
}

TEST_CASE("resolve: uses point at their declarations") {
    Project p({{"main.rifts", "u32 f(u32 x) { u32 y = x; return y; }\n"}});
    REQUIRE_EQ(p.codes(), std::string());
    ast::BlockExpr& body = first_body(p);
    auto& decl = body.statements[0]->as<ast::VariableStmt>();
    auto& init = decl.init->as<ast::PathExpr>();
    REQUIRE(init.symbol != nullptr);
    CHECK_EQ(init.symbol->kind, sema::SymbolKind::local);
    CHECK(init.symbol->local_kind == sema::LocalKind::value_param);
    auto& ret = body.statements[1]->as<ast::ExprStmt>().expr->as<ast::ReturnExpr>();
    CHECK(ret.value->as<ast::PathExpr>().symbol == decl.symbol);
}

TEST_CASE("resolve: module items are visible regardless of order") {
    CHECK_EQ(codes_of("u32 a() { return b(); }\nu32 b() { return 1; }\n"), std::string());
}

TEST_CASE("resolve: locals are not visible before their declaration") {
    CHECK_EQ(codes_of("void f() { u32 a = b; u32 b = 1; }"), std::string("E3020"));
}

TEST_CASE("resolve: no shadowing inside a function, sibling scopes are fine") {
    CHECK_EQ(codes_of("void f(u32 x) { { u32 x = 1; } }"), std::string("E3028"));
    CHECK_EQ(codes_of("void f() { { u32 y = 1; } { u32 y = 2; } }"), std::string());
    CHECK_EQ(codes_of("static u32 g = 0;\nvoid f() { u32 g = 1; }"), std::string());   // globals may be reused
}

TEST_CASE("resolve: overloads by parameter types") {
    CHECK_EQ(codes_of("u32 f(u32 x) { return x; }\nu32 f(u64 x) { return 0; }"), std::string());
    CHECK_EQ(codes_of("u32 f(u32 x) { return x; }\nu32 f(u32 y) { return 0; }"), std::string("E3010"));
    CHECK_EQ(codes_of("struct a { }\nu32 a() { return 0; }"), std::string("E3010"));
}

TEST_CASE("resolve: header and source merge into one symbol") {
    Project p({{"main.rifts", "import lib;\nvoid main() { u32 x = lib::f(1); }\n"},
               {"lib.rifth", "u32 f(u32 x);\nclass c {\npublic:\n    u32 get();\n}\n"},
               {"lib.rifts", "u32 f(u32 x) { return x; }\nclass c {\npublic:\n    u32 get() { return this.v; }\n"
                             "private:\n    u32 v = 0;\n}\n"}});
    REQUIRE_EQ(p.codes(), std::string());
    sema::Module& lib = p.module("lib");
    REQUIRE_EQ(lib.items.at("f").size(), 1u);
    sema::Symbol* f = lib.items.at("f").front();
    CHECK(f->in_header);
    CHECK(f->definition != nullptr);
    CHECK(f->definition != f->decl);
    sema::Symbol* c = lib.items.at("c").front();
    REQUIRE_EQ(c->member_order.size(), 2u);
    CHECK_EQ(c->member_order[0]->name, std::string("get"));
    CHECK_EQ(c->member_order[1]->name, std::string("v"));
    CHECK(c->member_order[1]->access == ast::Access::private_);
}

TEST_CASE("resolve: layout is header fields, then private fields in source order") {
    Project p({{"main.rifts", "import lib;\nvoid main() { }\n"},
               {"lib.rifth", "class c {\npublic:\n    u32 a = 0;\n    u32 b = 0;\n}\n"},
               {"lib.rifts", "class c {\nprivate:\n    u32 z = 0;\n    u32 y = 0;\n}\n"}});
    REQUIRE_EQ(p.codes(), std::string());
    CHECK(p.symbols("lib").find("layout: a b z y") != std::string::npos);
}

TEST_CASE("resolve: imports") {
    CHECK_EQ(Project({{"main.rifts", "import nope;\n"}}).codes(), std::string("E3001"));
    CHECK_EQ(Project({{"main.rifts", "import x;\n"}, {"x.rifts", "void f() { }\n"}}).codes(), std::string("E3003"));
    CHECK_EQ(Project({{"main.rifts", "import a;\n"}, {"a.rifth", "import b;\n"}, {"b.rifth", "import a;\n"}}).codes(),
             std::string("E3002"));
    CHECK_EQ(Project({{"main.rifts", "import d::e;\nvoid f() { u32 x = d::e::g(); }\n"}, {"d/e.rifth", "u32 g();\n"},
                      {"d/e.rifts", "u32 g() { return 0; }\n"}})
                 .codes(),
             std::string());
}

TEST_CASE("resolve: modules must be imported where they are used") {
    Project p({{"main.rifts", "import a;\nvoid f() { u32 x = b::g(); }\n"},
               {"a.rifth", "import b;\n"},
               {"b.rifth", "u32 g();\n"},
               {"b.rifts", "u32 g() { return 0; }\n"}});
    CHECK_EQ(p.codes(), std::string("E3023"));
}

TEST_CASE("resolve: std names are accepted unchecked once std is imported") {
    CHECK_EQ(codes_of("import std;\nvoid f() { std::println(std::anything::goes); }"), std::string());
    CHECK_EQ(codes_of("void f() { std::println(1); std::println(2); }"), std::string("E3023"));   // once per file
}

TEST_CASE("resolve: this.") {
    CHECK_EQ(codes_of("struct s { u32 v = 0; u32 get() { return this.v; } }"), std::string());
    CHECK_EQ(codes_of("struct s { u32 v = 0; u32 get() { return v; } }"), std::string("E3026"));
    CHECK_EQ(codes_of("struct s { u32 v = 0; static u32 get() { return this.v; } }"), std::string("E3027"));
    CHECK_EQ(codes_of("struct s { u32 v = 0; u32 get() { return this.w; } }"), std::string("E3025"));
}

TEST_CASE("resolve: inherited members are found through 'this.'") {
    CHECK_EQ(codes_of("open class a {\npublic:\n    u32 v = 0;\n}\nclass b : a {\npublic:\n    u32 get() { return this.v; }\n}\n"),
             std::string());
}

TEST_CASE("resolve: lambdas see only what they capture") {
    CHECK_EQ(codes_of("void f() { u32 x = 1; auto g = [x]() { return x; }; }"), std::string());
    CHECK_EQ(codes_of("void f() { u32 x = 1; auto g = []() { return x; }; }"), std::string("E3050"));
}

TEST_CASE("resolve: move, mut markers and loops") {
    CHECK_EQ(codes_of("void f(u32 a, u32& b) { u32 c = move a; u32 d = move b; }"), std::string("E3032"));
    CHECK_EQ(codes_of("void g(mut u32& x) { }\nvoid f() { mut u32 a = 0; g(mut a); u32 b = mut a; }"), std::string("E3031"));
    CHECK_EQ(codes_of("void f() { break; }"), std::string("E3030"));
    CHECK_EQ(codes_of("void f() { while (true) { break; } }"), std::string());
}

TEST_CASE("resolve: hierarchy rules") {
    CHECK_EQ(codes_of("class a { }\nclass b : a { }"), std::string("E3040"));
    CHECK_EQ(codes_of("interface i { u32 f(); }\nstruct s : i { }"), std::string("E3045"));
    CHECK_EQ(codes_of("interface i { u32 f(); }\nstruct s : i { u32 f() override { return 0; } }"), std::string());
    CHECK_EQ(codes_of("interface i { u32 f(); }\nstruct s : i { u32 f() { return 0; } }"), std::string("E3044"));
    CHECK_EQ(codes_of("struct s { u32 f() override { return 0; } }"), std::string("E3043"));
    CHECK_EQ(codes_of("open class a {\npublic:\n    virtual u32 f() abstract;\n}\nclass b : a { }"), std::string("E3045"));
}

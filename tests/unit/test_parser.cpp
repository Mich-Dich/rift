#include "rift/ast_printer.hpp"
#include "rift/lexer.hpp"
#include "rift/parser.hpp"
#include "rift_test.hpp"

#include <sstream>

using namespace rift;

namespace {

struct Parsed {
    SourceManager    sources;
    DiagnosticEngine diags;
    ast::File        file;

    explicit Parsed(std::string text) {
        FileId id = sources.add("t.rifts", std::move(text));
        auto   tokens = tokenize(sources.file(id), id, diags);
        file          = parse(sources.file(id), id, tokens, diags);
    }

    std::string codes() const {
        std::string out;
        for (const Diagnostic& d : diags.all()) {
            out += (out.empty() ? "" : " ") + std::string(code_name(d.code));
        }
        return out;
    }

    std::string dump() const {
        std::ostringstream out;
        ast::print(out, file);
        return out.str();
    }
};

/// Parses one expression and returns it as a single-line S-expression.
std::string expr(std::string text) {
    SourceManager    sources;
    DiagnosticEngine diags;
    FileId           id     = sources.add("t.rifts", std::move(text));
    auto             tokens = tokenize(sources.file(id), id, diags);
    ast::ExprPtr     e      = parse_expression(sources.file(id), id, tokens, diags);
    if (diags.has_errors()) {
        return "error " + std::string(code_name(diags.all().front().code));
    }
    return ast::to_sexpr(*e);
}

/// Codes reported for the body of a function.
std::string body_codes(const std::string& body) {
    return Parsed("void f() {\n" + body + "\n}\n").codes();
}

} // namespace

// --- expressions --------------------------------------------------------------------------

TEST_CASE("parser: multiplication binds tighter than addition") {
    CHECK_EQ(expr("a + b * c"), std::string("(+ (path a) (* (path b) (path c)))"));
    CHECK_EQ(expr("a * b + c"), std::string("(+ (* (path a) (path b)) (path c))"));
}

TEST_CASE("parser: binary operators are left associative") {
    CHECK_EQ(expr("a - b - c"), std::string("(- (- (path a) (path b)) (path c))"));
    CHECK_EQ(expr("a / b * c"), std::string("(* (/ (path a) (path b)) (path c))"));
}

TEST_CASE("parser: bit operators bind tighter than comparisons (spec 16)") {
    CHECK_EQ(expr("flags & mask == 0"), std::string("(== (& (path flags) (path mask)) (int 0))"));
    CHECK_EQ(expr("a | b == c"), std::string("(== (| (path a) (path b)) (path c))"));
}

TEST_CASE("parser: full precedence ladder") {
    // || < && < compare < range < | < ^ < & < shift < add < mul
    CHECK_EQ(expr("a || b && c"), std::string("(|| (path a) (&& (path b) (path c)))"));
    CHECK_EQ(expr("a | b ^ c & d"), std::string("(| (path a) (^ (path b) (& (path c) (path d))))"));
    CHECK_EQ(expr("a & b << c"), std::string("(& (path a) (<< (path b) (path c)))"));
    CHECK_EQ(expr("a << b + c"), std::string("(<< (path a) (+ (path b) (path c)))"));
    CHECK_EQ(expr("0..n + 1"), std::string("(.. (int 0) (+ (path n) (int 1)))"));
    CHECK_EQ(expr("a +% b *% c"), std::string("(+% (path a) (*% (path b) (path c)))"));
}

TEST_CASE("parser: comparisons and ranges do not chain") {
    CHECK_EQ(expr("a < b < c"), std::string("error E2008"));
    CHECK_EQ(expr("a == b != c"), std::string("error E2008"));
    CHECK_EQ(expr("(a < b) == c"), std::string("(== (< (path a) (path b)) (path c))"));
    CHECK_EQ(expr("a < b && b < c"), std::string("(&& (< (path a) (path b)) (< (path b) (path c)))"));
}

TEST_CASE("parser: prefix operators") {
    CHECK_EQ(expr("-a * b"), std::string("(* (- (path a)) (path b))"));
    CHECK_EQ(expr("!~x"), std::string("(! (~ (path x)))"));
    CHECK_EQ(expr("*p"), std::string("(deref (path p))"));
    CHECK_EQ(expr("&x"), std::string("(addr (path x))"));
    CHECK_EQ(expr("mut &x"), std::string("(addr-mut (path x))"));
    CHECK_EQ(expr("move s"), std::string("(move (path s))"));
    CHECK_EQ(expr("f(mut v)"), std::string("(call (path f) (mut (path v)))"));
}

TEST_CASE("parser: postfix chains") {
    CHECK_EQ(expr("a.b(c)[d]?"), std::string("(? (index (call (.b (path a)) (path c)) (path d)))"));
    CHECK_EQ(expr("p->x"), std::string("(->x (path p))"));
    CHECK_EQ(expr("this.shape::describe()"), std::string("(call (.shape::describe (this)))"));
}

TEST_CASE("parser: paths") {
    CHECK_EQ(expr("std::string::from(x)"), std::string("(call (path std::string::from) (path x))"));
    CHECK_EQ(expr("u32::max"), std::string("(path u32::max)"));
    CHECK_EQ(expr("std::file::open(p)"), std::string("(call (path std::file::open) (path p))"));
}

TEST_CASE("parser: '<' is a template list only before '(' '::' or '{'") {
    CHECK_EQ(expr("a < b"), std::string("(< (path a) (path b))"));
    CHECK_EQ(expr("larger<u32>(1, 2)"), std::string("(call (path larger<u32>) (int 1) (int 2))"));
    CHECK_EQ(expr("std::darray<u32>{}"), std::string("(new std::darray<u32>)"));
    CHECK_EQ(expr("ptr<circle>::make()"), std::string("(call (path ptr<circle>::make))"));
    CHECK_EQ(expr("x.parse<u32>()"), std::string("(call (.parse<u32> (path x)))"));
    CHECK_EQ(expr("a < b && c > d"), std::string("(&& (< (path a) (path b)) (> (path c) (path d)))"));
}

TEST_CASE("parser: struct literals need named fields") {
    CHECK_EQ(expr("point{ .x = 1, .y = 2, }"), std::string("(new point (.x (int 1)) (.y (int 2)))"));
    CHECK_EQ(expr("point{}"), std::string("(new point)"));
    CHECK_EQ(expr("point{ 1, 2 }"), std::string("error E2017"));
}

TEST_CASE("parser: casts and bit") {
    CHECK_EQ(expr("cast<u64>(x)"), std::string("(cast u64 (path x))"));
    CHECK_EQ(expr("cast::truncate<u8>(x)"), std::string("(cast::truncate u8 (path x))"));
    CHECK_EQ(expr("bit(4)"), std::string("(bit (int 4))"));
    CHECK_EQ(expr("bit<u128>(100)"), std::string("(bit u128 (int 100))"));
}

TEST_CASE("parser: if, match and blocks are expressions") {
    CHECK_EQ(expr("if (a) { 1 } else { 2 }"),
             std::string("(if (path a) (block (value (int 1))) (else (block (value (int 2)))))"));
    CHECK_EQ(expr("if (a) { 1 }"), std::string("error E2020"));
    CHECK_EQ(expr("match (x) { 0 => a, _ => b, }"),
             std::string("(match (path x) (arm (int 0) (path a)) (arm _ (path b)))"));
    CHECK_EQ(expr("{ u32 t = 1; t }"), std::string("(block (var t : u32 (int 1)) (value (path t)))"));
}

TEST_CASE("parser: lambdas") {
    CHECK_EQ(expr("[](u32 x) { return x; }"),
             std::string("(lambda (captures) (param x : u32) (block (return (path x))))"));
    CHECK_EQ(expr("[x, &y, mut &z, move w, v = 1](u32 a) -> u32 { return a; }"),
             std::string("(lambda (captures (copy x) (ref y) (mut-ref z) (move w) (init v (int 1))) "
                         "(param a : u32) (-> u32) (block (return (path a))))"));
    CHECK_EQ(expr("[=]() { }"), std::string("error E2026"));
}

TEST_CASE("parser: assignment is not an expression") {
    CHECK_EQ(expr("(a = b)"), std::string("error E2006"));
    CHECK_EQ(body_codes("if (x = 0) { }"), std::string("E2006"));
}

TEST_CASE("parser: '_' is not a value") {
    CHECK_EQ(expr("_"), std::string("error E2029"));
}

// --- statements ---------------------------------------------------------------------------

TEST_CASE("parser: declaration vs expression statements") {
    Parsed p("void f() { u32 a = 1; mut T b = c; T& r = d; x = 1; foo(x); a < b; std::darray<u32> v = default; }");
    CHECK_EQ(p.codes(), std::string());
    CHECK_EQ(p.dump(), std::string(
        "(fn f -> void\n"
        "  (block\n"
        "    (var a : u32 (int 1))\n"
        "    (var b : mut T (path c))\n"
        "    (var r : T& (path d))\n"
        "    (assign = (path x) (int 1))\n"
        "    (call (path foo) (path x))\n"
        "    (< (path a) (path b))\n"
        "    (var v : std::darray<u32> (default))))\n"));
}

TEST_CASE("parser: mut before a reference makes the reference mutable") {
    Parsed p("void f(mut u32& a, mut u32 b, u32& c) { }");
    CHECK_EQ(p.dump(), std::string("(fn f -> void (param a : mut u32&) (param b : mut u32) (param c : u32&) (block))\n"));
}

TEST_CASE("parser: the last expression without ';' is the block's value") {
    Parsed p("u32 f() { u32 x = 1; x + 1 }");
    CHECK_EQ(p.dump(), std::string("(fn f -> u32 (block (var x : u32 (int 1)) (value (+ (path x) (int 1)))))\n"));
}

TEST_CASE("parser: >> closes two template argument lists") {
    Parsed p("void f() { a<b<c>> x = default; }");
    CHECK_EQ(p.codes(), std::string());
    CHECK(p.dump().find("(var x : a<b<c>> (default))") != std::string::npos);
}

TEST_CASE("parser: spec-specific statement errors") {
    CHECK_EQ(body_codes("u32 z;"), std::string("E2010"));
    CHECK_EQ(body_codes("test t{ .x = 5 };"), std::string("E2011"));
    CHECK_EQ(body_codes("if (x) y = 1;"), std::string("E2005"));
    CHECK_EQ(body_codes("while (x) y = 1;"), std::string("E2005"));
    CHECK_EQ(body_codes("x++;"), std::string("E2007"));
    CHECK_EQ(body_codes("x--;"), std::string("E2007"));
    CHECK_EQ(body_codes("for (u32 i = 0; i < 3; i += 1) { }"), std::string("E2015"));
    CHECK_EQ(body_codes("if (x) { };"), std::string("E2009"));
    CHECK_EQ(body_codes("match (x) { 1 => 2 }"), std::string("E2014"));
    CHECK_EQ(body_codes("const u32 x = 1;"), std::string("E2019"));
}

// --- declarations -------------------------------------------------------------------------

TEST_CASE("parser: declaration errors") {
    CHECK_EQ(Parsed("u32 x = 0;").codes(), std::string("E2012"));
    CHECK_EQ(Parsed("compile_time u32 x = 0;").codes(), std::string());
    CHECK_EQ(Parsed("void f(u32 x = 1) { }").codes(), std::string("E2024"));
    CHECK_EQ(Parsed("void f(auto x) { }").codes(), std::string("E2025"));
    CHECK_EQ(Parsed("u32 a::b() { }").codes(), std::string("E2013"));
    CHECK_EQ(Parsed("run_time u32 f();").codes(), std::string("E2018"));
    CHECK_EQ(Parsed("enum e { a }").codes(), std::string("E2027"));
    CHECK_EQ(Parsed("struct s { };").codes(), std::string("E2009"));
    CHECK_EQ(Parsed("struct s { public: u32 x; }").codes(), std::string("E2021"));
    CHECK_EQ(Parsed("struct s { ~s(); }").codes(), std::string("E2022"));
    CHECK_EQ(Parsed("class c { virtual void f() override; }").codes(), std::string("E2023"));
    CHECK_EQ(Parsed("interface i { void f() { } }").codes(), std::string("E2028"));
    CHECK_EQ(Parsed("x = 1;").codes(), std::string("E2004"));
}

TEST_CASE("parser: class members get their access section") {
    Parsed p("class c { public: u32 get(); private: u32 v = 0; }");
    CHECK_EQ(p.dump(), std::string("(class c (public fn get -> u32) (private field v : u32 (int 0)))\n"));
}

TEST_CASE("parser: doc comments attach to the next declaration") {
    Parsed p("/// first\n/// second\nvoid f();");
    CHECK_EQ(p.file.decls.size(), 1u);
    CHECK_EQ(p.file.decls[0]->doc, std::string(" first\n second"));
}

TEST_CASE("parser: templates") {
    Parsed p("template<type T : a & b, usize N> struct s { }");
    CHECK_EQ(p.codes(), std::string());
    CHECK_EQ(p.dump(), std::string("(struct s (template (type T : a & b) (value N : usize)))\n"));
}

// --- recovery -----------------------------------------------------------------------------

TEST_CASE("parser: one error per broken statement, no cascades") {
    CHECK_EQ(body_codes("u32 a = ;\nu32 b = ;\nu32 c = 1;"), std::string("E2002 E2002"));
}

TEST_CASE("parser: a missing ';' at line end does not swallow the next line") {
    CHECK_EQ(body_codes("u32 a = 1\nu32 b = (2;"), std::string("E2001 E2001"));
}

TEST_CASE("parser: recovers at the next declaration") {
    Parsed p("void f( { }\nvoid g() { }\n");
    CHECK_EQ(p.codes(), std::string("E2003"));
    CHECK_EQ(p.file.decls.size(), 1u);
    CHECK_EQ(p.file.decls[0]->name, std::string("g"));
}

TEST_CASE("parser: unclosed braces point at the opener") {
    Parsed p("void f() {\n  u32 x = 1;\n");
    REQUIRE_EQ(p.codes(), std::string("E2016"));
    CHECK_EQ(p.diags.all()[0].span.begin, 9u);
}

TEST_CASE("parser: a forgotten '{' after a signature still parses the body") {
    Parsed p("void f()\n    u32 a = 1;\n    u32 b = 2;\n}\nvoid g() { }\n");
    CHECK_EQ(p.codes(), std::string("E2001"));
    CHECK_EQ(p.file.decls.size(), 2u);
}

TEST_CASE("parser: stray code at file scope is reported once per run") {
    Parsed p("void f() { }\n}\nx = 1;\ny = 2;\nz(3);\nvoid g() { }\nw = 4;\n");
    CHECK_EQ(p.codes(), std::string("E2004 E2004"));
}

// --- f-strings (M6) -----------------------------------------------------------------------

TEST_CASE("parser: f-strings split into text and holes") {
    rift::SourceManager    sm;
    rift::DiagnosticEngine diags;
    rift::FileId id = sm.add("t.rifts", "void f() { g(f\"a\\t{x}{{b}} {y.z:>8.2x}!\"); }");
    std::vector<rift::Token> tokens = rift::tokenize(sm.file(id), id, diags);
    rift::ast::File file = rift::parse(sm.file(id), id, tokens, diags);
    REQUIRE(!diags.has_errors());
    auto& body = *file.decls[0]->as<rift::ast::FunctionDecl>().body;
    auto& call = body.tail ? *body.tail : *body.statements[0]->as<rift::ast::ExprStmt>().expr;
    auto& lit  = call.as<rift::ast::CallExpr>().args[0]->as<rift::ast::LiteralExpr>();
    REQUIRE_EQ(lit.format_parts.size(), 5u);
    CHECK_EQ(lit.format_parts[0].text, std::string("a\t"));
    CHECK(lit.format_parts[1].expr != nullptr);
    CHECK_EQ(lit.format_parts[2].text, std::string("{b} "));
    const rift::ast::FormatSpec& spec = lit.format_parts[3].spec;
    CHECK(spec.present);
    CHECK_EQ(spec.align, '>');
    CHECK_EQ(spec.width, 8);
    CHECK_EQ(spec.precision, 2);
    CHECK_EQ(spec.radix, 'x');
    CHECK_EQ(lit.format_parts[4].text, std::string("!"));
    // Hole spans point into the original source.
    CHECK_EQ(sm.file(id).text().substr(lit.format_parts[1].expr->span.begin, 1), std::string_view("x"));
}

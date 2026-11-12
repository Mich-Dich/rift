#include "project.hpp"
#include "rift_test.hpp"

using rift_test::Project;

namespace {

std::string codes(const std::string& main) { return Project({{"main.rifts", "import std;\n" + main}}).codes(); }

} // namespace

// --- copy types (spec 12) ------------------------------------------------------------------

TEST_CASE("ownership: builtins, references and copy structs are copied") {
    CHECK_EQ(codes("void f(u32 a, f64 b, bool c) { u32 x = a; f64 y = b; bool z = c; }"), std::string());
    CHECK_EQ(codes("struct p { f64 x = 0.0; }\nvoid f(p a) { p b = a; }"), std::string());
    CHECK_EQ(codes("enum e : u8 { a, b(u32) }\nvoid f(e a) { e b = a; }"), std::string());
}

TEST_CASE("ownership: owning values are never copied implicitly") {
    CHECK_EQ(codes("void f(std::string a) { std::string b = a; }"), std::string("E5001"));
    CHECK_EQ(codes("struct n { std::string s; }\nvoid f(n a) { n b = a; }"), std::string("E5001"));
    CHECK_EQ(codes("class c {\npublic:\n    ~c() { }\n}\nvoid f(c a) { c b = a; }"), std::string("E5001"));
    CHECK_EQ(codes("enum e : u8 { a(std::string) }\nvoid f(e a) { e b = a; }"), std::string("E5001"));
    CHECK_EQ(codes("void f(std::string a) { std::string b = a.clone(); std::string c = move a; }"), std::string());
}

TEST_CASE("ownership: views bind without copying") {
    CHECK_EQ(codes("void look(std::str s) { }\nvoid f(std::string a) { look(a); std::str v = a; }"), std::string());
}

TEST_CASE("ownership: returning a local moves it") {
    CHECK_EQ(codes("std::string f(std::string s) { return s; }"), std::string());
    CHECK_EQ(codes("struct h { std::string t; }\nstd::string f(h& x) { return x.t; }"), std::string("E5001"));
}

// --- use after move ------------------------------------------------------------------------

TEST_CASE("ownership: use after move") {
    std::string sink = "void sink(std::string s) { }\n";
    CHECK_EQ(codes(sink + "void f(std::string a) { sink(move a); sink(move a); }"), std::string("E5002"));
    CHECK_EQ(codes(sink + "void f(mut std::string a) { sink(move a); a = std::string::from(\"x\"); sink(move a); }"),
             std::string());
}

TEST_CASE("ownership: moves merge across branches") {
    std::string sink = "void sink(std::string s) { }\n";
    CHECK_EQ(codes(sink + "void f(bool c, std::string a) { if (c) { sink(move a); } sink(move a); }"), std::string("E5002"));
    CHECK_EQ(codes(sink + "void f(bool c, std::string a) { if (c) { sink(move a); return; } sink(move a); }"),
             std::string());
    CHECK_EQ(codes(sink + "void f(bool c, std::string a) { if (c) { sink(move a); } else { sink(move a); } }"),
             std::string());
}

TEST_CASE("ownership: moves in loops") {
    std::string sink = "void sink(std::string s) { }\n";
    CHECK_EQ(codes(sink + "void f(bool c, std::string a) { while (c) { sink(move a); } }"), std::string("E5002"));
    CHECK_EQ(codes(sink + "void f(bool c, std::string a) { while (c) { sink(move a); break; } }"), std::string());
    CHECK_EQ(codes(sink + "void f(bool c, mut std::string a) { while (c) { sink(move a); a = std::string::from(\"x\"); } }"),
             std::string());
}

TEST_CASE("ownership: one report per moved use") {
    // the second argument is moved, not also an illegal copy
    CHECK_EQ(codes("void two(std::string a, std::string b) { }\nvoid f(std::string a) { two(move a, a); }"),
             std::string("E5002"));
}

// --- references and views ------------------------------------------------------------------

TEST_CASE("ownership: references cannot be stored") {
    CHECK_EQ(codes("struct s { u32& r; }"), std::string("E5010"));
    CHECK_EQ(codes("struct s { std::str v; }"), std::string("E5011"));
    CHECK_EQ(codes("view struct s { std::str v; u32& r; }"), std::string());
}

TEST_CASE("ownership: a returned reference has exactly one source") {
    CHECK_EQ(codes("std::str f(std::str a) { return a; }"), std::string());
    CHECK_EQ(codes("std::str f(std::str a, std::str b) { return a; }"), std::string("E5012"));
    CHECK_EQ(codes("std::str f() { return \"x\"; }"), std::string("E5012"));
    CHECK_EQ(codes("struct s { u32 v = 0; u32& get() { return this.v; } }"), std::string());
}

TEST_CASE("ownership: no references to values that die") {
    CHECK_EQ(codes("u32& f(u32& a) { u32 x = 1; return x; }"), std::string("E5013"));
    CHECK_EQ(codes("std::str f(std::str v, std::string s) { return s; }"), std::string("E5013"));
    CHECK_EQ(codes("u32& f(u32& a) { return a; }"), std::string());
}

// --- aliasing and borrows ------------------------------------------------------------------

TEST_CASE("ownership: a mut reference is the only way to a value") {
    std::string fns = "void swap(mut u32& a, mut u32& b) { }\nvoid read2(mut u32& a, u32& b) { }\n";
    CHECK_EQ(codes(fns + "void f(mut u32 a, mut u32 b) { swap(mut a, mut b); }"), std::string());
    CHECK_EQ(codes(fns + "void f(mut u32 a) { swap(mut a, mut a); }"), std::string("E5020"));
    CHECK_EQ(codes(fns + "void f(mut u32 a) { read2(mut a, a); }"), std::string("E5020"));
}

TEST_CASE("ownership: containers are frozen while a loop borrows them") {
    CHECK_EQ(codes("void f(mut std::darray<u32> v) { for (u32& x : v) { v.push(x); } }"), std::string("E5021"));
    CHECK_EQ(codes("void f(mut std::darray<u32> v) { for (u32& x : v) { } v.push(1); }"), std::string());
    CHECK_EQ(codes("void g(u32& x) { }\nvoid f(mut std::darray<u32> v) { for (mut u32& x : v) { x += 1; g(v[0]); } }"),
             std::string("E5021"));
}

TEST_CASE("ownership: local references keep their source frozen while used") {
    std::string g = "void g(u32& x) { }\n";
    CHECK_EQ(codes(g + "void f(mut std::darray<u32> v) { u32& r = v[0]; v.push(1); g(r); }"), std::string("E5022"));
    CHECK_EQ(codes(g + "void f(mut std::darray<u32> v) { u32& r = v[0]; g(r); v.push(1); }"), std::string());
}

// --- clone and lambdas ---------------------------------------------------------------------

TEST_CASE("ownership: clone needs every field to be cloneable") {
    CHECK_EQ(codes("struct s { std::string a; }\nvoid f(s x) { s y = x.clone(); }"), std::string());
    CHECK_EQ(codes("struct s { std::file a; }\nvoid f(s x) { s y = x.clone(); }"), std::string("E5030"));
}

TEST_CASE("ownership: lambda captures") {
    CHECK_EQ(codes("void f(std::string a) { auto g = [move a]() { }; std::str v = a; }"), std::string("E5002"));
    CHECK_EQ(codes("void f(std::string a) { auto g = [a]() { }; }"), std::string("E5001"));
    CHECK_EQ(codes("void f(std::string a, u32 n) { auto g = [&a, n]() { }; std::str v = a; }"), std::string());
}

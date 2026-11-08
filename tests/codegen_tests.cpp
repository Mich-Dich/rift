
#include "test_framework.h"

#include <memory>
#include <string>
#include <string_view>

#include "rift/ast.h"
#include "rift/codegen.h"
#include "rift/diagnostic_engine.h"
#include "rift/lexer.h"
#include "rift/parser.h"
#include "rift/sema.h"
#include "rift/source.h"



using namespace rift;

// FORWARD DECLARATIONS ================================================================================================

namespace {

    struct cg_test_result {
        std::unique_ptr<source_manager>             sm;
        std::unique_ptr<diagnostic_engine>          diag;
        std::unique_ptr<parser>                     p;
        sema_result                                 sema;
        std::string                                 ir;
    };


    cg_test_result compile_to_ir(std::string_view source) {

        cg_test_result r;
        r.sm   = std::make_unique<source_manager>();
        r.diag = std::make_unique<diagnostic_engine>();

        const file_id f = r.sm->add("<test>", std::string(source));
        lexer lex(*r.sm, f, *r.diag);
        auto tokens = lex.tokenize();

        r.p = std::make_unique<parser>(*r.sm, f, *r.diag, std::move(tokens));
        const node_id program = r.p->parse_program();

        r.sema = analyze(r.p->arena(), program, *r.sm, f, *r.diag);
        if (!r.diag->has_errors())
            r.ir = emit_llvm_ir_to_string(r.p->arena(), program, r.sema, *r.diag);

        return r;
    }


    bool contains(const std::string& hay, std::string_view needle) {
        return hay.find(needle) != std::string::npos;
    }

}

// TESTS ===============================================================================================================

TEST(codegen_minimal_main) {
    auto r = compile_to_ir("u32 main() { return 0u32; }");
    EXPECT_FALSE(r.diag->has_errors());
    EXPECT_TRUE(contains(r.ir, "define i32 @main()"));
    EXPECT_TRUE(contains(r.ir, "ret i32 0"));
}

TEST(codegen_return_constant) {
    auto r = compile_to_ir("u32 main() { return 42u32; }");
    EXPECT_FALSE(r.diag->has_errors());
    EXPECT_TRUE(contains(r.ir, "ret i32 42"));
}

TEST(codegen_hex_literal) {
    auto r = compile_to_ir("u32 main() { return 0xFFu32; }");
    EXPECT_FALSE(r.diag->has_errors());
    EXPECT_TRUE(contains(r.ir, "ret i32 255"));
}

TEST(codegen_var_decl_and_load) {
    auto r = compile_to_ir("u32 main() { u32 x = 7u32; return x; }");
    EXPECT_FALSE(r.diag->has_errors());
    EXPECT_TRUE(contains(r.ir, "%x = alloca i32"));
    EXPECT_TRUE(contains(r.ir, "store i32 7, ptr %x"));
    EXPECT_TRUE(contains(r.ir, "load i32, ptr %x"));
}

TEST(codegen_arith) {
    auto r = compile_to_ir("u32 main() { return 3u32 * 5u32; }");
    EXPECT_FALSE(r.diag->has_errors());
    EXPECT_TRUE(contains(r.ir, "mul i32 3, 5"));
}

TEST(codegen_unsigned_div) {
    auto r = compile_to_ir("u32 main() { return 10u32 / 3u32; }");
    EXPECT_FALSE(r.diag->has_errors());
    EXPECT_TRUE(contains(r.ir, "udiv i32 10, 3"));
}

TEST(codegen_signed_div) {
    auto r = compile_to_ir("u32 main() { i32 x = 10i32 / 3i32; return 0u32; }");
    EXPECT_FALSE(r.diag->has_errors());
    EXPECT_TRUE(contains(r.ir, "sdiv i32 10, 3"));
}

TEST(codegen_unary_minus) {
    auto r = compile_to_ir("u32 main() { i32 x = -5i32; return 0u32; }");
    EXPECT_FALSE(r.diag->has_errors());
    EXPECT_TRUE(contains(r.ir, "sub i32 0, 5"));
}

TEST(codegen_comparison_yields_i1) {
    auto r = compile_to_ir("u32 main() { bool b = 1u32 < 2u32; return 0u32; }");
    EXPECT_FALSE(r.diag->has_errors());
    EXPECT_TRUE(contains(r.ir, "icmp slt i32 1, 2"));
    EXPECT_TRUE(contains(r.ir, "alloca i1"));
}

TEST(codegen_empty_void_function) {
    auto r = compile_to_ir(
        "void f() { }\n"
        "u32 main() { return 0u32; }");
    EXPECT_FALSE(r.diag->has_errors());
    EXPECT_TRUE(contains(r.ir, "define void @f()"));
    EXPECT_TRUE(contains(r.ir, "ret void"));
}

TEST(codegen_if_else_emits_branch) {
    auto r = compile_to_ir(
        "u32 main() { if (true) { return 0u32; } else { return 1u32; } }");
    EXPECT_FALSE(r.diag->has_errors());
    EXPECT_TRUE(contains(r.ir, "br i1 true, label %if_then_0, label %if_else_1"));
    EXPECT_TRUE(contains(r.ir, "if_then_0:"));
    EXPECT_TRUE(contains(r.ir, "if_else_1:"));
    EXPECT_TRUE(contains(r.ir, "ret i32 0"));
    EXPECT_TRUE(contains(r.ir, "ret i32 1"));
}

TEST(codegen_if_both_terminate_no_merge) {
    auto r = compile_to_ir(
        "u32 main() { if (true) { return 0u32; } else { return 1u32; } }");
    EXPECT_FALSE(r.diag->has_errors());
    // No merge label since both branches terminate
    EXPECT_FALSE(contains(r.ir, "if_merge_"));
}

TEST(codegen_if_no_else_emits_merge) {
    auto r = compile_to_ir(
        "u32 main() { if (false) { return 0u32; } return 1u32; }");
    EXPECT_FALSE(r.diag->has_errors());
    EXPECT_TRUE(contains(r.ir, "if_merge_"));
    EXPECT_TRUE(contains(r.ir, "ret i32 1"));
}

TEST(codegen_while_emits_loop) {
    auto r = compile_to_ir(
        "u32 main() {\n"
        "    mut u32 n = 0u32;\n"
        "    while (n < 3u32) { n = n + 1u32; }\n"
        "    return n;\n"
        "}");
    EXPECT_FALSE(r.diag->has_errors());
    EXPECT_TRUE(contains(r.ir, "br label %while_cond_"));
    EXPECT_TRUE(contains(r.ir, "while_cond_"));
    EXPECT_TRUE(contains(r.ir, "while_body_"));
    EXPECT_TRUE(contains(r.ir, "while_end_"));
    EXPECT_TRUE(contains(r.ir, "br label %while_cond_"));
}

TEST(codegen_assign_emits_store) {
    auto r = compile_to_ir(
        "u32 main() { mut u32 x = 0u32; x = 42u32; return x; }");
    EXPECT_FALSE(r.diag->has_errors());
    EXPECT_TRUE(contains(r.ir, "store i32 42, ptr %x"));
}

TEST(codegen_bool_literal) {
    auto r = compile_to_ir(
        "u32 main() { if (true) { return 0u32; } return 1u32; }");
    EXPECT_FALSE(r.diag->has_errors());
    EXPECT_TRUE(contains(r.ir, "br i1 true"));
}

TEST(codegen_mut_param_alloca) {
    auto r = compile_to_ir(
        "u32 f(mut u32 x) { x = 5u32; return x; }\n"
        "u32 main() { return 0u32; }");
    EXPECT_FALSE(r.diag->has_errors());
    EXPECT_TRUE(contains(r.ir, "%x.addr = alloca i32"));
    EXPECT_TRUE(contains(r.ir, "store i32 %x, ptr %x.addr"));
}

TEST(codegen_call_emits_call) {
    auto r = compile_to_ir(
        "u32 add(u32 a, u32 b) { return a + b; }\n"
        "u32 main() { return add(1u32, 2u32); }");
    EXPECT_FALSE(r.diag->has_errors());
    EXPECT_TRUE(contains(r.ir, "call i32 @add(i32 1, i32 2)"));
}

TEST(codegen_default_init_zero) {
    auto r = compile_to_ir("u32 main() { u32 x = default; return x; }");
    EXPECT_FALSE(r.diag->has_errors());
    EXPECT_TRUE(contains(r.ir, "store i32 0, ptr %x"));
}

TEST(codegen_bit_emits_shl) {
    auto r = compile_to_ir("u32 main() { u32 x = bit(3u32); return x; }");
    EXPECT_FALSE(r.diag->has_errors());
    EXPECT_TRUE(contains(r.ir, "shl i32 1, 3"));
}

TEST(codegen_recursion_emits_self_call) {
    auto r = compile_to_ir(
        "u32 fact(u32 n) {\n"
        "    if (n < 1u32) { return 1u32; }\n"
        "    return n * fact(n - 1u32);\n"
        "}\n"
        "u32 main() { return fact(5u32); }");
    EXPECT_FALSE(r.diag->has_errors());
    EXPECT_TRUE(contains(r.ir, "call i32 @fact("));
}



int main() {
    return test::run_all();
}


#include "test_framework.h"

#include <memory>
#include <string>
#include <string_view>

#include "rift/ast.h"
#include "rift/diagnostic_engine.h"
#include "rift/lexer.h"
#include "rift/parser.h"
#include "rift/sema.h"
#include "rift/source.h"
#include "rift/token.h"



using namespace rift;

// FORWARD DECLARATIONS ================================================================================================

namespace {

    struct sema_test_result {

        std::unique_ptr<source_manager>             sm;
        std::unique_ptr<diagnostic_engine>          diag;
        std::unique_ptr<parser>                     p;
        sema_result                                 sema;
    };


    sema_test_result analyze_source(std::string_view source) {

        sema_test_result r;
        r.sm   = std::make_unique<source_manager>();
        r.diag = std::make_unique<diagnostic_engine>();

        const file_id f = r.sm->add("<test>", std::string(source));
        lexer lex(*r.sm, f, *r.diag);
        auto tokens = lex.tokenize();

        r.p = std::make_unique<parser>(*r.sm, f, *r.diag, std::move(tokens));
        const node_id program = r.p->parse_program();

        r.sema = analyze(r.p->arena(), program, *r.sm, f, *r.diag);
        return r;
    }


    bool has_code(const diagnostic_engine& diag, diagnostic_code code) {

        for (const auto& d : diag.diagnostics())
            if (d.code == code) return true;
        return false;
    }

}

// TESTS ===============================================================================================================

TEST(sema_valid_main) {
    auto r = analyze_source("u32 main() { return 0u32; }");
    EXPECT_FALSE(r.diag->has_errors());
}

TEST(sema_unsuffixed_literal_adapts) {
    auto r = analyze_source("u32 main() { return 0; }");
    EXPECT_FALSE(r.diag->has_errors());
}

TEST(sema_missing_main) {
    auto r = analyze_source("u32 not_main() { return 0u32; }");
    EXPECT_TRUE(r.diag->has_errors());
    EXPECT_TRUE(has_code(*r.diag, diagnostic_code::missing_main));
}

TEST(sema_main_wrong_return_type) {
    auto r = analyze_source("i32 main() { return 0i32; }");
    EXPECT_TRUE(r.diag->has_errors());
    EXPECT_TRUE(has_code(*r.diag, diagnostic_code::invalid_main_return_type));
}

TEST(sema_main_with_params) {
    auto r = analyze_source("u32 main(u32 argc) { return 0u32; }");
    EXPECT_TRUE(r.diag->has_errors());
    EXPECT_TRUE(has_code(*r.diag, diagnostic_code::invalid_main_signature));
}

TEST(sema_duplicate_function) {
    auto r = analyze_source(
        "u32 foo() { return 0u32; }\n"
        "u32 foo() { return 0u32; }\n"
        "u32 main() { return 0u32; }");
    EXPECT_TRUE(r.diag->has_errors());
    EXPECT_TRUE(has_code(*r.diag, diagnostic_code::duplicate_function));
}

TEST(sema_return_type_mismatch) {
    auto r = analyze_source("u32 main() { return 1i32; }");
    EXPECT_TRUE(r.diag->has_errors());
    EXPECT_TRUE(has_code(*r.diag, diagnostic_code::type_mismatch));
}

TEST(sema_unknown_identifier) {
    auto r = analyze_source("u32 main() { return x; }");
    EXPECT_TRUE(r.diag->has_errors());
    EXPECT_TRUE(has_code(*r.diag, diagnostic_code::unknown_identifier));
}

TEST(sema_unknown_type) {
    auto r = analyze_source("u32 main() { Foo x = 1u32; return 0u32; }");
    EXPECT_TRUE(r.diag->has_errors());
    EXPECT_TRUE(has_code(*r.diag, diagnostic_code::unknown_type));
}

TEST(sema_literal_out_of_range) {
    auto r = analyze_source("u32 main() { u8 x = 300u8; return 0u32; }");
    EXPECT_TRUE(r.diag->has_errors());
    EXPECT_TRUE(has_code(*r.diag, diagnostic_code::integer_out_of_range));
    // With the cascade fix above, only one diagnostic should fire
    EXPECT_EQ(r.diag->error_count(), 1u);
}

TEST(sema_binary_type_mismatch) {
    auto r = analyze_source("u32 main() { return 1u32 + 1i32; }");
    EXPECT_TRUE(r.diag->has_errors());
    EXPECT_TRUE(has_code(*r.diag, diagnostic_code::type_mismatch));
}

TEST(sema_binary_ok_unsuffixed_rhs) {
    // `1u32 + 2` — the unsuffixed rhs should adopt u32
    auto r = analyze_source("u32 main() { u32 x = 1u32 + 2; return 0u32; }");
    EXPECT_FALSE(r.diag->has_errors());
}

TEST(sema_bang_requires_bool) {
    auto r = analyze_source(
        "bool not_u32(u32 x) { return !x; }\n"
        "u32 main() { return 0u32; }");
    EXPECT_TRUE(r.diag->has_errors());
    EXPECT_TRUE(has_code(*r.diag, diagnostic_code::invalid_operand_type));
}

TEST(sema_tilde_rejects_float) {
    auto r = analyze_source(
        "f32 neg(f32 x) { return ~x; }\n"
        "u32 main() { return 0u32; }");
    EXPECT_TRUE(r.diag->has_errors());
    EXPECT_TRUE(has_code(*r.diag, diagnostic_code::invalid_operand_type));
}

TEST(sema_duplicate_variable) {
    auto r = analyze_source(
        "u32 main() {\n"
        "    u32 x = 1u32;\n"
        "    u32 x = 2u32;\n"
        "    return 0u32;\n"
        "}");
    EXPECT_TRUE(r.diag->has_errors());
    EXPECT_TRUE(has_code(*r.diag, diagnostic_code::duplicate_variable));
}

TEST(sema_duplicate_parameter) {
    auto r = analyze_source(
        "u32 f(u32 a, u32 a) { return a; }\n"
        "u32 main() { return 0u32; }");
    EXPECT_TRUE(r.diag->has_errors());
    EXPECT_TRUE(has_code(*r.diag, diagnostic_code::duplicate_parameter));
}

TEST(sema_param_in_scope) {
    auto r = analyze_source(
        "u32 id(u32 x) { return x; }\n"
        "u32 main() { return 0u32; }");
    EXPECT_FALSE(r.diag->has_errors());
}

TEST(sema_expr_types_recorded) {
    auto r = analyze_source(
        "u32 main() {\n"
        "    u32 x = 5u32;\n"
        "    return x;\n"
        "}");
    EXPECT_FALSE(r.diag->has_errors());

    const auto& prog = r.p->arena().get_as<program_node>(r.sema.program);
    const auto& fn = r.p->arena().get_as<function_node>(prog.declarations[0]);
    const auto& blk = r.p->arena().get_as<block_node>(fn.body);
    const auto& ret = r.p->arena().get_as<return_stmt_node>(blk.statements[1]);

    EXPECT_EQ(r.sema.expr_type(ret.value), primitive_type_id(token_kind::prim_u32));
}

TEST(sema_if_valid) {
    auto r = analyze_source(
        "u32 main() { if (true) { return 0u32; } return 1u32; }");
    EXPECT_FALSE(r.diag->has_errors());
}

TEST(sema_if_else_valid) {
    auto r = analyze_source(
        "u32 main() { if (true) { return 0u32; } else { return 1u32; } }");
    EXPECT_FALSE(r.diag->has_errors());
}

TEST(sema_if_condition_not_bool) {
    auto r = analyze_source(
        "u32 main() { if (1u32) { return 0u32; } return 1u32; }");
    EXPECT_TRUE(r.diag->has_errors());
    EXPECT_TRUE(has_code(*r.diag, diagnostic_code::invalid_condition_type));
}

TEST(sema_while_condition_not_bool) {
    auto r = analyze_source(
        "u32 main() { while (1u32) { } return 0u32; }");
    EXPECT_TRUE(r.diag->has_errors());
    EXPECT_TRUE(has_code(*r.diag, diagnostic_code::invalid_condition_type));
}

TEST(sema_assign_ok) {
    auto r = analyze_source(
        "u32 main() { mut u32 x = 0u32; x = 42u32; return x; }");
    EXPECT_FALSE(r.diag->has_errors());
}

TEST(sema_assign_to_immutable) {
    auto r = analyze_source(
        "u32 main() { u32 x = 0u32; x = 42u32; return x; }");
    EXPECT_TRUE(r.diag->has_errors());
    EXPECT_TRUE(has_code(*r.diag, diagnostic_code::cannot_assign_immutable));
}

TEST(sema_assign_type_mismatch) {
    auto r = analyze_source(
        "u32 main() { mut u32 x = 0u32; x = 1i32; return x; }");
    EXPECT_TRUE(r.diag->has_errors());
    EXPECT_TRUE(has_code(*r.diag, diagnostic_code::type_mismatch));
}

TEST(sema_assign_unknown_var) {
    auto r = analyze_source(
        "u32 main() { x = 5u32; return 0u32; }");
    EXPECT_TRUE(r.diag->has_errors());
    EXPECT_TRUE(has_code(*r.diag, diagnostic_code::unknown_identifier));
}

TEST(sema_while_counter) {
    auto r = analyze_source(
        "u32 main() {\n"
        "    mut u32 n = 0u32;\n"
        "    while (n < 3u32) {\n"
        "        n = n + 1u32;\n"
        "    }\n"
        "    return n;\n"
        "}");
    EXPECT_FALSE(r.diag->has_errors());
}

TEST(sema_nested_if) {
    auto r = analyze_source(
        "u32 main() {\n"
        "    bool a = true;\n"
        "    bool b = false;\n"
        "    if (a) { if (b) { return 0u32; } }\n"
        "    return 1u32;\n"
        "}");
    EXPECT_FALSE(r.diag->has_errors());
}

TEST(sema_if_else_if_chain_ok) {
    auto r = analyze_source(
        "u32 main() {\n"
        "    mut u32 x = 0u32;\n"
        "    if (x < 1u32) { return 0u32; }\n"
        "    else if (x < 2u32) { return 1u32; }\n"
        "    else { return 2u32; }\n"
        "}");
    EXPECT_FALSE(r.diag->has_errors());
}

TEST(sema_call_valid) {
    auto r = analyze_source(
        "u32 add(u32 a, u32 b) { return a + b; }\n"
        "u32 main() { return add(1u32, 2u32); }");
    EXPECT_FALSE(r.diag->has_errors());
}

TEST(sema_call_unknown_function) {
    auto r = analyze_source("u32 main() { return nope(); }");
    EXPECT_TRUE(r.diag->has_errors());
    EXPECT_TRUE(has_code(*r.diag, diagnostic_code::unknown_identifier));
}

TEST(sema_call_wrong_arg_count) {
    auto r = analyze_source(
        "u32 add(u32 a, u32 b) { return a + b; }\n"
        "u32 main() { return add(1u32); }");
    EXPECT_TRUE(r.diag->has_errors());
    EXPECT_TRUE(has_code(*r.diag, diagnostic_code::argument_count_mismatch));
}

TEST(sema_call_wrong_arg_type) {
    auto r = analyze_source(
        "u32 add(u32 a, u32 b) { return a + b; }\n"
        "u32 main() { return add(1u32, 2i32); }");
    EXPECT_TRUE(r.diag->has_errors());
    EXPECT_TRUE(has_code(*r.diag, diagnostic_code::type_mismatch));
}

TEST(sema_call_recursive) {
    auto r = analyze_source(
        "u32 fact(u32 n) {\n"
        "    if (n < 1u32) { return 1u32; }\n"
        "    return n * fact(n - 1u32);\n"
        "}\n"
        "u32 main() { return fact(5u32); }");
    EXPECT_FALSE(r.diag->has_errors());
}

TEST(sema_default_init_ok) {
    auto r = analyze_source("u32 main() { u32 x = default; return x; }");
    EXPECT_FALSE(r.diag->has_errors());
}

TEST(sema_default_init_bool) {
    auto r = analyze_source("u32 main() { bool b = default; return 0u32; }");
    EXPECT_FALSE(r.diag->has_errors());
}

TEST(sema_bit_ok) {
    auto r = analyze_source("u32 main() { u32 x = bit(3u32); return x; }");
    EXPECT_FALSE(r.diag->has_errors());
}

TEST(sema_bit_default_context) {
    auto r = analyze_source("u32 main() { i32 x = bit(3u32); return 0u32; }");
    EXPECT_FALSE(r.diag->has_errors());
}

TEST(sema_bit_out_of_range) {
    auto r = analyze_source("u32 main() { u8 x = bit(8u32); return 0u32; }");
    EXPECT_TRUE(r.diag->has_errors());
    EXPECT_TRUE(has_code(*r.diag, diagnostic_code::bit_index_out_of_range));
}



int main() {
    return test::run_all();
}

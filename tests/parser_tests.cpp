
#include "test_framework.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "rift/ast.h"
#include "rift/ast_dump.h"
#include "rift/diagnostic_engine.h"
#include "rift/lexer.h"
#include "rift/parser.h"
#include "rift/source.h"
#include "rift/token.h"



using namespace rift;

// FORWARD DECLARATIONS ================================================================================================

namespace {

    // CONSTANTS =======================================================================================================

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    struct parse_result {

        std::unique_ptr<source_manager>             sm;
        std::unique_ptr<diagnostic_engine>          diag;
        std::unique_ptr<parser>                     p;
        node_id                                     program;
    };

    // STATIC VARIABLES ================================================================================================

    // INTERNAL FUNCTION DECLARATION ===================================================================================

    parse_result parse_source(std::string_view source);

    // INTERNAL FUNCTION IMPLEMENTATION ================================================================================

    parse_result parse_source(std::string_view source) {

        parse_result r;
        r.sm = std::make_unique<source_manager>();
        r.diag = std::make_unique<diagnostic_engine>();
        const file_id f = r.sm->add("<test>", std::string(source));
        lexer lex(*r.sm, f, *r.diag);
        auto tokens = lex.tokenize();
        r.p = std::make_unique<parser>(*r.sm, f, *r.diag, std::move(tokens));
        r.program = r.p->parse_program();
        return r;
    }

    // TEMPLATE IMPLEMENTATION =========================================================================================

    // FUNCTION IMPLEMENTATION =========================================================================================

    // CLASS IMPLEMENTATION ============================================================================================

}

// TESTS ===============================================================================================================

TEST(parse_empty_program) {
    auto r = parse_source("");
    EXPECT_FALSE(r.diag->has_errors());
    const auto& prog = r.p->arena().get_as<program_node>(r.program);
    EXPECT_EQ(prog.declarations.size(), 0u);
}

TEST(parse_minimal_function) {
    auto r = parse_source("u32 main() { return 0u32; }");
    EXPECT_FALSE(r.diag->has_errors());
    const auto& prog = r.p->arena().get_as<program_node>(r.program);
    EXPECT_EQ(prog.declarations.size(), 1u);
}

TEST(parse_minimal_function_shape) {
    auto r = parse_source("u32 main() { return 0u32; }");
    EXPECT_FALSE(r.diag->has_errors());

    const auto& prog = r.p->arena().get_as<program_node>(r.program);
    const auto& fn = r.p->arena().get_as<function_node>(prog.declarations[0]);
    EXPECT_EQ(fn.name, "main");
    EXPECT_EQ(fn.params.size(), 0u);

    const auto& rt = r.p->arena().get_as<primitive_type_node>(fn.return_type);
    EXPECT_EQ(rt.type_kind, token_kind::prim_u32);

    const auto& blk = r.p->arena().get_as<block_node>(fn.body);
    EXPECT_EQ(blk.statements.size(), 1u);

    const auto& ret = r.p->arena().get_as<return_stmt_node>(blk.statements[0]);
    EXPECT_TRUE(ret.value.is_valid());

    const auto& lit = r.p->arena().get_as<int_literal_node>(ret.value);
    EXPECT_EQ(lit.lexeme, "0u32");
    EXPECT_EQ(lit.suffix_kind, token_kind::prim_u32);
}

TEST(parse_minimal_function_dump) {
    auto r = parse_source("u32 main() { return 0u32; }");
    EXPECT_FALSE(r.diag->has_errors());
    const auto dump = dump_ast_to_string(r.p->arena(), r.program);
    const std::string expected =
        "(program\n"
        "  (function main -> u32\n"
        "    (params)\n"
        "    (block\n"
        "      (return\n"
        "        (int_literal 0u32 u32)\n"
        "      )\n"
        "    )\n"
        "  )\n"
        ")\n";
    EXPECT_EQ(dump, expected);
}

TEST(parse_compile_check_file) {
    // The M0/M1 canonical example with exact formatting from the design doc.
    auto r = parse_source(
        "u32 main() {\n"
        "    return 0u32;\n"
        "}\n");
    EXPECT_FALSE(r.diag->has_errors());
    const auto& prog = r.p->arena().get_as<program_node>(r.program);
    EXPECT_EQ(prog.declarations.size(), 1u);
}

TEST(parse_unsuffixed_int_literal) {
    // §16.2: unsuffixed literals get their type from context at sema time.
    auto r = parse_source("u32 main() { return 0; }");
    EXPECT_FALSE(r.diag->has_errors());
    const auto& prog = r.p->arena().get_as<program_node>(r.program);
    const auto& fn = r.p->arena().get_as<function_node>(prog.declarations[0]);
    const auto& blk = r.p->arena().get_as<block_node>(fn.body);
    const auto& ret = r.p->arena().get_as<return_stmt_node>(blk.statements[0]);
    const auto& lit = r.p->arena().get_as<int_literal_node>(ret.value);
    EXPECT_EQ(lit.lexeme, "0");
    EXPECT_EQ(lit.suffix_kind, token_kind::invalid);
}

TEST(parse_function_with_params) {
    auto r = parse_source("u32 add(u32 a, u32 b) { return a; }");
    EXPECT_FALSE(r.diag->has_errors());
    const auto& prog = r.p->arena().get_as<program_node>(r.program);
    const auto& fn = r.p->arena().get_as<function_node>(prog.declarations[0]);
    EXPECT_EQ(fn.params.size(), 2u);

    const auto& p0 = r.p->arena().get_as<parameter_node>(fn.params[0]);
    EXPECT_EQ(p0.name, "a");
    EXPECT_FALSE(p0.is_mut);

    const auto& p1 = r.p->arena().get_as<parameter_node>(fn.params[1]);
    EXPECT_EQ(p1.name, "b");
}

TEST(parse_mut_parameter) {
    auto r = parse_source("void bump(mut u32 x) { }");
    EXPECT_FALSE(r.diag->has_errors());
    const auto& prog = r.p->arena().get_as<program_node>(r.program);
    const auto& fn = r.p->arena().get_as<function_node>(prog.declarations[0]);
    EXPECT_EQ(fn.params.size(), 1u);
    const auto& p = r.p->arena().get_as<parameter_node>(fn.params[0]);
    EXPECT_TRUE(p.is_mut);
}

TEST(parse_var_decl_immut) {
    auto r = parse_source("u32 main() { u32 x = 5u32; return x; }");
    EXPECT_FALSE(r.diag->has_errors());
    const auto& prog = r.p->arena().get_as<program_node>(r.program);
    const auto& fn = r.p->arena().get_as<function_node>(prog.declarations[0]);
    const auto& blk = r.p->arena().get_as<block_node>(fn.body);
    EXPECT_EQ(blk.statements.size(), 2u);

    const auto& v = r.p->arena().get_as<var_decl_node>(blk.statements[0]);
    EXPECT_EQ(v.name, "x");
    EXPECT_FALSE(v.is_mut);
    EXPECT_TRUE(v.initializer.is_valid());
}

TEST(parse_var_decl_mut) {
    auto r = parse_source("u32 main() { mut u32 x = 5u32; return x; }");
    EXPECT_FALSE(r.diag->has_errors());
    const auto& prog = r.p->arena().get_as<program_node>(r.program);
    const auto& fn = r.p->arena().get_as<function_node>(prog.declarations[0]);
    const auto& blk = r.p->arena().get_as<block_node>(fn.body);
    const auto& v = r.p->arena().get_as<var_decl_node>(blk.statements[0]);
    EXPECT_TRUE(v.is_mut);
}

TEST(parse_binary_precedence) {
    auto r = parse_source("u32 main() { return 1u32 + 2u32 * 3u32; }");
    EXPECT_FALSE(r.diag->has_errors());

    const auto& prog = r.p->arena().get_as<program_node>(r.program);
    const auto& fn = r.p->arena().get_as<function_node>(prog.declarations[0]);
    const auto& blk = r.p->arena().get_as<block_node>(fn.body);
    const auto& ret = r.p->arena().get_as<return_stmt_node>(blk.statements[0]);

    // Should be (1 + (2 * 3)) — top-level is '+'
    const auto& top = r.p->arena().get_as<binary_expr_node>(ret.value);
    EXPECT_EQ(top.op, token_kind::plus);

    // rhs of '+' should be the '*' node
    const auto& mult = r.p->arena().get_as<binary_expr_node>(top.rhs);
    EXPECT_EQ(mult.op, token_kind::star);

    // dump sanity: '*' nested deeper than '+'
    const auto dump = dump_ast_to_string(r.p->arena(), r.program);
    EXPECT_TRUE(dump.find("(binary +") != std::string::npos);
    EXPECT_TRUE(dump.find("(binary *") != std::string::npos);
    // '+' should appear before '*' in the pre-order dump
    EXPECT_TRUE(dump.find("(binary +") < dump.find("(binary *"));
}

TEST(parse_unary_minus) {
    auto r = parse_source("u32 main() { return -5u32; }");
    EXPECT_FALSE(r.diag->has_errors());
    const auto& prog = r.p->arena().get_as<program_node>(r.program);
    const auto& fn = r.p->arena().get_as<function_node>(prog.declarations[0]);
    const auto& blk = r.p->arena().get_as<block_node>(fn.body);
    const auto& ret = r.p->arena().get_as<return_stmt_node>(blk.statements[0]);
    const auto& un = r.p->arena().get_as<unary_expr_node>(ret.value);
    EXPECT_EQ(un.op, token_kind::minus);
}

TEST(parse_paren_grouping) {
    auto r = parse_source("u32 main() { return (1u32 + 2u32) * 3u32; }");
    EXPECT_FALSE(r.diag->has_errors());
    const auto& prog = r.p->arena().get_as<program_node>(r.program);
    const auto& fn = r.p->arena().get_as<function_node>(prog.declarations[0]);
    const auto& blk = r.p->arena().get_as<block_node>(fn.body);
    const auto& ret = r.p->arena().get_as<return_stmt_node>(blk.statements[0]);

    // Parens change associativity: top should be '*' now.
    const auto& top = r.p->arena().get_as<binary_expr_node>(ret.value);
    EXPECT_EQ(top.op, token_kind::star);

    // lhs of '*' should be the '+' node
    const auto& plus = r.p->arena().get_as<binary_expr_node>(top.lhs);
    EXPECT_EQ(plus.op, token_kind::plus);
}

TEST(parse_error_missing_semicolon) {
    auto r = parse_source("u32 main() { return 0u32 }");
    EXPECT_TRUE(r.diag->has_errors());
}

TEST(parse_error_missing_rparen) {
    auto r = parse_source("u32 main( { return 0u32; }");
    EXPECT_TRUE(r.diag->has_errors());
}

TEST(parse_error_missing_return_type) {
    auto r = parse_source("main() { return 0u32; }");
    EXPECT_TRUE(r.diag->has_errors());
}

TEST(parse_error_empty_expr) {
    auto r = parse_source("u32 main() { return ; }");
    EXPECT_TRUE(r.diag->has_errors());
}

TEST(parse_if_stmt) {
    auto r = parse_source("u32 main() { if (true) { return 0u32; } return 1u32; }");
    EXPECT_FALSE(r.diag->has_errors());
    const auto& prog = r.p->arena().get_as<program_node>(r.program);
    const auto& fn = r.p->arena().get_as<function_node>(prog.declarations[0]);
    const auto& blk = r.p->arena().get_as<block_node>(fn.body);
    EXPECT_EQ(blk.statements.size(), 2u);
    const auto& iff = r.p->arena().get_as<if_stmt_node>(blk.statements[0]);
    EXPECT_TRUE(r.p->arena().holds<bool_literal_node>(iff.condition));
    EXPECT_FALSE(iff.else_block.is_valid());
}

TEST(parse_if_else) {
    auto r = parse_source(
        "u32 main() {\n"
        "    if (true) { return 0u32; } else { return 1u32; }\n"
        "}");
    EXPECT_FALSE(r.diag->has_errors());
    const auto& prog = r.p->arena().get_as<program_node>(r.program);
    const auto& fn = r.p->arena().get_as<function_node>(prog.declarations[0]);
    const auto& blk = r.p->arena().get_as<block_node>(fn.body);
    const auto& iff = r.p->arena().get_as<if_stmt_node>(blk.statements[0]);
    EXPECT_TRUE(iff.else_block.is_valid());
    EXPECT_TRUE(r.p->arena().holds<block_node>(iff.else_block));
}

TEST(parse_if_else_if_chain) {
    auto r = parse_source(
        "u32 main() {\n"
        "    if (true) { return 0u32; }\n"
        "    else if (false) { return 1u32; }\n"
        "    else { return 2u32; }\n"
        "}");
    EXPECT_FALSE(r.diag->has_errors());
    const auto& prog = r.p->arena().get_as<program_node>(r.program);
    const auto& fn = r.p->arena().get_as<function_node>(prog.declarations[0]);
    const auto& blk = r.p->arena().get_as<block_node>(fn.body);
    const auto& iff = r.p->arena().get_as<if_stmt_node>(blk.statements[0]);
    EXPECT_TRUE(r.p->arena().holds<if_stmt_node>(iff.else_block));
}

TEST(parse_while) {
    auto r = parse_source("u32 main() { while (false) { } return 0u32; }");
    EXPECT_FALSE(r.diag->has_errors());
    const auto& prog = r.p->arena().get_as<program_node>(r.program);
    const auto& fn = r.p->arena().get_as<function_node>(prog.declarations[0]);
    const auto& blk = r.p->arena().get_as<block_node>(fn.body);
    EXPECT_EQ(blk.statements.size(), 2u);
    EXPECT_TRUE(r.p->arena().holds<while_stmt_node>(blk.statements[0]));
}

TEST(parse_assign) {
    auto r = parse_source("u32 main() { mut u32 x = 0u32; x = 5u32; return x; }");
    EXPECT_FALSE(r.diag->has_errors());
    const auto& prog = r.p->arena().get_as<program_node>(r.program);
    const auto& fn = r.p->arena().get_as<function_node>(prog.declarations[0]);
    const auto& blk = r.p->arena().get_as<block_node>(fn.body);
    EXPECT_EQ(blk.statements.size(), 3u);
    EXPECT_TRUE(r.p->arena().holds<assign_stmt_node>(blk.statements[1]));
}

TEST(parse_bool_literals) {
    auto r = parse_source("u32 main() { bool a = true; bool b = false; return 0u32; }");
    EXPECT_FALSE(r.diag->has_errors());
}

TEST(parse_error_if_missing_paren) {
    auto r = parse_source("u32 main() { if true { } return 0u32; }");
    EXPECT_TRUE(r.diag->has_errors());
}

TEST(parse_error_while_missing_body) {
    auto r = parse_source("u32 main() { while (false) return 0u32; }");
    EXPECT_TRUE(r.diag->has_errors());
}

TEST(parse_call_no_args) {
    auto r = parse_source("u32 f() { return 0u32; } u32 main() { return f(); }");
    EXPECT_FALSE(r.diag->has_errors());
    const auto& prog = r.p->arena().get_as<program_node>(r.program);
    const auto& main_fn = r.p->arena().get_as<function_node>(prog.declarations[1]);
    const auto& blk = r.p->arena().get_as<block_node>(main_fn.body);
    const auto& ret = r.p->arena().get_as<return_stmt_node>(blk.statements[0]);
    EXPECT_TRUE(r.p->arena().holds<call_expr_node>(ret.value));
}

TEST(parse_call_with_args) {
    auto r = parse_source("u32 main() { return add(1u32, 2u32); }");
    EXPECT_FALSE(r.diag->has_errors());
    const auto& prog = r.p->arena().get_as<program_node>(r.program);
    const auto& fn = r.p->arena().get_as<function_node>(prog.declarations[0]);
    const auto& blk = r.p->arena().get_as<block_node>(fn.body);
    const auto& ret = r.p->arena().get_as<return_stmt_node>(blk.statements[0]);
    const auto& call = r.p->arena().get_as<call_expr_node>(ret.value);
    EXPECT_EQ(call.args.size(), 2u);
}

TEST(parse_call_nested) {
    auto r = parse_source("u32 main() { return f(g(1u32)); }");
    EXPECT_FALSE(r.diag->has_errors());
}

TEST(parse_default_init) {
    auto r = parse_source("u32 main() { u32 x = default; return x; }");
    EXPECT_FALSE(r.diag->has_errors());
    const auto& prog = r.p->arena().get_as<program_node>(r.program);
    const auto& fn = r.p->arena().get_as<function_node>(prog.declarations[0]);
    const auto& blk = r.p->arena().get_as<block_node>(fn.body);
    const auto& v = r.p->arena().get_as<var_decl_node>(blk.statements[0]);
    EXPECT_TRUE(r.p->arena().holds<default_init_node>(v.initializer));
}

TEST(parse_bit_expr) {
    auto r = parse_source("u32 main() { return bit(3u32); }");
    EXPECT_FALSE(r.diag->has_errors());
    const auto& prog = r.p->arena().get_as<program_node>(r.program);
    const auto& fn = r.p->arena().get_as<function_node>(prog.declarations[0]);
    const auto& blk = r.p->arena().get_as<block_node>(fn.body);
    const auto& ret = r.p->arena().get_as<return_stmt_node>(blk.statements[0]);
    EXPECT_TRUE(r.p->arena().holds<bit_expr_node>(ret.value));
}

TEST(parse_error_bit_missing_paren) {
    auto r = parse_source("u32 main() { return bit 3u32; }");
    EXPECT_TRUE(r.diag->has_errors());
}



int main() {
    return test::run_all();
}

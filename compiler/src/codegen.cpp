
#include "rift/codegen.h"
#include "rift/types.h"

#include <cstdint>
#include <format>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>



// FORWARD DECLARATIONS ================================================================================================

namespace rift {

    // CONSTANTS =======================================================================================================

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    struct expr_value {
        std::string                             text;
        type_id                                 type = invalid_type;
    };


    struct codegen_context {

        const ast_arena&                        arena;
        const sema_result&                      sema;
        std::ostream&                           os;
        std::uint32_t                           next_temp = 0;
        std::uint32_t                           next_label = 0;
        bool                                    terminated = false;

        std::unordered_map<std::string_view, std::string> vars;

        codegen_context(const ast_arena& a, const sema_result& s, std::ostream& o) noexcept
            : arena(a), sema(s), os(o) {}

        std::string new_temp() { return "%t" + std::to_string(next_temp++); }
        std::string new_label(std::string_view hint) {
            return std::string(hint) + '_' + std::to_string(next_label++);
        }
    };

    // STATIC VARIABLES ================================================================================================

    // INTERNAL TEMPLATE DECLARATION ===================================================================================

    // INTERNAL FUNCTION DECLARATION ===================================================================================

    std::string_view llvm_type_name(type_id t) noexcept;

    type_id resolve_type_node(const ast_arena& arena, node_id id);

    std::string decimal_literal_value(std::string_view lexeme);

    std::string default_value_text(type_id t);

    // Statements ------------------------------------------------------------------------------------------------------

    void emit_block_body(codegen_context& ctx, const block_node& block);

    void emit_return(codegen_context& ctx, const return_stmt_node& n);

    void emit_var_decl(codegen_context& ctx, const var_decl_node& n);

    void emit_assign(codegen_context& ctx, const assign_stmt_node& n);

    void emit_if(codegen_context& ctx, const if_stmt_node& n);

    void emit_while(codegen_context& ctx, const while_stmt_node& n);

    void emit_statement(codegen_context& ctx, node_id id);

    // Expressions -----------------------------------------------------------------------------------------------------

    expr_value emit_int_literal(codegen_context& ctx, node_id id, const int_literal_node& n);

    expr_value emit_bool_literal(codegen_context& ctx, node_id id, const bool_literal_node& n);

    expr_value emit_identifier(codegen_context& ctx, node_id id, const identifier_node& n);

    expr_value emit_unary(codegen_context& ctx, const unary_expr_node& n, type_id result_type);

    expr_value emit_binary(codegen_context& ctx, const binary_expr_node& n, type_id result_type);

    expr_value emit_call(codegen_context& ctx, node_id id, const call_expr_node& n);

    expr_value emit_default_init(codegen_context& ctx, node_id id);

    expr_value emit_bit(codegen_context& ctx, node_id id, const bit_expr_node& n);

    expr_value emit_expr(codegen_context& ctx, node_id id);

    // Functions -------------------------------------------------------------------------------------------------------

    void emit_function(std::ostream& os, const ast_arena& arena, const function_node& fn, const sema_result& sema);

    // INTERNAL TEMPLATE IMPLEMENTATION ================================================================================

    // INTERNAL FUNCTION IMPLEMENTATION ================================================================================

    std::string_view llvm_type_name(type_id t) noexcept {

        const auto info = type_info_of(t);
        if (info.is_void) return "void";
        if (info.is_bool) return "i1";
        if (info.is_float)
            return info.size_bytes == 4 ? "float" : "double";
        if (info.is_integer) {
            switch (info.size_bytes) {
                case 1:  return "i8";
                case 2:  return "i16";
                case 4:  return "i32";
                case 8:  return "i64";
                case 16: return "i128";
            }
        }
        return "<invalid>";
    }


    type_id resolve_type_node(const ast_arena& arena, node_id id) {

        if (!id.is_valid()) return invalid_type;
        return std::visit([&](const auto& n) -> type_id {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, primitive_type_node>)
                return primitive_type_id(n.type_kind);
            else
                return invalid_type;
        }, arena.get(id));
    }


    // M4 duplicate of the sema-side literal parser, trimmed to uint64_t (see TODO below). Consolidate when M5 starts touching 128-bit literals
    std::string decimal_literal_value(std::string_view lexeme) {

        std::size_t end = lexeme.size();
        for (std::string_view suf : { "u128", "i128", "u64", "i64", "u32", "i32", "u16", "i16", "u8", "i8" }) {
            if (lexeme.size() >= suf.size() && lexeme.substr(lexeme.size() - suf.size()) == suf) {
                end = lexeme.size() - suf.size();
                break;
            }
        }

        unsigned       base = 10;
        std::size_t    pos  = 0;
        if (end >= 2 && lexeme[0] == '0') {
            const char p1 = lexeme[1];
            if (p1 == 'x' || p1 == 'X') { base = 16; pos = 2; }
            else if (p1 == 'o' || p1 == 'O') {
                base = 8;
                pos = 2;
            }
            else if (p1 == 'b' || p1 == 'B') {
                base = 2;
                pos = 2;
            }
        }

        std::uint64_t value = 0;
        for (; pos < end; ++pos) {
            const char c = lexeme[pos];
            if (c == '_')
                continue;
            unsigned digit = 0;
            if (c >= '0' && c <= '9')
                digit = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                digit = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                digit = static_cast<unsigned>(c - 'A' + 10);
            else
                break;
            // TODO(M5): route through the shared parser to handle values > 2^64
            value = value * base + digit;
        }
        return std::to_string(value);
    }


    std::string default_value_text(type_id t) {

        const auto info = type_info_of(t);
        if (info.is_bool)
            return "false";
        if (info.is_float)
            return info.size_bytes == 4 ? "0.0" : "0.0";
        return "0";
    }

    // Statements ------------------------------------------------------------------------------------------------------

    void emit_block_body(codegen_context& ctx, const block_node& block) {

        for (const auto s_id : block.statements) {
            if (ctx.terminated) return;   // dead code after terminator
            emit_statement(ctx, s_id);
        }
    }


    void emit_return(codegen_context& ctx, const return_stmt_node& n) {

        if (!n.value.is_valid()) {
            ctx.os << "  ret void\n";
        } else {
            const auto v = emit_expr(ctx, n.value);
            ctx.os << "  ret " << llvm_type_name(v.type) << ' ' << v.text << '\n';
        }
        ctx.terminated = true;
    }


    void emit_var_decl(codegen_context& ctx, const var_decl_node& n) {

        const type_id t = resolve_type_node(ctx.arena, n.type);
        const std::string tn(llvm_type_name(t));
        const std::string alloca_reg = "%" + std::string(n.name);

        ctx.os << "  " << alloca_reg << " = alloca " << tn << '\n';

        const auto v = emit_expr(ctx, n.initializer);
        ctx.os << "  store " << tn << ' ' << v.text << ", ptr " << alloca_reg << '\n';

        ctx.vars[n.name] = alloca_reg;
    }


    void emit_assign(codegen_context& ctx, const assign_stmt_node& n) {

        const auto& target = ctx.arena.get_as<identifier_node>(n.target);
        auto it = ctx.vars.find(target.name);
        if (it == ctx.vars.end())
            return;

        const auto v = emit_expr(ctx, n.value);
        ctx.os << "  store " << llvm_type_name(v.type) << ' ' << v.text
                << ", ptr " << it->second << '\n';
    }


    void emit_if(codegen_context& ctx, const if_stmt_node& n) {

        const std::string then_label = ctx.new_label("if_then");
        const bool has_else = n.else_block.is_valid();

        std::string else_label;
        std::string merge_label;
        if (has_else)
            else_label = ctx.new_label("if_else");
        else
            merge_label = ctx.new_label("if_merge");

        const auto cond = emit_expr(ctx, n.condition);
        const std::string false_target = has_else ? else_label : merge_label;
        ctx.os << "  br i1 " << cond.text << ", label %" << then_label
                << ", label %" << false_target << '\n';

        ctx.os << then_label << ":\n";
        ctx.terminated = false;
        emit_block_body(ctx, ctx.arena.get_as<block_node>(n.then_block));
        const bool then_terminated = ctx.terminated;

        if (has_else) {

            if (!then_terminated) {
                if (merge_label.empty())
                    merge_label = ctx.new_label("if_merge");
                ctx.os << "  br label %" << merge_label << '\n';
            }

            ctx.os << else_label << ":\n";
            ctx.terminated = false;

            if (ctx.arena.holds<block_node>(n.else_block))
                emit_block_body(ctx, ctx.arena.get_as<block_node>(n.else_block));
            else if (ctx.arena.holds<if_stmt_node>(n.else_block))
                emit_if(ctx, ctx.arena.get_as<if_stmt_node>(n.else_block));

            const bool else_terminated = ctx.terminated;

            if (!else_terminated) {
                if (merge_label.empty())
                    merge_label = ctx.new_label("if_merge");
                ctx.os << "  br label %" << merge_label << '\n';
            }

            if (!merge_label.empty()) {
                ctx.os << merge_label << ":\n";
                ctx.terminated = false;
            } else {
                ctx.terminated = true;
            }

        } else {
            if (!then_terminated)
                ctx.os << "  br label %" << merge_label << '\n';
            ctx.os << merge_label << ":\n";
            ctx.terminated = false;
        }
    }


    void emit_while(codegen_context& ctx, const while_stmt_node& n) {

        const std::string cond_label = ctx.new_label("while_cond");
        const std::string body_label = ctx.new_label("while_body");
        const std::string end_label  = ctx.new_label("while_end");

        ctx.os << "  br label %" << cond_label << '\n';

        ctx.os << cond_label << ":\n";
        ctx.terminated = false;
        const auto cond = emit_expr(ctx, n.condition);
        ctx.os << "  br i1 " << cond.text << ", label %" << body_label
                << ", label %" << end_label << '\n';

        ctx.os << body_label << ":\n";
        ctx.terminated = false;
        emit_block_body(ctx, ctx.arena.get_as<block_node>(n.body));
        if (!ctx.terminated)
            ctx.os << "  br label %" << cond_label << '\n';

        ctx.os << end_label << ":\n";
        ctx.terminated = false;
    }


    void emit_statement(codegen_context& ctx, node_id id) {

        std::visit([&](const auto& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, return_stmt_node>)
                emit_return(ctx, n);
            else if constexpr (std::is_same_v<T, var_decl_node>)
                emit_var_decl(ctx, n);
            else if constexpr (std::is_same_v<T, assign_stmt_node>)
                emit_assign(ctx, n);
            else if constexpr (std::is_same_v<T, if_stmt_node>)
                emit_if(ctx, n);
            else if constexpr (std::is_same_v<T, while_stmt_node>)
                emit_while(ctx, n);
        }, ctx.arena.get(id));
    }

    // Expressions -----------------------------------------------------------------------------------------------------

    expr_value emit_int_literal(codegen_context& ctx, node_id id, const int_literal_node& n) {

        return { decimal_literal_value(n.lexeme), ctx.sema.expr_type(id) };
    }


    expr_value emit_bool_literal(codegen_context& ctx, node_id id, const bool_literal_node& n) {

        return { n.value ? "true" : "false", ctx.sema.expr_type(id) };
    }


    expr_value emit_identifier(codegen_context& ctx, node_id id, const identifier_node& n) {

        const type_id t = ctx.sema.expr_type(id);

        auto it = ctx.vars.find(n.name);
        if (it == ctx.vars.end())
            return { "0", t };

        const std::string temp = ctx.new_temp();
        ctx.os << "  " << temp << " = load " << llvm_type_name(t)
                << ", ptr " << it->second << '\n';
        return { temp, t };
    }


    expr_value emit_unary(codegen_context& ctx, const unary_expr_node& n, type_id result_type) {

        const auto operand = emit_expr(ctx, n.operand);
        if (!operand.type) return { "0", result_type };

        const std::string tn(llvm_type_name(operand.type));

        if (n.op == token_kind::plus)
            return operand;

        const std::string temp = ctx.new_temp();

        if (n.op == token_kind::minus) {
            ctx.os << "  " << temp << " = sub " << tn << " 0, " << operand.text << '\n';
        } else if (n.op == token_kind::bang) {
            ctx.os << "  " << temp << " = xor i1 " << operand.text << ", true\n";
        } else if (n.op == token_kind::tilde) {
            ctx.os << "  " << temp << " = xor " << tn << ' ' << operand.text << ", -1\n";
        } else {
            return operand;
        }

        return { temp, result_type };
    }


    expr_value emit_binary(codegen_context& ctx, const binary_expr_node& n, type_id result_type) {

        const auto lhs = emit_expr(ctx, n.lhs);
        const auto rhs = emit_expr(ctx, n.rhs);
        if (!lhs.type || !rhs.type) return { "0", result_type };

        const std::string tn(llvm_type_name(lhs.type));
        const std::string temp = ctx.new_temp();

        auto emit_op = [&](std::string_view op) {
            ctx.os << "  " << temp << " = " << op << ' ' << tn << ' '
                    << lhs.text << ", " << rhs.text << '\n';
        };

        switch (n.op) {

            case token_kind::plus:      emit_op("add");  break;
            case token_kind::minus:     emit_op("sub");  break;
            case token_kind::star:      emit_op("mul");  break;

            case token_kind::slash:
                emit_op(type_info_of(lhs.type).is_signed ? "sdiv" : "udiv");
                break;
            case token_kind::percent:
                emit_op(type_info_of(lhs.type).is_signed ? "srem" : "urem");
                break;

            case token_kind::amp:       emit_op("and");  break;
            case token_kind::pipe:      emit_op("or");   break;
            case token_kind::caret:     emit_op("xor");  break;
            case token_kind::lt_lt:     emit_op("shl");  break;
            case token_kind::gt_gt:
                emit_op(type_info_of(lhs.type).is_signed ? "ashr" : "lshr");
                break;

            case token_kind::eq_eq:     emit_op("icmp eq");  break;
            case token_kind::bang_eq:   emit_op("icmp ne");  break;
            case token_kind::lt:        emit_op("icmp slt"); break;
            case token_kind::lt_eq:     emit_op("icmp sle"); break;
            case token_kind::gt:        emit_op("icmp sgt"); break;
            case token_kind::gt_eq:     emit_op("icmp sge"); break;

            case token_kind::amp_amp:
                ctx.os << "  " << temp << " = and i1 " << lhs.text << ", " << rhs.text << '\n';
                break;
            case token_kind::pipe_pipe:
                ctx.os << "  " << temp << " = or  i1 " << lhs.text << ", " << rhs.text << '\n';
                break;

            default:
                return { "0", result_type };
        }

        return { temp, result_type };
    }


    expr_value emit_call(codegen_context& ctx, node_id id, const call_expr_node& n) {

        const type_id ret_type = ctx.sema.expr_type(id);
        const auto& callee = ctx.arena.get_as<identifier_node>(n.callee);

        std::string args_ir;
        for (std::size_t i = 0; i < n.args.size(); ++i) {
            if (i) args_ir += ", ";
            const auto a = emit_expr(ctx, n.args[i]);
            args_ir += std::string(llvm_type_name(a.type)) + " " + a.text;
        }

        if (is_void(ret_type)) {
            ctx.os << "  call void @" << callee.name << '(' << args_ir << ")\n";
            return { "", ret_type };
        }

        const std::string temp = ctx.new_temp();
        ctx.os << "  " << temp << " = call " << llvm_type_name(ret_type)
                << " @" << callee.name << '(' << args_ir << ")\n";
        return { temp, ret_type };
    }


    expr_value emit_default_init(codegen_context& ctx, node_id id) {

        const type_id t = ctx.sema.expr_type(id);
        const auto info = type_info_of(t);
        if (info.is_bool)  return { "false", t };
        if (info.is_float) return { "0.0",   t };
        return { "0", t };
    }


    expr_value emit_bit(codegen_context& ctx, node_id id, const bit_expr_node& n) {

        const type_id t = ctx.sema.expr_type(id);
        const auto operand = emit_expr(ctx, n.operand);
        const std::string tn(llvm_type_name(t));
        const std::string temp = ctx.new_temp();
        ctx.os << "  " << temp << " = shl " << tn << " 1, " << operand.text << '\n';
        return { temp, t };
    }


    expr_value emit_expr(codegen_context& ctx, node_id id) {

        if (!id.is_valid()) return { "0", invalid_type };
        const type_id result_type = ctx.sema.expr_type(id);

        return std::visit([&](const auto& n) -> expr_value {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, int_literal_node>)
                return emit_int_literal(ctx, id, n);
            else if constexpr (std::is_same_v<T, bool_literal_node>)
                return emit_bool_literal(ctx, id, n);
            else if constexpr (std::is_same_v<T, identifier_node>)
                return emit_identifier(ctx, id, n);
            else if constexpr (std::is_same_v<T, unary_expr_node>)
                return emit_unary(ctx, n, result_type);
            else if constexpr (std::is_same_v<T, binary_expr_node>)
                return emit_binary(ctx, n, result_type);
            else if constexpr (std::is_same_v<T, call_expr_node>)
                return emit_call(ctx, id, n);
            else if constexpr (std::is_same_v<T, default_init_node>)
                return emit_default_init(ctx, id);
            else if constexpr (std::is_same_v<T, bit_expr_node>)
                return emit_bit(ctx, id, n);
            else
                return { "0", result_type };
        }, ctx.arena.get(id));
    }

    // Functions -------------------------------------------------------------------------------------------------------

    void emit_function(std::ostream& os, const ast_arena& arena, const function_node& fn, const sema_result& sema) {

        const type_id ret_type = resolve_type_node(arena, fn.return_type);
        const std::string ret_tn(llvm_type_name(ret_type));

        os << "define " << ret_tn << " @" << fn.name << '(';

        bool first = true;
        for (const auto p_id : fn.params) {
            const auto& p = arena.get_as<parameter_node>(p_id);
            const type_id p_type = resolve_type_node(arena, p.type);
            if (!first) os << ", ";
            first = false;
            os << llvm_type_name(p_type) << " %" << p.name;
        }
        os << ") {\n";
        os << "entry:\n";

        codegen_context ctx{ arena, sema, os };

        // Alloca every parameter so `emit_identifier` can load it. `mut` is purely a sema property, for codegen, all
        // parameters are spilled and treated identically
        for (const auto p_id : fn.params) {
            const auto& p = arena.get_as<parameter_node>(p_id);
            const type_id p_type = resolve_type_node(arena, p.type);
            const std::string tn(llvm_type_name(p_type));
            const std::string alloca_reg = "%" + std::string(p.name) + ".addr";
            os << "  " << alloca_reg << " = alloca " << tn << '\n';
            os << "  store " << tn << " %" << p.name << ", ptr " << alloca_reg << '\n';
            ctx.vars[p.name] = alloca_reg;
        }

        const auto& body = arena.get_as<block_node>(fn.body);
        emit_block_body(ctx, body);

        if (!ctx.terminated) {
            if (is_void(ret_type))
                os << "  ret void\n";
            else
                os << "  ret " << ret_tn << ' ' << default_value_text(ret_type) << '\n';
        }

        os << "}\n\n";
    }

    // TEMPLATE IMPLEMENTATION =========================================================================================

    // FUNCTION IMPLEMENTATION =========================================================================================

    // CLASS IMPLEMENTATION ============================================================================================

    // FUNCTION IMPLEMENTATION =========================================================================================

    void emit_llvm_ir(const ast_arena& arena, node_id program, const sema_result& sema, diagnostic_engine& /*diag*/, std::ostream& os) {

        os << "; ModuleID = 'rift'\n";
        os << "source_filename = \"<rift>\"\n\n";

        const auto& prog = arena.get_as<program_node>(program);
        for (const auto fn_id : prog.declarations) {
            if (!arena.holds<function_node>(fn_id))
                continue;
            emit_function(os, arena, arena.get_as<function_node>(fn_id), sema);
        }
    }


    std::string emit_llvm_ir_to_string(const ast_arena& arena, node_id program, const sema_result& sema, diagnostic_engine& diag) {

        std::ostringstream os;
        emit_llvm_ir(arena, program, sema, diag, os);
        return os.str();
    }

    // CLASS IMPLEMENTATION ============================================================================================

    // CLASS PUBLIC ====================================================================================================

    // CLASS PROTECTED =================================================================================================

    // CLASS PRIVATE ===================================================================================================

}

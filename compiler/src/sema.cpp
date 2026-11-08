
#include "rift/sema.h"
#include "rift/diagnostic.h"

#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <algorithm>



// FORWARD DECLARATIONS ================================================================================================

namespace rift {

    // CONSTANTS =======================================================================================================

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    // `unsigned __int128` triggers -Wpedantic on GCC/Clang. The literal checker needs it to hold magnitudes up to 2^128 - 1
    // without overflow Scope the suppression tightly around the ~50 lines that use it
    #if defined(__GNUC__) || defined(__clang__)
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wpedantic"
    #endif

    struct literal_value {

        bool                    ok = false;
        bool                    overflowed = false;
        unsigned __int128       magnitude = 0;
    };

    #if defined(__GNUC__) || defined(__clang__)
        #pragma GCC diagnostic pop
    #endif


    // `expr_result.flexible` marks nodes that could take on a different type if the surrounding context demanded it. Unsuffixed integer
    // literals are flexible; explicit-suffix literals, variables, and arithmetic results whose operands were not flexible are not
    struct expr_result {

        type_id                     type = invalid_type;
        bool                        flexible = false;
        symbol_id                   identifier_symbol = invalid_symbol;
    };


    class analyzer {
    public:

        analyzer(const ast_arena& arena, diagnostic_engine& diag, sema_result& result);

        void run(node_id program_id);

    private:

        // Passes
        void collect_declarations(const program_node& prog);
        void check_function(const function_node& fn, symbol_id fn_sym);

        // Statements
        void check_statement(node_id id, type_id expected_return_type);
        void check_return(const return_stmt_node& node, type_id expected_return_type);
        void check_var_decl(const var_decl_node& node);

        // Expressions
        expr_result check_expr(node_id id, type_id expected);
        expr_result check_int_literal(const int_literal_node& node, type_id expected);
        expr_result check_identifier(const identifier_node& node);
        expr_result check_unary(const unary_expr_node& node, type_id expected);
        expr_result check_binary(const binary_expr_node& node, type_id expected);
        expr_result check_bool_literal(const bool_literal_node& node);
        expr_result check_call(const call_expr_node& node, type_id expected);
        expr_result check_default_init(const default_init_node& node, type_id expected);
        expr_result check_bit(const bit_expr_node& node, type_id expected);

        void check_assign(const assign_stmt_node& node);
        void check_if_stmt(const if_stmt_node& node, type_id expected_return_type);
        void check_while_stmt(const while_stmt_node& node, type_id expected_return_type);

        // Helpers
        type_id resolve_type(node_id type_node);
        std::string_view type_name(type_id t) const { return type_info_of(t).name; }

        void report_type_mismatch(source_span span, type_id expected, type_id found, std::string_view context);
        void report_unknown_name(source_span span, std::string_view name);

        // State
        const ast_arena&            m_arena;
        diagnostic_engine&          m_diag;
        sema_result&                m_result;
        symbol_id                   m_main_sym = invalid_symbol;
    };

    // STATIC VARIABLES ================================================================================================

    // INTERNAL TEMPLATE DECLARATION ===================================================================================

    // INTERNAL FUNCTION DECLARATION ===================================================================================

    // `unsigned __int128` triggers -Wpedantic on GCC/Clang. The literal checker needs it to hold magnitudes up to 2^128 - 1
    // without overflow Scope the suppression tightly around the ~50 lines that use it
    #if defined(__GNUC__) || defined(__clang__)
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wpedantic"
    #endif

    literal_value parse_int_literal(std::string_view lexeme);


    std::string format_magnitude(unsigned __int128 value);


    bool check_literal_range(type_id type, unsigned __int128 magnitude, source_span span, diagnostic_engine& diag);

    // INTERNAL TEMPLATE IMPLEMENTATION ================================================================================

    // INTERNAL FUNCTION IMPLEMENTATION ================================================================================

    literal_value parse_int_literal(std::string_view lexeme) {

        std::size_t pos = 0;
        unsigned    base = 10;

        if (lexeme.size() >= 2 && lexeme[0] == '0') {
            switch (lexeme[1]) {
                case 'x':   [[fallthrough]];
                case 'X':
                    base = 16;
                    pos = 2;
                    break;
                case 'o':   [[fallthrough]];
                case 'O':
                    base = 8;
                    pos = 2;
                    break;
                case 'b':   [[fallthrough]];
                case 'B':
                    base = 2;
                    pos = 2;
                    break;
                default:
                    break;
            }
        }

        unsigned __int128 value = 0;
        unsigned __int128 limit = ~static_cast<unsigned __int128>(0);
        bool overflowed = false;

        for (; pos < lexeme.size(); ++pos) {
            const char c = lexeme[pos];
            if (c == '_')
                continue;

            unsigned digit;
            if (c >= '0' && c <= '9')
                digit = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                digit = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                digit = static_cast<unsigned>(c - 'A' + 10);
            else
                break;   // start of suffix

            if (digit >= base)
                break;

            if (!overflowed) {
                if (value > (limit - digit) / base) {
                    overflowed = true;
                    value = 0;
                } else {
                    value = value * base + digit;
                }
            }
        }

        return literal_value{ !overflowed, overflowed, value };
    }


    std::string format_magnitude(unsigned __int128 value) {

        if (value == 0)
            return "0";

        std::string out;
        while (value > 0) {
            out.push_back(static_cast<char>('0' + static_cast<unsigned>(value % 10)));
            value /= 10;
        }
        std::reverse(out.begin(), out.end());
        return out;
    }


    bool check_literal_range(type_id type, unsigned __int128 magnitude, source_span span, diagnostic_engine& diag) {

        const auto range = int_range_of(type);
        const auto info = type_info_of(type);

        if (!range.valid)
            return true;   // 128-bit or non-integer: no range check in M3

        if (range.is_signed) {
            const auto max = static_cast<unsigned __int128>(range.max_s);
            if (magnitude > max) {
                diag.error(diagnostic_code::integer_out_of_range, span, std::format("integer literal {} does not fit in '{}'",
                    format_magnitude(magnitude), info.name));
                return false;
            }
        } else {
            const auto max = static_cast<unsigned __int128>(range.max_u);
            if (magnitude > max) {
                diag.error(diagnostic_code::integer_out_of_range, span, std::format("integer literal {} does not fit in '{}'",
                    format_magnitude(magnitude), info.name));
                return false;
            }
        }
        return true;
    }

    #if defined(__GNUC__) || defined(__clang__)
        #pragma GCC diagnostic pop
    #endif

    // TEMPLATE IMPLEMENTATION =========================================================================================

    // FUNCTION IMPLEMENTATION =========================================================================================

    sema_result analyze(const ast_arena& arena, node_id program, source_manager& /*sm*/, file_id /*file*/, diagnostic_engine& diag) {

        sema_result result;
        result.program = program;
        analyzer a(arena, diag, result);
        a.run(program);
        return result;
    }

    // CLASS IMPLEMENTATION ============================================================================================

    analyzer::analyzer(const ast_arena& arena, diagnostic_engine& diag, sema_result& result)
        : m_arena(arena), m_diag(diag), m_result(result) {}

    // CLASS PUBLIC ====================================================================================================

    void analyzer::run(node_id program_id) {

        const auto& prog = m_arena.get_as<program_node>(program_id);

        collect_declarations(prog);

        if (m_main_sym == invalid_symbol) {
            diagnostic d;
            d.level = severity::error;
            d.code = diagnostic_code::missing_main;
            d.span = m_arena.span_of(program_id);
            d.message = "missing 'main' function";
            d.help = "add `u32 main() { ... }`";
            m_diag.report(std::move(d));
            return;
        }

        for (const auto fn_id : prog.declarations) {

            if (!m_arena.holds<function_node>(fn_id))
                continue;
            const auto& fn = m_arena.get_as<function_node>(fn_id);
            const auto fn_sym = m_result.symbols().lookup_function(fn.name);
            check_function(fn, fn_sym);
        }
    }

    // CLASS PRIVATE ===================================================================================================

    // Pass 1 ----------------------------------------------------------------------------------------------------------

    void analyzer::collect_declarations(const program_node& prog) {

        for (const auto fn_id : prog.declarations) {

            if (!m_arena.holds<function_node>(fn_id))
                continue;
                
            const auto& fn = m_arena.get_as<function_node>(fn_id);
            const type_id return_type = resolve_type(fn.return_type);

            // main's shape: u32 main(), no parameters. §7.4 also permits
            // std::exit_code and sized integers, but M3 only has u32
            if (fn.name == "main") {
                if (return_type != invalid_type &&
                    return_type != primitive_type_id(token_kind::prim_u32)) {
                    diagnostic d;
                    d.level = severity::error;
                    d.code = diagnostic_code::invalid_main_return_type;
                    d.span = m_arena.span_of(fn.return_type);
                    d.message = std::format("'main' must return 'u32', found '{}'", type_name(return_type));
                    d.help = "change the return type to `u32`";
                    m_diag.report(std::move(d));
                }
                if (!fn.params.empty()) {
                    diagnostic d;
                    d.level = severity::error;
                    d.code = diagnostic_code::invalid_main_signature;
                    d.span = fn.span;
                    d.message = "'main' takes no parameters";
                    d.help = "remove the parameter list";
                    m_diag.report(std::move(d));
                }
            }

            if (m_result.symbols().lookup_function(fn.name) != invalid_symbol) {
                diagnostic d;
                d.level = severity::error;
                d.code = diagnostic_code::duplicate_function;
                d.span = fn.span;
                d.message = std::format("function '{}' is already defined", fn.name);
                m_diag.report(std::move(d));
                continue;
            }

            symbol fn_sym;
            fn_sym.kind = symbol_kind::function;
            fn_sym.name = fn.name;
            fn_sym.type = return_type;
            fn_sym.return_type = return_type;
            fn_sym.span = fn.span;
            fn_sym.definition = fn_id;

            const symbol_id fn_sym_id = m_result.symbols().declare_global_function(fn.name, std::move(fn_sym));
            if (fn.name == "main")
                m_main_sym = fn_sym_id;
        }
    }

    // Pass 2 ----------------------------------------------------------------------------------------------------------

    void analyzer::check_function(const function_node& fn, symbol_id fn_sym) {

        if (fn_sym == invalid_symbol)
            return;

        const type_id return_type = m_result.symbols().get(fn_sym).return_type;

        m_result.symbols().push_scope();

        for (const auto p_id : fn.params) {
            const auto& p = m_arena.get_as<parameter_node>(p_id);
            const type_id p_type = resolve_type(p.type);

            symbol s;
            s.kind = symbol_kind::parameter;
            s.name = p.name;
            s.type = p_type;      // invalid_type is fine; callers skip
            s.is_mut = p.is_mut;
            s.span = p.span;

            const symbol_id id = m_result.symbols().declare(p.name, std::move(s));
            if (id == invalid_symbol) {
                diagnostic d;
                d.level = severity::error;
                d.code = diagnostic_code::duplicate_parameter;
                d.span = p.span;
                d.message = std::format("parameter '{}' is already declared in this function", p.name);
                m_diag.report(std::move(d));
            }
        }

        if (fn.body.is_valid()) {
            const auto& body = m_arena.get_as<block_node>(fn.body);
            for (const auto s_id : body.statements)
                check_statement(s_id, return_type);
        }

        m_result.symbols().pop_scope();
    }

    // Statements ------------------------------------------------------------------------------------------------------

    void analyzer::check_statement(node_id id, type_id expected_return_type) {

        std::visit([&](const auto& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, return_stmt_node>)
                check_return(n, expected_return_type);
            else if constexpr (std::is_same_v<T, var_decl_node>)
                check_var_decl(n);
            else if constexpr (std::is_same_v<T, assign_stmt_node>)
                check_assign(n);
            else if constexpr (std::is_same_v<T, if_stmt_node>)
                check_if_stmt(n, expected_return_type);
            else if constexpr (std::is_same_v<T, while_stmt_node>)
                check_while_stmt(n, expected_return_type);
        }, m_arena.get(id));
    }


    void analyzer::check_return(const return_stmt_node& node, type_id expected_return_type) {

        if (!node.value.is_valid())
            return;

        if (expected_return_type == invalid_type)
            return;   // function's return type is unknown; skip to avoid cascade

        const auto result = check_expr(node.value, expected_return_type);

        if (result.type == invalid_type)
            return;

        if (result.type != expected_return_type)
            report_type_mismatch(m_arena.span_of(node.value), expected_return_type, result.type, "return value");
    }


    void analyzer::check_var_decl(const var_decl_node& node) {

        const type_id declared = resolve_type(node.type);

        // If there's an initializer, check it (even if the declared type is invalid — we still want to type-check literals inside for
        // their own errors)
        if (declared != invalid_type && node.initializer.is_valid()) {
            const auto result = check_expr(node.initializer, declared);
            if (result.type != invalid_type && result.type != declared)
                report_type_mismatch(m_arena.span_of(node.initializer), declared, result.type, "variable initializer");

        } else if (declared == invalid_type && node.initializer.is_valid()) {
            // Still walk the initializer, ignoring any type mismatch — this surfaces inner errors (bad literals, unknown identifiers) that
            // would otherwise be masked by the type error
            (void)check_expr(node.initializer, invalid_type);
        }

        // Always declare the name, even with an invalid type, so subsequent uses don't trigger "unknown identifier"
        symbol s;
        s.kind = symbol_kind::local;
        s.name = node.name;
        s.type = declared;
        s.is_mut = node.is_mut;
        s.span = node.span;

        const symbol_id id = m_result.symbols().declare(node.name, std::move(s));
        if (id == invalid_symbol) {
            diagnostic d;
            d.level = severity::error;
            d.code = diagnostic_code::duplicate_variable;
            d.span = node.span;
            d.message = std::format("variable '{}' is already declared in this scope", node.name);
            m_diag.report(std::move(d));
        }
    }

    // Expressions ------------------------------------------------------------------------------------------------------

    expr_result analyzer::check_expr(node_id id, type_id expected) {

        if (!id.is_valid())
            return {};

        expr_result result = std::visit([&](const auto& n) -> expr_result {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, int_literal_node>)          return check_int_literal(n, expected);
            else if constexpr (std::is_same_v<T, identifier_node>)      return check_identifier(n);
            else if constexpr (std::is_same_v<T, unary_expr_node>)      return check_unary(n, expected);
            else if constexpr (std::is_same_v<T, binary_expr_node>)     return check_binary(n, expected);
            else if constexpr (std::is_same_v<T, bool_literal_node>)    return check_bool_literal(n);
            else if constexpr (std::is_same_v<T, call_expr_node>)       return check_call(n, expected);
            else if constexpr (std::is_same_v<T, default_init_node>)    return check_default_init(n, expected);
            else if constexpr (std::is_same_v<T, bit_expr_node>)        return check_bit(n, expected);
            else                                                        return {};
        }, m_arena.get(id));

        if (result.type != invalid_type)
            m_result.expr_types[id.index] = result.type;
        if (result.identifier_symbol != invalid_symbol)
            m_result.identifier_symbols[id.index] = result.identifier_symbol;

        return result;
    }


    expr_result analyzer::check_int_literal(const int_literal_node& node, type_id expected) {

        const auto parsed = parse_int_literal(node.lexeme);
        if (parsed.overflowed) {
            m_diag.error(diagnostic_code::integer_literal_overflow, node.span,
                "integer literal is too large (exceeds 128 bits)");
            return {};
        }

        // Explicit suffix: the literal is committed to that type
        if (node.suffix_kind != token_kind::invalid) {
            const type_id explicit_type = primitive_type_id(node.suffix_kind);
            if (!check_literal_range(explicit_type, parsed.magnitude, node.span, m_diag))
                return {};                        // <- was returning { explicit_type, false }
            return { explicit_type, false };
        }

        // Unsuffixed with a concrete integer context
        if (expected != invalid_type && is_integer(expected)) {
            if (!check_literal_range(expected, parsed.magnitude, node.span, m_diag))
                return {};
            return { expected, false };
        }

        // Unsuffixed, no context: default to i32 and stay flexible
        const type_id default_type = primitive_type_id(token_kind::prim_i32);
        if (!check_literal_range(default_type, parsed.magnitude, node.span, m_diag))
            return {};
        return { default_type, true };
    }


    expr_result analyzer::check_identifier(const identifier_node& node) {

        const symbol_id sym = m_result.symbols().lookup(node.name);
        if (sym == invalid_symbol) {
            report_unknown_name(node.span, node.name);
            return {};
        }
        return { m_result.symbols().get(sym).type, false, sym };
    }


    expr_result analyzer::check_unary(const unary_expr_node& node, type_id expected) {

        if (!node.operand.is_valid())
            return {};

        // `!` requires bool
        if (node.op == token_kind::bang) {
            const type_id b = primitive_type_id(token_kind::prim_bool);
            const auto operand = check_expr(node.operand, b);
            if (operand.type != invalid_type && !is_bool(operand.type)) {
                m_diag.error(diagnostic_code::invalid_operand_type, m_arena.span_of(node.operand),
                    std::format("operator '!' requires a 'bool' operand, found '{}'", type_name(operand.type)));
            }
            return { b, false };
        }

        // +, -, ~ : numeric operand, same type as operand
        const auto operand = check_expr(node.operand, expected);
        if (operand.type == invalid_type)
            return {};

        if (!is_numeric(operand.type)) {
            m_diag.error(diagnostic_code::invalid_operand_type, m_arena.span_of(node.operand),
                std::format("operator '{}' requires a numeric operand, found '{}'", token_kind_name(node.op), type_name(operand.type)));
            return {};
        }

        if (node.op == token_kind::tilde && !is_integer(operand.type)) {
            m_diag.error(diagnostic_code::invalid_operand_type, m_arena.span_of(node.operand),
                std::format("operator '~' requires an integer operand, found '{}'", type_name(operand.type)));
            return {};
        }

        return { operand.type, operand.flexible };
    }


    expr_result analyzer::check_binary(const binary_expr_node& node, type_id expected) {

        if (!node.lhs.is_valid() || !node.rhs.is_valid())
            return {};

        // Logical operators: bool on both sides
        if (node.op == token_kind::amp_amp || node.op == token_kind::pipe_pipe) {
            const type_id b = primitive_type_id(token_kind::prim_bool);
            const auto lhs = check_expr(node.lhs, b);
            const auto rhs = check_expr(node.rhs, b);
            if (lhs.type != invalid_type && !is_bool(lhs.type)) {
                m_diag.error(diagnostic_code::invalid_operand_type, m_arena.span_of(node.lhs), 
                    std::format("operator '{}' requires 'bool' operands, found '{}'", token_kind_name(node.op), type_name(lhs.type)));
            }
            if (rhs.type != invalid_type && !is_bool(rhs.type)) {
                m_diag.error(diagnostic_code::invalid_operand_type, m_arena.span_of(node.rhs),
                    std::format("operator '{}' requires 'bool' operands, found '{}'", token_kind_name(node.op), type_name(rhs.type)));
            }
            return { b, false };
        }

        auto lhs = check_expr(node.lhs, expected);
        auto rhs = check_expr(node.rhs, expected);

        if (lhs.type == invalid_type || rhs.type == invalid_type)
            return {};

        // Unify operand types. §16.1: no implicit numeric conversion, so the two sides must be identical — except that a *flexible*
        // side (unsuffixed literal) may adopt the other side's concrete type
        type_id unified = invalid_type;

        if (!lhs.flexible && !rhs.flexible) {
            if (lhs.type != rhs.type) {
                report_type_mismatch(m_arena.span_of(node.rhs), lhs.type, rhs.type, std::format("right operand of '{}'", token_kind_name(node.op)));
                return {};
            }
            unified = lhs.type;
        } else if (lhs.flexible && !rhs.flexible) {
            const auto retyped = check_expr(node.lhs, rhs.type);
            if (retyped.type != invalid_type && retyped.type != rhs.type) {
                report_type_mismatch(m_arena.span_of(node.lhs), rhs.type, retyped.type, std::format("left operand of '{}'", token_kind_name(node.op)));
                return {};
            }
            unified = rhs.type;
        } else if (!lhs.flexible && rhs.flexible) {
            const auto retyped = check_expr(node.rhs, lhs.type);
            if (retyped.type != invalid_type && retyped.type != lhs.type) {
                report_type_mismatch(m_arena.span_of(node.rhs), lhs.type, retyped.type, std::format("right operand of '{}'", token_kind_name(node.op)));
                return {};
            }
            unified = lhs.type;
        } else {
            // Both flexible: they share the default type (i32, or whatever expected propagated down as). No retyping needed
            unified = lhs.type;
        }

        const bool is_comparison =
            node.op == token_kind::eq_eq        ||
            node.op == token_kind::bang_eq      ||
            node.op == token_kind::lt           ||
            node.op == token_kind::lt_eq        ||
            node.op == token_kind::gt           ||
            node.op == token_kind::gt_eq;

        const bool is_bitwise =
            node.op == token_kind::amp          ||
            node.op == token_kind::pipe         ||
            node.op == token_kind::caret        ||
            node.op == token_kind::lt_lt        ||
            node.op == token_kind::gt_gt;

        if (is_comparison) {
            // Equality works on bools and numerics. Ordering only on numerics
            const bool is_ordering =
                node.op == token_kind::lt       ||
                node.op == token_kind::lt_eq    ||
                node.op == token_kind::gt       ||
                node.op == token_kind::gt_eq;

            if (is_bool(unified) && is_ordering) {
                m_diag.error(diagnostic_code::invalid_operand_type, node.span, std::format("operator '{}' cannot be applied to 'bool' operands",
                    token_kind_name(node.op)));
                return {};
            }
            if (!is_numeric(unified) && !is_bool(unified)) {
                m_diag.error(diagnostic_code::invalid_operand_type, node.span, std::format("operator '{}' requires numeric operands, found '{}'",
                    token_kind_name(node.op), type_name(unified)));
                return {};
            }
            return { primitive_type_id(token_kind::prim_bool), false };
        }

        if (is_bitwise && !is_integer(unified)) {
            m_diag.error(diagnostic_code::invalid_operand_type, node.span, std::format("operator '{}' requires integer operands, found '{}'",
                token_kind_name(node.op), type_name(unified)));
            return {};
        }

        if (!is_numeric(unified)) {
            m_diag.error(diagnostic_code::invalid_operand_type, node.span, std::format("operator '{}' requires numeric operands, found '{}'",
                token_kind_name(node.op), type_name(unified)));
            return {};
        }

        // Arithmetic: result type is the unified type; flexible only if both operands were flexible (which we collapsed to concrete
        // above — so this is always false after a successful unification through a concrete side, and reflects flexibility otherwise)
        return { unified, lhs.flexible && rhs.flexible };
    }


    expr_result analyzer::check_bool_literal(const bool_literal_node& /*node*/) { return { primitive_type_id(token_kind::prim_bool), false }; }


    expr_result analyzer::check_call(const call_expr_node& node, type_id /*expected*/) {

        if (!node.callee.is_valid())
            return {};

        const auto& callee = m_arena.get_as<identifier_node>(node.callee);
        const symbol_id fn_sym = m_result.symbols().lookup_function(callee.name);

        if (fn_sym == invalid_symbol) {
            diagnostic d;
            d.level = severity::error;
            d.code = diagnostic_code::unknown_identifier;
            d.span = callee.span;
            d.message = std::format("unknown function '{}'", callee.name);
            d.help = "check the function name and ensure it is declared";
            m_diag.report(std::move(d));
            for (const auto a : node.args) (void)check_expr(a, invalid_type);
            return {};
        }

        const auto& sym = m_result.symbols().get(fn_sym);
        const auto& fn_node = m_arena.get_as<function_node>(sym.definition);

        if (node.args.size() != fn_node.params.size()) {
            m_diag.error(diagnostic_code::argument_count_mismatch, node.span,
                std::format("function '{}' expects {} argument{}, but {} {} given",
                    callee.name, fn_node.params.size(),
                    fn_node.params.size() == 1 ? "" : "s",
                    node.args.size(),
                    node.args.size() == 1 ? "was" : "were"));
            for (const auto a : node.args) (void)check_expr(a, invalid_type);
            return { sym.return_type, false };
        }

        for (std::size_t i = 0; i < node.args.size(); ++i) {
            const auto& param = m_arena.get_as<parameter_node>(fn_node.params[i]);
            const type_id p_type = resolve_type(param.type);
            const auto a = check_expr(node.args[i], p_type);
            if (p_type != invalid_type && a.type != invalid_type && a.type != p_type)
                report_type_mismatch(m_arena.span_of(node.args[i]), p_type, a.type,
                    std::format("argument {} of call to '{}'", i + 1, callee.name));
        }

        return { sym.return_type, false };
    }


    expr_result analyzer::check_default_init(const default_init_node& node, type_id expected) {

        if (expected == invalid_type) {
            diagnostic d;
            d.level = severity::error;
            d.code = diagnostic_code::default_requires_context;
            d.span = node.span;
            d.message = "`default` requires a known type from context";
            d.help = "write the variable's type explicitly, e.g. `u32 x = default;`";
            m_diag.report(std::move(d));
            return {};
        }
        return { expected, false };
    }


    expr_result analyzer::check_bit(const bit_expr_node& node, type_id expected) {

        if (!node.operand.is_valid())
            return {};

        type_id result_type = invalid_type;
        if (expected != invalid_type && is_integer(expected))
            result_type = expected;
        else
            result_type = primitive_type_id(token_kind::prim_i32);

        const auto operand = check_expr(node.operand, result_type);
        if (operand.type == invalid_type)
            return { result_type, false };

        if (!is_integer(operand.type)) {
            m_diag.error(diagnostic_code::invalid_operand_type, m_arena.span_of(node.operand),
                std::format("'bit()' index must be an integer, found '{}'", type_name(operand.type)));
            return { result_type, false };
        }

        if (m_arena.holds<int_literal_node>(node.operand)) {
            const auto& lit = m_arena.get_as<int_literal_node>(node.operand);
            const auto parsed = parse_int_literal(lit.lexeme);
            if (!parsed.overflowed) {
                const auto info = type_info_of(result_type);
                const std::uint64_t width = static_cast<std::uint64_t>(info.size_bytes) * 8;
                const auto mag = static_cast<std::uint64_t>(parsed.magnitude);
                if (mag >= width) {
                    m_diag.error(diagnostic_code::bit_index_out_of_range, lit.span,
                        std::format("bit index {} is out of range for '{}' (valid: 0..{})", mag, info.name, width - 1));
                }
            }
        }

        return { result_type, false };
    }

    // -----------------------------------------------------------------------------------------------------------------

    void analyzer::check_assign(const assign_stmt_node& node) {

        if (!node.target.is_valid() || !node.value.is_valid())
            return;

        const auto& ident = m_arena.get_as<identifier_node>(node.target);
        const symbol_id sym_id = m_result.symbols().lookup(ident.name);

        if (sym_id == invalid_symbol) {
            report_unknown_name(ident.span, ident.name);
            (void)check_expr(node.value, invalid_type);   // surface inner errors too
            return;
        }

        const auto& sym = m_result.symbols().get(sym_id);

        if (!sym.is_mut) {
            diagnostic d;
            d.level = severity::error;
            d.code = diagnostic_code::cannot_assign_immutable;
            d.span = ident.span;
            d.message = std::format("cannot assign to immutable variable '{}'", ident.name);
            d.help = std::format("declare it with `mut {} {} = ...;`", type_name(sym.type), ident.name);
            m_diag.report(std::move(d));
            (void)check_expr(node.value, sym.type);
            return;
        }

        if (sym.type != invalid_type) {
            const auto v = check_expr(node.value, sym.type);
            if (v.type != invalid_type && v.type != sym.type)
                report_type_mismatch(m_arena.span_of(node.value), sym.type, v.type, "assignment");
        } else {
            (void)check_expr(node.value, invalid_type);
        }

        m_result.identifier_symbols[node.target.index] = sym_id;
    }


    void analyzer::check_if_stmt(const if_stmt_node& node, type_id expected_return_type) {

        const type_id b = primitive_type_id(token_kind::prim_bool);

        if (node.condition.is_valid()) {
            const auto cond = check_expr(node.condition, b);
            if (cond.type != invalid_type && !is_bool(cond.type)) {
                diagnostic d;
                d.level = severity::error;
                d.code = diagnostic_code::invalid_condition_type;
                d.span = m_arena.span_of(node.condition);
                d.message = std::format("'if' condition must be 'bool', found '{}'", type_name(cond.type));
                m_diag.report(std::move(d));
            }
        }

        if (node.then_block.is_valid() && m_arena.holds<block_node>(node.then_block)) {
            const auto& tb = m_arena.get_as<block_node>(node.then_block);
            m_result.symbols().push_scope();
            for (const auto s : tb.statements)
                check_statement(s, expected_return_type);
            m_result.symbols().pop_scope();
        }

        if (node.else_block.is_valid()) {
            if (m_arena.holds<block_node>(node.else_block)) {
                const auto& eb = m_arena.get_as<block_node>(node.else_block);
                m_result.symbols().push_scope();
                for (const auto s : eb.statements)
                    check_statement(s, expected_return_type);
                m_result.symbols().pop_scope();
            } else if (m_arena.holds<if_stmt_node>(node.else_block)) {
                check_if_stmt(m_arena.get_as<if_stmt_node>(node.else_block), expected_return_type);
            }
        }
    }


    void analyzer::check_while_stmt(const while_stmt_node& node, type_id expected_return_type) {

        const type_id b = primitive_type_id(token_kind::prim_bool);

        if (node.condition.is_valid()) {
            const auto cond = check_expr(node.condition, b);
            if (cond.type != invalid_type && !is_bool(cond.type)) {
                diagnostic d;
                d.level = severity::error;
                d.code = diagnostic_code::invalid_condition_type;
                d.span = m_arena.span_of(node.condition);
                d.message = std::format("'while' condition must be 'bool', found '{}'", type_name(cond.type));
                m_diag.report(std::move(d));
            }
        }

        if (node.body.is_valid() && m_arena.holds<block_node>(node.body)) {
            const auto& body = m_arena.get_as<block_node>(node.body);
            m_result.symbols().push_scope();
            for (const auto s : body.statements)
                check_statement(s, expected_return_type);
            m_result.symbols().pop_scope();
        }
    }

    // Helpers ----------------------------------------------------------------------------------------------------------

    type_id analyzer::resolve_type(node_id type_node) {

        if (!type_node.is_valid())
            return invalid_type;

        return std::visit([&](const auto& n) -> type_id {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, primitive_type_node>) {
                return primitive_type_id(n.type_kind);
            } else if constexpr (std::is_same_v<T, named_type_node>) {
                // M3 has no user-defined types yet
                m_diag.error(diagnostic_code::unknown_type, n.span, std::format("unknown type '{}'", n.name));
                return invalid_type;
            } else {
                return invalid_type;
            }
        }, m_arena.get(type_node));
    }


    void analyzer::report_type_mismatch(source_span span, type_id expected, type_id found, std::string_view context) {

        diagnostic d;
        d.level = severity::error;
        d.code = diagnostic_code::type_mismatch;
        d.span = span;
        d.message = std::format("type mismatch in {}: expected '{}', found '{}'", context, type_name(expected), type_name(found));
        m_diag.report(std::move(d));
    }


    void analyzer::report_unknown_name(source_span span, std::string_view name) {

        diagnostic d;
        d.level = severity::error;
        d.code = diagnostic_code::unknown_identifier;
        d.span = span;
        d.message = std::format("unknown identifier '{}'", name);
        d.help = "declare the variable before using it";
        m_diag.report(std::move(d));
    }

    // CLASS IMPLEMENTATION ============================================================================================

    // CLASS PROTECTED =================================================================================================

    // CLASS PRIVATE ===================================================================================================

    const symbol_table& sema_result::symbols() const noexcept { return m_symbols; }


    symbol_table& sema_result::symbols() noexcept { return m_symbols; }


    type_id sema_result::expr_type(node_id id) const {

        auto it = expr_types.find(id.index);
        return it == expr_types.end() ? invalid_type : it->second;
    }


    symbol_id sema_result::identifier_symbol(node_id id) const {

        auto it = identifier_symbols.find(id.index);
        return it == identifier_symbols.end() ? invalid_symbol : it->second;
    }

    // TEMPLATE IMPLEMENTATION =========================================================================================

}

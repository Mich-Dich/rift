
#pragma once

#include "rift/span.h"
#include "rift/token.h"

#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>
#include <variant>
#include <vector>



// FORWARD DECLARATIONS ================================================================================================

namespace rift {

    // CONSTANTS =======================================================================================================

    inline constexpr std::uint32_t invalid_node_index = 0xFFFFFFFFu;

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    enum class node_kind : std::uint8_t {
        program,
        function,
        parameter,
        block,
        return_stmt,
        var_decl,
        int_literal,
        identifier,
        unary_expr,
        binary_expr,
        primitive_type,
        named_type,
    };


    // Compact handle into an ast_arena. Trivially copyable; invalid means "no node".
    struct node_id {

        std::uint32_t                       index = invalid_node_index;

        constexpr bool is_valid() const noexcept { return index != invalid_node_index; }

        constexpr bool operator==(const node_id&) const noexcept = default;
    };

    // Every node carries its source span. Concrete node types add their own fields.
    struct node_base {

        source_span                         span;
    };


    struct program_node : node_base {

        static constexpr node_kind          kind = node_kind::program;

        std::vector<node_id>                declarations;
    };


    struct function_node : node_base {

        static constexpr node_kind          kind = node_kind::function;

        node_id                             return_type;
        std::string_view                    name;
        std::vector<node_id>                params;
        node_id                             body;
    };


    struct parameter_node : node_base {

        static constexpr node_kind          kind = node_kind::parameter;

        node_id                             type;
        std::string_view                    name;
        bool                                is_mut = false;
    };


    struct block_node : node_base {

        static constexpr node_kind          kind = node_kind::block;

        std::vector<node_id>                statements;
    };


    struct return_stmt_node : node_base {

        static constexpr node_kind          kind = node_kind::return_stmt;

        node_id                             value;  // may be invalid (bare `return;`)
    };


    struct var_decl_node : node_base {

        static constexpr node_kind          kind = node_kind::var_decl;

        node_id                             type;
        std::string_view                    name;
        bool                                is_mut = false;
        node_id                             initializer;
    };


    struct int_literal_node : node_base {

        static constexpr node_kind          kind = node_kind::int_literal;

        // Raw lexeme including suffix: "0u32", "42", "0xFFu8".
        std::string_view                    lexeme;

        // Suffix type — prim_u32, prim_i64, … — or invalid when the literal is
        // unsuffixed. Sema (M3) supplies the contextual type for unsuffixed ones.
        token_kind                          suffix_kind = token_kind::invalid;
    };


    struct identifier_node : node_base {

        static constexpr node_kind          kind = node_kind::identifier;

        std::string_view                    name;
    };


    struct unary_expr_node : node_base {

        static constexpr node_kind          kind = node_kind::unary_expr;

        token_kind                          op;
        node_id                             operand;
    };


    struct binary_expr_node : node_base {

        static constexpr node_kind          kind = node_kind::binary_expr;

        token_kind                          op;
        node_id                             lhs;
        node_id                             rhs;
    };


    struct primitive_type_node : node_base {

        static constexpr node_kind          kind = node_kind::primitive_type;

        token_kind                          type_kind;  // prim_u32, prim_i32, ...
    };


    struct named_type_node : node_base {

        static constexpr node_kind          kind = node_kind::named_type;

        std::string_view                    name;
    };


    using ast_variant = std::variant<
        program_node,
        function_node,
        parameter_node,
        block_node,
        return_stmt_node,
        var_decl_node,
        int_literal_node,
        identifier_node,
        unary_expr_node,
        binary_expr_node,
        primitive_type_node,
        named_type_node
    >;

    // STATIC VARIABLES ================================================================================================

    // FUNCTION DECLARATION ============================================================================================

    std::string_view node_kind_name(node_kind k) noexcept;

    // TEMPLATE DECLARATION ============================================================================================

    // CLASS DECLARATION ===============================================================================================

    class ast_arena {
    public:

        node_id add(ast_variant node);

        const ast_variant& get(node_id id) const;

        std::size_t size() const noexcept { return m_nodes.size(); }
        bool empty() const noexcept { return m_nodes.empty(); }

        template <typename T>
        const T& get_as(node_id id) const { return std::get<T>(m_nodes[id.index]); }

        template <typename T>
        bool holds(node_id id) const { return std::holds_alternative<T>(m_nodes[id.index]); }

        // Convenience accessors that work for any node kind.
        node_kind kind_of(node_id id) const;
        source_span span_of(node_id id) const;

    private:

        std::vector<ast_variant>            m_nodes;
    };

}


#include "rift/ast.h"

#include <ostream>
#include <stdexcept>



// FORWARD DECLARATIONS ================================================================================================

namespace rift {

    // CONSTANTS =======================================================================================================

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    // STATIC VARIABLES ================================================================================================

    // INTERNAL TEMPLATE DECLARATION ===================================================================================

    // INTERNAL FUNCTION DECLARATION ===================================================================================

    // INTERNAL TEMPLATE IMPLEMENTATION ================================================================================

    // INTERNAL FUNCTION IMPLEMENTATION ================================================================================

    // TEMPLATE IMPLEMENTATION =========================================================================================

    // FUNCTION IMPLEMENTATION =========================================================================================

    std::string_view node_kind_name(node_kind k) noexcept {

        switch (k) {
            case node_kind::program:        return "program";
            case node_kind::function:       return "function";
            case node_kind::parameter:      return "parameter";
            case node_kind::block:          return "block";
            case node_kind::return_stmt:    return "return_stmt";
            case node_kind::var_decl:       return "var_decl";
            case node_kind::int_literal:    return "int_literal";
            case node_kind::identifier:     return "identifier";
            case node_kind::unary_expr:     return "unary_expr";
            case node_kind::binary_expr:    return "binary_expr";
            case node_kind::primitive_type: return "primitive_type";
            case node_kind::named_type:     return "named_type";
        }
        return "unknown";
    }

    // CLASS IMPLEMENTATION ============================================================================================

    // CLASS PUBLIC ====================================================================================================

    node_id ast_arena::add(ast_variant node) {

        node_id id;
        id.index = static_cast<std::uint32_t>(m_nodes.size());
        m_nodes.push_back(std::move(node));
        return id;
    }


    const ast_variant& ast_arena::get(node_id id) const {

        if (!id.is_valid() || id.index >= m_nodes.size())
            throw std::out_of_range("ast_arena::get: invalid node_id");
        return m_nodes[id.index];
    }


    node_kind ast_arena::kind_of(node_id id) const {

        if (!id.is_valid())
            return node_kind::program;  // sentinel; callers should check id.is_valid()
        return std::visit([](const auto& n) { return std::decay_t<decltype(n)>::kind; }, m_nodes[id.index]);
    }


    source_span ast_arena::span_of(node_id id) const {

        if (!id.is_valid())
            return source_span{};
        return std::visit([](const auto& n) -> source_span { return n.span; }, m_nodes[id.index]);
    }

    // CLASS PROTECTED =================================================================================================

    // CLASS PRIVATE ===================================================================================================

}

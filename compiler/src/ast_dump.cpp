
#include "rift/ast_dump.h"

#include <ostream>
#include <sstream>
#include <string>



// FORWARD DECLARATIONS ================================================================================================

namespace rift {

    // CONSTANTS =======================================================================================================

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    // STATIC VARIABLES ================================================================================================

    // INTERNAL TEMPLATE DECLARATION ===================================================================================

    // INTERNAL FUNCTION DECLARATION ===================================================================================

    std::string pad(int indent);


    std::string inline_dump(const ast_arena& a, node_id id);


    void dump_node(const ast_arena& a, node_id id, std::ostream& os, int indent);

    // INTERNAL TEMPLATE IMPLEMENTATION ================================================================================

    // INTERNAL FUNCTION IMPLEMENTATION ================================================================================

    std::string pad(int indent) { return std::string(static_cast<std::size_t>(indent) * 2, ' '); }


    // Leaf-only inline printer for use inside attributes of another node. Non-leaf nodes fall back to a placeholder;
    // containers should never be emitted inline by the tree-walking callers below
    std::string inline_dump(const ast_arena& a, node_id id) {

        if (!id.is_valid()) return "<invalid>";

        return std::visit([](const auto& n) -> std::string {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, primitive_type_node>)
                return std::string(token_kind_name(n.type_kind));

            else if constexpr (std::is_same_v<T, named_type_node>)
                return std::string(n.name);

            else if constexpr (std::is_same_v<T, identifier_node>)
                return std::string(n.name);

            else if constexpr (std::is_same_v<T, int_literal_node>)
                return std::string("(int_literal ") + std::string(n.lexeme) + " " + std::string(token_kind_name(n.suffix_kind)) + ")";

            else
                return "<non-leaf>";
        }, a.get(id));
    }


    void dump_node(const ast_arena& a, node_id id, std::ostream& os, int indent) {

        if (!id.is_valid()) {
            os << pad(indent) << "<invalid>\n";
            return;
        }

        const std::string p = pad(indent);

        std::visit([&](const auto& n) {
            using T = std::decay_t<decltype(n)>;

            if constexpr (std::is_same_v<T, program_node>) {

                if (n.declarations.empty()) {
                    os << p << "(program)\n";
                } else {
                    os << p << "(program\n";
                    for (const auto d : n.declarations) dump_node(a, d, os, indent + 1);
                    os << p << ")\n";
                }

            } else if constexpr (std::is_same_v<T, function_node>) {

                os << p << "(function " << n.name << " -> "
                   << inline_dump(a, n.return_type) << "\n";

                if (n.params.empty()) {
                    os << p << "  (params)\n";
                } else {
                    os << p << "  (params\n";
                    for (const auto prm : n.params) dump_node(a, prm, os, indent + 2);
                    os << p << "  )\n";
                }

                dump_node(a, n.body, os, indent + 1);
                os << p << ")\n";

            } else if constexpr (std::is_same_v<T, parameter_node>) {

                os << p << "(param " << (n.is_mut ? "mut " : "")
                   << n.name << " : " << inline_dump(a, n.type) << ")\n";

            } else if constexpr (std::is_same_v<T, block_node>) {

                if (n.statements.empty()) {
                    os << p << "(block)\n";
                } else {
                    os << p << "(block\n";
                    for (const auto s : n.statements) dump_node(a, s, os, indent + 1);
                    os << p << ")\n";
                }

            } else if constexpr (std::is_same_v<T, return_stmt_node>) {

                if (n.value.is_valid()) {
                    os << p << "(return\n";
                    dump_node(a, n.value, os, indent + 1);
                    os << p << ")\n";
                } else {
                    os << p << "(return)\n";
                }

            } else if constexpr (std::is_same_v<T, var_decl_node>) {

                os << p << "(var_decl " << (n.is_mut ? "mut " : "")
                   << n.name << " : " << inline_dump(a, n.type) << " =\n";
                dump_node(a, n.initializer, os, indent + 1);
                os << p << ")\n";

            } else if constexpr (std::is_same_v<T, int_literal_node>) {

                os << p << "(int_literal " << n.lexeme << " "
                   << token_kind_name(n.suffix_kind) << ")\n";

            } else if constexpr (std::is_same_v<T, identifier_node>) {

                os << p << "(identifier " << n.name << ")\n";

            } else if constexpr (std::is_same_v<T, unary_expr_node>) {

                os << p << "(unary " << token_kind_name(n.op) << "\n";
                dump_node(a, n.operand, os, indent + 1);
                os << p << ")\n";

            } else if constexpr (std::is_same_v<T, binary_expr_node>) {

                os << p << "(binary " << token_kind_name(n.op) << "\n";
                dump_node(a, n.lhs, os, indent + 1);
                dump_node(a, n.rhs, os, indent + 1);
                os << p << ")\n";

            } else if constexpr (std::is_same_v<T, primitive_type_node>)
                os << p << "(type " << token_kind_name(n.type_kind) << ")\n";

            else if constexpr (std::is_same_v<T, named_type_node>)
                os << p << "(type " << n.name << ")\n";

        }, a.get(id));
    }

    // TEMPLATE IMPLEMENTATION =========================================================================================

    // FUNCTION IMPLEMENTATION =========================================================================================

    void dump_ast(const ast_arena& arena, node_id root, std::ostream& os) { dump_node(arena, root, os, 0); }


    std::string dump_ast_to_string(const ast_arena& arena, node_id root) {

        std::ostringstream os;
        dump_ast(arena, root, os);
        return os.str();
    }

    // CLASS IMPLEMENTATION ============================================================================================

    // CLASS PUBLIC ====================================================================================================

    // CLASS PROTECTED =================================================================================================

    // CLASS PRIVATE ===================================================================================================

}

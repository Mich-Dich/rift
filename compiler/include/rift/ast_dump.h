#pragma once

#include "rift/ast.h"

#include <iosfwd>
#include <string>



// FORWARD DECLARATIONS ================================================================================================

namespace rift {

    // CONSTANTS =======================================================================================================

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    // STATIC VARIABLES ================================================================================================

    // FUNCTION DECLARATION ============================================================================================

    // Writes a stable S-expression-style dump of the AST rooted at `root`
    //
    // Format:
    //   - A leaf node prints as `(kind attrs...)` on one line
    //   - A container prints as `(kind attrs...`, its children each on their
    //     own indented line, then `)` on a line at the container's indent
    //
    // The output always ends with a single newline
    void dump_ast(const ast_arena& arena, node_id root, std::ostream& os);

    std::string dump_ast_to_string(const ast_arena& arena, node_id root);

    // TEMPLATE DECLARATION ============================================================================================

    // CLASS DECLARATION ===============================================================================================

}

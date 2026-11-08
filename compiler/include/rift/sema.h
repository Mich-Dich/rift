
#pragma once

#include "rift/ast.h"
#include "rift/diagnostic_engine.h"
#include "rift/source.h"
#include "rift/symbol.h"
#include "rift/types.h"

#include <cstdint>
#include <unordered_map>



// FORWARD DECLARATIONS ================================================================================================

namespace rift {
    class sema_result;
}

namespace rift {

    // CONSTANTS =======================================================================================================

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    // STATIC VARIABLES ================================================================================================

    // FUNCTION DECLARATION ============================================================================================

    // Run semantic analysis on `program`. Diagnostics accumulate in `diag`. Always returns a valid `sema_result`, even on
    // errors — subsequent passes can inspect what was successfully checked
    sema_result analyze(const ast_arena& arena, node_id program, source_manager& sm, file_id file, diagnostic_engine& diag);

    // TEMPLATE DECLARATION ============================================================================================

    // CLASS DECLARATION ===============================================================================================

    // Result of semantic analysis. Holds side tables keyed on node_id::index. The AST itself is not mutated; everything
    // sema learns is stored here
    class sema_result {
    public:

        node_id                                             program;
        std::unordered_map<std::uint32_t, type_id>          expr_types;
        std::unordered_map<std::uint32_t, symbol_id>        identifier_symbols;

        const symbol_table& symbols() const noexcept;

        symbol_table& symbols() noexcept;

        type_id expr_type(node_id id) const;

        symbol_id identifier_symbol(node_id id) const;

    private:

        symbol_table                                  m_symbols;
    };

}





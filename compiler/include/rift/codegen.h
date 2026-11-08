
#pragma once

#include "rift/ast.h"
#include "rift/diagnostic_engine.h"
#include "rift/sema.h"

#include <iosfwd>
#include <string>



// FORWARD DECLARATIONS ================================================================================================

namespace rift {

    // CONSTANTS =======================================================================================================

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    // STATIC VARIABLES ================================================================================================

    // FUNCTION DECLARATION ============================================================================================

    // Emits LLVM IR text for `program` into `os`. `sema` must be the result of `analyze(arena, program, ...)` on the same arena;
    // codegen relies on sema.expr_type() to determine every expression's type (M3 records one for every checked expression node)
    //
    // Diagnostics emitted here are internal-only. If sema succeeded, codegen should not fail — anything it reports points
    // to a compiler bug, not a user error. Assumes LLVM 15+ (opaque pointers)
    void emit_llvm_ir(const ast_arena& arena, node_id program, const sema_result& sema, diagnostic_engine& diag, std::ostream& os);


    std::string emit_llvm_ir_to_string(const ast_arena& arena, node_id program, const sema_result& sema, diagnostic_engine& diag);

    // TEMPLATE DECLARATION ============================================================================================

    // CLASS DECLARATION ===============================================================================================

}


#pragma once

#include <span>
#include <string_view>



// FORWARD DECLARATIONS ================================================================================================

namespace rift {

    // CONSTANTS =======================================================================================================

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    // Exit codes for the CLI. `ok` is success; `usage_error` is a user-facing problem (bad flags,
    // missing file); `not_implemented` is a milestone placeholder (M0 only)
    enum class exit_code : int {
        ok = 0,
        usage_error = 1,
        not_implemented = 2,
        internal_error = 3,
    };

    // STATIC VARIABLES ================================================================================================

    // FUNCTION DECLARATION ============================================================================================

    // Runs the CLI. `args` does NOT include argv[0]
    int run_cli(std::span<const std::string_view> args);

    // TEMPLATE DECLARATION ============================================================================================

    // CLASS DECLARATION ===============================================================================================

}

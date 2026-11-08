
#pragma once

#include <string>
#include <string_view>
#include <cstdint>



// FORWARD DECLARATIONS ================================================================================================

namespace rift {

    // CONSTANTS =======================================================================================================

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    // The compiler's version. `phase` names the active milestone so `riftc --version` immediately tells a user which
    // slice of the roadmap they're looking at
    struct version_info {
        std::int32_t                            major{};
        std::int32_t                            minor{};
        std::int32_t                            patch{};
        std::string_view                        phase{};
    };

    // STATIC VARIABLES ================================================================================================

    constexpr version_info current_version() noexcept { return version_info{ 0, 6, 0, "M6" }; }


    std::string format_version();

    // FUNCTION DECLARATION ============================================================================================

    // TEMPLATE DECLARATION ============================================================================================

    // CLASS DECLARATION ===============================================================================================

}

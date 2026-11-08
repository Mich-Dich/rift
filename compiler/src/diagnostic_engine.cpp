
#include "rift/diagnostic_engine.h"

#include <ostream>
#include <utility>



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

    // CLASS IMPLEMENTATION ============================================================================================

    // CLASS PUBLIC ====================================================================================================

    void diagnostic_engine::report(diagnostic d) {

        if (d.level == severity::error)
            m_error_count++;
        m_diagnostics.push_back(std::move(d));
    }


    void diagnostic_engine::report(severity level, diagnostic_code code, source_span span, std::string message) {

        diagnostic d;
        d.level = level;
        d.code = code;
        d.span = span;
        d.message = std::move(message);
        report(std::move(d));
    }


    void diagnostic_engine::print_all(std::ostream& os, const source_manager& sm) const {

        bool first = true;
        for (const auto& d : m_diagnostics) {
            if (!first) os << '\n';
            first = false;
            print_diagnostic(os, d, sm);
        }
    }

    // CLASS PROTECTED =================================================================================================

    // CLASS PRIVATE ===================================================================================================

}

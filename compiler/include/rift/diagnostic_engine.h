
#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

#include "rift/diagnostic.h"
#include "rift/source.h"
#include "rift/span.h"



// FORWARD DECLARATIONS ================================================================================================

namespace rift {

    // CONSTANTS =======================================================================================================

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    // STATIC VARIABLES ================================================================================================

    // FUNCTION DECLARATION ============================================================================================

    // TEMPLATE DECLARATION ============================================================================================

    // CLASS DECLARATION ===============================================================================================

    class diagnostic_engine {
    public:
        diagnostic_engine() = default;

        void report(diagnostic d);

        void report(severity level, diagnostic_code code, source_span span, std::string message);

        void error(diagnostic_code code, source_span span, std::string message) {

            report(severity::error, code, span, std::move(message));
        }

        void warning(diagnostic_code code, source_span span, std::string message) {

            report(severity::warning, code, span, std::move(message));
        }

        bool has_errors() const noexcept { return m_error_count > 0; }

        std::size_t error_count() const noexcept { return m_error_count; }

        std::size_t diagnostic_count() const noexcept { return m_diagnostics.size(); }

        const std::vector<diagnostic>& diagnostics() const noexcept { return m_diagnostics; }

        void print_all(std::ostream& os, const source_manager& sm, const diagnostic_style& style = {}) const;

    private:

        std::vector<diagnostic>                 m_diagnostics;
        std::size_t                             m_error_count = 0;
    };

}

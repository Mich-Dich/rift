
#include <algorithm>
#include <format>
#include <ostream>
#include <string>

#include "rift/diagnostic.h"


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

    std::string_view severity_name(severity s) {
        switch (s) {
            case severity::error:           return "error";
            case severity::warning:         return "warning";
            case severity::note:            return "note";
            case severity::hint:            return "hint";
        }
        return "error";
    }


    void print_diagnostic(std::ostream& os, const diagnostic& d, const source_manager& sm) {

        if (d.span.file == invalid_file) {
            // No span — emit a single line.
            os << severity_name(d.level) << ": " << d.message << '\n';
            for (const auto& note : d.notes)
                os << "  note: " << note << '\n';

            return;
        }

        const auto& file = sm.get(d.span.file);
        const auto start = file.resolve(d.span.begin);
        const auto end = file.resolve(d.span.end == d.span.begin ? d.span.end : d.span.end - 1);
        const auto line_text = file.line_of(d.span.begin);

        const std::string line_str = std::to_string(start.line);
        const std::size_t width = line_str.size();
        const std::string pad = std::string(width + 1, ' ');

        // Header
        os << severity_name(d.level)
        << std::format("[E{:04d}]: ", static_cast<unsigned>(d.code))
        << d.message << '\n';

        // Location
        os << pad << "--> " << file.path() << ':'
        << start.line << ':' << start.column << '\n';

        // Gutter
        os << pad << " |\n";

        // Source line
        os << line_str << " | " << line_text << '\n';

        // Caret line
        const std::uint32_t line_begin_offset = file.line_start(d.span.begin);
        const std::uint32_t line_end_offset = line_begin_offset + static_cast<std::uint32_t>(line_text.size());

        std::uint32_t caret_start = d.span.begin;
        std::uint32_t caret_end = std::min<std::uint32_t>(d.span.end, line_end_offset);
        if (caret_end <= caret_start)
            caret_end = caret_start + 1;
        // Clamp to the source content just in case.
        if (caret_start > line_end_offset)
            caret_start = line_end_offset;
        if (caret_end > line_end_offset)
            caret_end = line_end_offset;

        const std::size_t caret_col = caret_start - line_begin_offset;
        const std::size_t caret_len = caret_end - caret_start;

        os << pad << " | "
        << std::string(caret_col, ' ')
        << std::string(caret_len, '^') << '\n';

        for (const auto& note : d.notes)
            os << pad << " = note: " << note << '\n';

        (void)end;
    }

    // CLASS IMPLEMENTATION ============================================================================================

    // CLASS PUBLIC ====================================================================================================

    // CLASS PROTECTED =================================================================================================

    // CLASS PRIVATE ===================================================================================================

}

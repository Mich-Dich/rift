#include <algorithm>
#include <format>
#include <ostream>
#include <string>

#include "rift/diagnostic.h"


// FORWARD DECLARATIONS ================================================================================================

namespace rift {

    // CONSTANTS =======================================================================================================

    // ANSI escapes. Kept out of any "no color" code path; callers only emit them via the `c()` helper below when style.use_color is true
    namespace ansi {
        constexpr std::string_view              reset = "\x1b[0m";
        constexpr std::string_view              bold_red = "\x1b[1;91m";
        constexpr std::string_view              bold_yellow = "\x1b[1;93m";
        constexpr std::string_view              bold_cyan = "\x1b[1;96m";
        constexpr std::string_view              bold_blue = "\x1b[1;94m";
        constexpr std::string_view              blue = "\x1b[34m";
    }

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    // STATIC VARIABLES ================================================================================================

    // INTERNAL TEMPLATE DECLARATION ===================================================================================

    // INTERNAL FUNCTION DECLARATION ===================================================================================

    std::string_view severity_color(severity s);

    // INTERNAL TEMPLATE IMPLEMENTATION ================================================================================

    // INTERNAL FUNCTION IMPLEMENTATION ================================================================================

    std::string_view severity_color(severity s) {
        switch (s) {
            case severity::error:   return ansi::bold_red;
            case severity::warning: return ansi::bold_yellow;
            case severity::note:    return ansi::bold_cyan;
            case severity::hint:    return ansi::bold_cyan;
        }
        return ansi::bold_red;
    }

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


    void print_diagnostic(std::ostream& os, const diagnostic& d, const source_manager& sm, const diagnostic_style& style) {

        // When style.use_color is on, wraps `text` in the given ANSI codes
        auto c = [&](std::string_view color, std::string_view text) -> std::string {
            if (!style.use_color)
                return std::string(text);
            std::string out;
            out.reserve(color.size() + text.size() + ansi::reset.size());
            out += color;
            out += text;
            out += ansi::reset;
            return out;
        };

        const auto sev_color = severity_color(d.level);

        // No span — a single line is enough
        if (d.span.file == invalid_file) {
            os << c(sev_color, severity_name(d.level)) << ": " << d.message << '\n';
            for (const auto& note : d.notes)
                os << "  " << c(ansi::bold_cyan, "note:") << ' ' << note << '\n';
            return;
        }

        const auto& file = sm.get(d.span.file);
        const auto  start = file.resolve(d.span.begin);
        const auto  line_text = file.line_of(d.span.begin);

        // Column width of the line number determines every subsequent prefix. The source line prints as `N | text`,
        // so `pad` must be exactly `width` spaces — one less than the old code — so that every `|` lands in the same column
        const std::string line_str = std::to_string(start.line);
        const std::size_t width = line_str.size();
        const std::string pad = std::string(width, ' ');

        // Header: "error[E0100]: message". std::format gives us the zero-padding that a bare std::to_string(std::uint16_t) would drop
        const std::string header =
            std::format("{}[E{:04d}]:", severity_name(d.level), static_cast<unsigned>(d.code));
        os << c(sev_color, header) << ' ' << d.message << '\n';

        // Location: " --> path:line:col"
        os << pad << c(ansi::blue, "-->") << ' '
           << file.path() << ':' << start.line << ':' << start.column << '\n';

        // Gutter: " |"
        os << pad << ' ' << c(ansi::blue, "|") << '\n';

        // Source line: "N | source"
        os << c(ansi::blue, line_str) << ' ' << c(ansi::blue, "|") << ' ' << line_text << '\n';

        // Caret line. Caret must be aligned with the source text; the offset is (pad + " | " + caret_col)
        // Both gutter and caret are `width + 2` characters before the caret can start, matching `N | `'s prefix
        const std::uint32_t line_begin_offset = file.line_start(d.span.begin);
        const std::uint32_t line_end_offset   = line_begin_offset + static_cast<std::uint32_t>(line_text.size());

        std::uint32_t caret_start = d.span.begin;
        std::uint32_t caret_end   = std::min<std::uint32_t>(d.span.end, line_end_offset);

        if (caret_end <= caret_start)
            caret_end = caret_start + 1;
        if (caret_start > line_end_offset)
            caret_start = line_end_offset;
        if (caret_end > line_end_offset)
            caret_end = line_end_offset;

        const std::size_t caret_col = caret_start - line_begin_offset;
        std::size_t       caret_len = caret_end - caret_start;
        if (caret_len == 0)
            caret_len = 1;   // zero-width spans still draw one caret

        os << pad << ' ' << c(ansi::blue, "|") << ' '
           << std::string(caret_col, ' ')
           << c(sev_color, std::string(caret_len, '^')) << '\n';

        // "= help: ..." — one actionable hint per diagnostic, rendered like a note
        if (!d.help.empty())
            os << pad << ' ' << c(ansi::blue, "=") << ' '
               << c(ansi::bold_cyan, "help:") << ' ' << d.help << '\n';

        for (const auto& note : d.notes)
            os << pad << ' ' << c(ansi::blue, "=") << ' '
               << c(ansi::bold_cyan, "note:") << ' ' << note << '\n';
    }

    // CLASS IMPLEMENTATION ============================================================================================

    // CLASS PUBLIC ====================================================================================================

    // CLASS PROTECTED =================================================================================================

    // CLASS PRIVATE ===================================================================================================

}

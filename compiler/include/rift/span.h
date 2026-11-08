
#pragma once

#include <cstdint>
#include <limits>



namespace rift {

    // CONSTANTS =======================================================================================================

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    // A `file_id` is a compact handle into a `source_manager`. Spans carry one so diagnostics can be reported against the right
    // file without hanging on to filenames everywhere
    using file_id = std::uint32_t;

    constexpr file_id invalid_file = std::numeric_limits<file_id>::max();

    // Half-open byte range [begin, end) into a source file. Line/column are NOT stored here — resolving an offset to line/column
    // is cheap but only needed for pretty-printing, and we want spans to stay 12 bytes
    struct source_span {

        file_id file = invalid_file;
        std::uint32_t begin = 0;
        std::uint32_t end = 0;

        constexpr bool empty() const noexcept { return begin >= end; }
        constexpr std::uint32_t length() const noexcept { return end >= begin ? end - begin : 0; }
    };

    // STATIC VARIABLES ================================================================================================

    // FUNCTION DECLARATION ============================================================================================

    constexpr source_span join(source_span a, source_span b) noexcept {

        if (a.file != b.file)
            return a;
        return source_span{
            a.file,
            a.begin < b.begin ? a.begin : b.begin,
            a.end   > b.end   ? a.end   : b.end,
        };
    }

    // TEMPLATE DECLARATION ============================================================================================

    // CLASS DECLARATION ===============================================================================================

}


#pragma once

#include "rift/span.h"

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>



// FORWARD DECLARATIONS ================================================================================================

namespace rift {

    // CONSTANTS =======================================================================================================

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    // STATIC VARIABLES ================================================================================================

    // FUNCTION DECLARATION ============================================================================================

    // TEMPLATE DECLARATION ============================================================================================

    // CLASS DECLARATION ===============================================================================================

    // One loaded source file. Newlines are normalized to '\n' at load time so byte offsets and column arithmetic stay simple;
    // CRLF/CR both work
    //
    // `content()` returns a stable view for the lifetime of this object. Any token's `lexeme` string_view points into it,
    // so this object must outlive those tokens
    class source_file {
    public:
        source_file(file_id id, std::string path, std::string content);

        file_id id() const noexcept { return m_id; }
        std::string_view path() const noexcept { return m_path; }
        std::string_view content() const noexcept { return m_content; }
        std::size_t size() const noexcept { return m_content.size(); }

        struct line_column {
            std::uint32_t                           line{};     // 1-based
            std::uint32_t                           column{};   // 1-based, in bytes
        };

        line_column resolve(std::uint32_t offset) const;

        // Text of the line containing `offset`, WITHOUT the trailing newline
        std::string_view line_of(std::uint32_t offset) const;

        // Offset of the first byte of the line containing `offset`
        std::uint32_t line_start(std::uint32_t offset) const;

    private:

        void compute_line_starts();


        file_id                                     m_id{};
        std::string                                 m_path{};
        std::string                                 m_content{};
        std::vector<std::uint32_t>                  m_line_starts{};
    };


    // Owns all loaded files. Uses a deque so `const source_file&` handles stay stable as more files are added (a vector could
    // reallocate and invalidate every in-flight token's `lexeme`)
    class source_manager {
    public:
        source_manager() = default;
        source_manager(const source_manager&) = delete;
        source_manager& operator=(const source_manager&) = delete;

        // Registers a new file. `content` is normalized (\r\n and \r -> \n)
        file_id add(std::string path, std::string content);

        const source_file& get(file_id id) const;

        std::size_t file_count() const noexcept { return m_files.size(); }

    private:

        std::deque<source_file>                     m_files{};
    };

}

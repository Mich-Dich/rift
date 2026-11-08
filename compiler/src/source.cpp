
#include "rift/source.h"

#include <algorithm>
#include <cassert>
#include <utility>



// FORWARD DECLARATIONS ================================================================================================

namespace rift {

    // CONSTANTS =======================================================================================================

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    // STATIC VARIABLES ================================================================================================

    // INTERNAL TEMPLATE DECLARATION ===================================================================================

    // INTERNAL FUNCTION DECLARATION ===================================================================================

    std::string normalize_newlines(std::string s);

    // INTERNAL TEMPLATE IMPLEMENTATION ================================================================================

    // INTERNAL FUNCTION IMPLEMENTATION ================================================================================

    std::string normalize_newlines(std::string s) {

        std::string out;
        out.reserve(s.size());
        for (std::size_t i = 0; i < s.size(); i++) {
            if (s[i] == '\r') {
                out.push_back('\n');
                if (i + 1 < s.size() && s[i + 1] == '\n') i++;
            } else {
                out.push_back(s[i]);
            }
        }
        return out;
    }

    // TEMPLATE IMPLEMENTATION =========================================================================================

    // FUNCTION IMPLEMENTATION =========================================================================================

    // CLASS IMPLEMENTATION ============================================================================================

    source_file::source_file(file_id id, std::string path, std::string content)
        : m_id(id), m_path(std::move(path)), m_content(std::move(content)) {

        compute_line_starts();
    }

    // CLASS PUBLIC ====================================================================================================

    // source_file -----------------------------------------------------------------------------------------------------

    source_file::line_column source_file::resolve(std::uint32_t offset) const {

        // Find the last line_start <= offset.
        auto it = std::upper_bound(m_line_starts.begin(), m_line_starts.end(), offset);
        const std::size_t idx = static_cast<std::size_t>(it - m_line_starts.begin()) - 1;
        const std::uint32_t start = m_line_starts[idx];
        return line_column{
            static_cast<std::uint32_t>(idx + 1),
            static_cast<std::uint32_t>(offset - start + 1),
        };
    }


    std::string_view source_file::line_of(std::uint32_t offset) const {

        auto it = std::upper_bound(m_line_starts.begin(), m_line_starts.end(), offset);
        const std::size_t idx = static_cast<std::size_t>(it - m_line_starts.begin()) - 1;
        const std::uint32_t start = m_line_starts[idx];
        std::uint32_t end = start;
        while (end < m_content.size() && m_content[end] != '\n') ++end;
        return std::string_view(m_content).substr(start, end - start);
    }


    std::uint32_t source_file::line_start(std::uint32_t offset) const {

        auto it = std::upper_bound(m_line_starts.begin(), m_line_starts.end(), offset);
        const std::size_t idx = static_cast<std::size_t>(it - m_line_starts.begin()) - 1;
        return m_line_starts[idx];
    }

    // source_manager --------------------------------------------------------------------------------------------------

    file_id source_manager::add(std::string path, std::string content) {

        const file_id id = static_cast<file_id>(m_files.size());
        m_files.emplace_back(id, std::move(path), normalize_newlines(std::move(content)));
        return id;
    }


    const source_file& source_manager::get(file_id id) const {

        assert(id < m_files.size() && "invalid file_id");
        return m_files[id];
    }

    // CLASS PROTECTED =================================================================================================

    // CLASS PRIVATE ===================================================================================================

    // source_file -----------------------------------------------------------------------------------------------------

    void source_file::compute_line_starts() {

        m_line_starts.clear();
        m_line_starts.push_back(0);
        for (std::uint32_t i = 0; i < m_content.size(); i++) {
            if (m_content[i] == '\n') {
                m_line_starts.push_back(i + 1);
            }
        }
    }

}


#pragma once

#include "rift/span.h"
#include "rift/types.h"
#include "rift/ast.h"

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>



// FORWARD DECLARATIONS ================================================================================================

namespace rift {

    // CONSTANTS =======================================================================================================

    using symbol_id = std::uint32_t;

    inline constexpr symbol_id                      invalid_symbol = 0;

    // TYPES ===========================================================================================================

    enum class symbol_kind : std::uint8_t {

        function = 0,
        parameter,
        local,
    };


    struct symbol {

        symbol_id                                   id = invalid_symbol;
        symbol_kind                                 kind = symbol_kind::local;
        std::string_view                            name;
        type_id                                     type = invalid_type;
        bool                                        is_mut = false;
        source_span                                 span;

        // Functions only:
        type_id                                     return_type = invalid_type;
        node_id                                     definition;
    };

    // STATIC VARIABLES ================================================================================================

    // FUNCTION DECLARATION ============================================================================================

    // TEMPLATE DECLARATION ============================================================================================

    // CLASS DECLARATION ===============================================================================================

    // Symbol table with lexical scopes. M3 is small enough that linear search through each scope is fine; when a function
    // has 50+ locals, swap the per-scope vector for an unordered_map
    //
    // Function symbols live in a separate "global" namespace that is not affected by push_scope()/pop_scope()
    class symbol_table {
    public:

        symbol_table();

        symbol_id add(symbol s);

        const symbol& get(symbol_id id) const;

        std::size_t size() const noexcept { return m_symbols.size(); }

        void push_scope();

        void pop_scope();

        // Declares `name` in the innermost scope. Returns invalid_symbol on redeclaration in the same scope; callers should report and skip
        symbol_id declare(std::string_view name, symbol s);

        // Resolves a name from the innermost scope outwards
        symbol_id lookup(std::string_view name) const;

        // Global function namespace, unaffected by scopes
        symbol_id declare_global_function(std::string_view name, symbol s);

        symbol_id lookup_function(std::string_view name) const;

    private:

        struct scope {
            std::vector<std::pair<std::string_view, symbol_id>>         entries;
        };

        std::vector<symbol>                                             m_symbols;
        std::vector<scope>                                              m_scopes;
        std::vector<std::pair<std::string_view, symbol_id>>             m_globals;
    };

}

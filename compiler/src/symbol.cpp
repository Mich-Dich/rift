
#include "rift/symbol.h"

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

    // INTERNAL TEMPLATE IMPLEMENTATION ================================================================================

    // INTERNAL FUNCTION IMPLEMENTATION ================================================================================

    // TEMPLATE IMPLEMENTATION =========================================================================================

    // FUNCTION IMPLEMENTATION =========================================================================================

    // CLASS IMPLEMENTATION ============================================================================================

    // CLASS PUBLIC ====================================================================================================

    symbol_table::symbol_table() {

        // Reserve symbol 0 so invalid_symbol is never a real symbol.
        m_symbols.push_back(symbol{});
        m_scopes.emplace_back();      // file scope (empty; functions live in m_globals)
    }


    symbol_id symbol_table::add(symbol s) {

        const symbol_id id = static_cast<symbol_id>(m_symbols.size());
        s.id = id;
        m_symbols.push_back(std::move(s));
        return id;
    }


    const symbol& symbol_table::get(symbol_id id) const {

        assert(id < m_symbols.size() && "invalid symbol_id");
        return m_symbols[id];
    }


    void symbol_table::push_scope() { m_scopes.emplace_back(); }


    void symbol_table::pop_scope() {

        assert(m_scopes.size() > 1 && "popped the file scope");
        m_scopes.pop_back();
    }


    symbol_id symbol_table::declare(std::string_view name, symbol s) {

        auto& loc_scope = m_scopes.back();
        for (const auto& [n, _] : loc_scope.entries) {
            if (n == name)
                return invalid_symbol;
        }
        const symbol_id id = add(std::move(s));
        loc_scope.entries.emplace_back(name, id);
        return id;
    }


    symbol_id symbol_table::lookup(std::string_view name) const {

        for (auto it = m_scopes.rbegin(); it != m_scopes.rend(); ++it) {
            for (const auto& [n, id] : it->entries)
                if (n == name)
                    return id;
        }
        return invalid_symbol;
    }


    symbol_id symbol_table::declare_global_function(std::string_view name, symbol s) {

        for (const auto& [n, _] : m_globals)
            if (n == name)
                return invalid_symbol;
        const symbol_id id = add(std::move(s));
        m_globals.emplace_back(name, id);
        return id;
    }


    symbol_id symbol_table::lookup_function(std::string_view name) const {

        for (const auto& [n, id] : m_globals)
            if (n == name)
                return id;
        return invalid_symbol;
    }

    // CLASS PROTECTED =================================================================================================

    // CLASS PRIVATE ===================================================================================================

}

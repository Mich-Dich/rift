
#pragma once

#include "rift/token.h"

#include <cstdint>
#include <string_view>



// FORWARD DECLARATIONS ================================================================================================

namespace rift {

    // CONSTANTS =======================================================================================================

    // type_id is a small integer handle. 0 is reserved for "invalid" — the *error type* that failed checks produce
    // Any operation on an error- type value is silently skipped, which suppresses cascading diagnostics
    //
    // Primitive types occupy IDs 1..14 in the order below. User-defined types will get IDs from 15 onward in M6 (struct/enum/class)
    // Do not reorder; only append
    using type_id = std::uint32_t;

    inline constexpr type_id invalid_type = 0;

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    enum class primitive_type : std::uint8_t {

        u8 = 0, u16, u32, u64, u128,
        i8, i16, i32, i64, i128,
        f32, f64,
        bool_,
        void_,
        count
    };


    struct type_info {

        std::string_view                name;         // "u32", "bool", "<invalid>"
        std::uint8_t                    size_bytes;   // 0 for void and invalid
        bool                            is_integer;
        bool                            is_signed;    // meaningful only when is_integer
        bool                            is_float;
        bool                            is_bool;
        bool                            is_void;

        bool is_numeric() const noexcept { return is_integer || is_float; }
    };


    // Inclusive range for integer types. `valid == false` means "no range constraint known" (128-bit types, non-integer types)
    struct integer_range {

        bool                            valid = false;
        bool                            is_signed = false;
        std::uint64_t                   max_u = 0;    // for unsigned types
        std::int64_t                    min_s = 0;    // for signed types
        std::int64_t                    max_s = 0;    // for signed types
    };

    // STATIC VARIABLES ================================================================================================

    // FUNCTION DECLARATION ============================================================================================

    constexpr type_id primitive_type_id(token_kind k) noexcept {

        switch (k) {
            case token_kind::prim_u8:       return 1 + static_cast<type_id>(primitive_type::u8);
            case token_kind::prim_u16:      return 1 + static_cast<type_id>(primitive_type::u16);
            case token_kind::prim_u32:      return 1 + static_cast<type_id>(primitive_type::u32);
            case token_kind::prim_u64:      return 1 + static_cast<type_id>(primitive_type::u64);
            case token_kind::prim_u128:     return 1 + static_cast<type_id>(primitive_type::u128);
            case token_kind::prim_i8:       return 1 + static_cast<type_id>(primitive_type::i8);
            case token_kind::prim_i16:      return 1 + static_cast<type_id>(primitive_type::i16);
            case token_kind::prim_i32:      return 1 + static_cast<type_id>(primitive_type::i32);
            case token_kind::prim_i64:      return 1 + static_cast<type_id>(primitive_type::i64);
            case token_kind::prim_i128:     return 1 + static_cast<type_id>(primitive_type::i128);
            case token_kind::prim_f32:      return 1 + static_cast<type_id>(primitive_type::f32);
            case token_kind::prim_f64:      return 1 + static_cast<type_id>(primitive_type::f64);
            case token_kind::prim_bool:     return 1 + static_cast<type_id>(primitive_type::bool_);
            case token_kind::prim_void:     return 1 + static_cast<type_id>(primitive_type::void_);
            default:                        return invalid_type;
        }
    }

    type_info type_info_of(type_id id) noexcept;
    integer_range int_range_of(type_id id) noexcept;

    bool is_integer(type_id id) noexcept;
    bool is_numeric(type_id id) noexcept;
    bool is_bool(type_id id) noexcept;
    bool is_void(type_id id) noexcept;

    // TEMPLATE DECLARATION ============================================================================================

    // CLASS DECLARATION ===============================================================================================

}

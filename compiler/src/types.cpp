#include "rift/types.h"



// FORWARD DECLARATIONS ================================================================================================

namespace rift {

    // CONSTANTS =======================================================================================================

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    // STATIC VARIABLES ================================================================================================

    // INTERNAL TEMPLATE DECLARATION ===================================================================================

    // INTERNAL FUNCTION DECLARATION ===================================================================================

    type_info make_info(std::string_view name, std::uint8_t size, bool is_int, bool is_signed, bool is_float, bool is_bool, bool is_void);

    // INTERNAL TEMPLATE IMPLEMENTATION ================================================================================

    // INTERNAL FUNCTION IMPLEMENTATION ================================================================================

    type_info make_info(std::string_view name, std::uint8_t size, bool is_int, bool is_signed, bool is_float, bool is_bool, bool is_void) {

        return type_info{ name, size, is_int, is_signed, is_float, is_bool, is_void };
    }

    // TEMPLATE IMPLEMENTATION =========================================================================================

    // FUNCTION IMPLEMENTATION =========================================================================================

    type_info type_info_of(type_id id) noexcept {

        if (id == invalid_type)
            return make_info("<invalid>", 0, false, false, false, false, false);

        const auto prim = static_cast<primitive_type>(id - 1);
        switch (prim) {
            case primitive_type::u8:            return make_info("u8",   1, true,  false, false, false, false);
            case primitive_type::u16:           return make_info("u16",  2, true,  false, false, false, false);
            case primitive_type::u32:           return make_info("u32",  4, true,  false, false, false, false);
            case primitive_type::u64:           return make_info("u64",  8, true,  false, false, false, false);
            case primitive_type::u128:          return make_info("u128", 16, true, false, false, false, false);
            case primitive_type::i8:            return make_info("i8",   1, true,  true,  false, false, false);
            case primitive_type::i16:           return make_info("i16",  2, true,  true,  false, false, false);
            case primitive_type::i32:           return make_info("i32",  4, true,  true,  false, false, false);
            case primitive_type::i64:           return make_info("i64",  8, true,  true,  false, false, false);
            case primitive_type::i128:          return make_info("i128", 16, true,  true,  false, false, false);
            case primitive_type::f32:           return make_info("f32",  4, false, false, true,  false, false);
            case primitive_type::f64:           return make_info("f64",  8, false, false, true,  false, false);
            case primitive_type::bool_:         return make_info("bool", 1, false, false, false, true,  false);
            case primitive_type::void_:         return make_info("void", 0, false, false, false, false, true);
            case primitive_type::count:         break;
        }
        return make_info("<invalid>", 0, false, false, false, false, false);
    }


    bool is_integer(type_id id) noexcept { return id != invalid_type && type_info_of(id).is_integer; }
    bool is_numeric(type_id id) noexcept { return id != invalid_type && type_info_of(id).is_numeric(); }
    bool is_bool(type_id id) noexcept { return id != invalid_type && type_info_of(id).is_bool; }
    bool is_void(type_id id) noexcept { return id != invalid_type && type_info_of(id).is_void; }


    integer_range int_range_of(type_id id) noexcept {

        if (!is_integer(id))
            return {};

        const auto info = type_info_of(id);

        // 128-bit types: no range constraint reported. Their full range (up to 2^128 - 1) doesn't fit into uint64_t; check at literal-
        // parse time instead. M3 tolerates any in-band literal here
        if (info.size_bytes == 16)
            return {};

        switch (info.size_bytes) {
            case 1:
                if (info.is_signed)
                    return integer_range{ true, true,  0, -128, 127 };
                return integer_range{ true, false, 255, 0, 0 };
            case 2:
                if (info.is_signed)
                    return integer_range{ true, true,  0, -32768, 32767 };
                return integer_range{ true, false, 65535, 0, 0 };
            case 4:
                if (info.is_signed)
                    return integer_range{ true, true,  0, -2147483648LL, 2147483647LL };
                return integer_range{ true, false, 4294967295ULL, 0, 0 };
            case 8:
                if (info.is_signed)
                    return integer_range{ true, true, 0, INT64_MIN, INT64_MAX };
                return integer_range{ true, false, UINT64_MAX, 0, 0 };
            default:
                return {};
        }
    }

    // CLASS IMPLEMENTATION ============================================================================================

    // CLASS PUBLIC ====================================================================================================

    // CLASS PROTECTED =================================================================================================

    // CLASS PRIVATE ===================================================================================================

}

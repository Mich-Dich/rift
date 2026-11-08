
#include "rift/token.h"



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

    std::string_view token_kind_name(token_kind k) noexcept {

        switch (k) {
            case token_kind::end_of_file:               return "eof";
            case token_kind::invalid:                   return "invalid";

            case token_kind::identifier:                return "identifier";
            case token_kind::integer_literal:           return "int_literal";
            case token_kind::float_literal:             return "float_literal";
            case token_kind::string_literal:            return "string_literal";
            case token_kind::char_literal:              return "char_literal";

            case token_kind::kw_import:                 return "import";
            case token_kind::kw_struct:                 return "struct";
            case token_kind::kw_class:                  return "class";
            case token_kind::kw_open:                   return "open";
            case token_kind::kw_enum:                   return "enum";
            case token_kind::kw_interface:              return "interface";
            case token_kind::kw_impl:                   return "impl";

            case token_kind::kw_virtual:                return "virtual";
            case token_kind::kw_override:               return "override";
            case token_kind::kw_mut:                    return "mut";
            case token_kind::kw_private:                return "private";
            case token_kind::kw_static:                 return "static";

            case token_kind::kw_compile_time:           return "compile_time";
            case token_kind::kw_run_time:               return "run_time";
            case token_kind::kw_deduce:                 return "deduce";

            case token_kind::kw_if:                     return "if";
            case token_kind::kw_else:                   return "else";
            case token_kind::kw_match:                  return "match";
            case token_kind::kw_for:                    return "for";
            case token_kind::kw_in:                     return "in";
            case token_kind::kw_return:                 return "return";

            case token_kind::kw_true:                   return "true";
            case token_kind::kw_false:                  return "false";
            case token_kind::kw_null:                   return "null";

            case token_kind::kw_cast:                   return "cast";
            case token_kind::kw_unsafe:                 return "unsafe";
            case token_kind::kw_auto:                   return "auto";
            case token_kind::kw_default:                return "default";
            case token_kind::kw_bit:                    return "bit";

            case token_kind::prim_u8:                   return "u8";
            case token_kind::prim_u16:                  return "u16";
            case token_kind::prim_u32:                  return "u32";
            case token_kind::prim_u64:                  return "u64";
            case token_kind::prim_u128:                 return "u128";
            case token_kind::prim_i8:                   return "i8";
            case token_kind::prim_i16:                  return "i16";
            case token_kind::prim_i32:                  return "i32";
            case token_kind::prim_i64:                  return "i64";
            case token_kind::prim_i128:                 return "i128";
            case token_kind::prim_f32:                  return "f32";
            case token_kind::prim_f64:                  return "f64";
            case token_kind::prim_bool:                 return "bool";
            case token_kind::prim_void:                 return "void";

            case token_kind::l_brace:                   return "{";
            case token_kind::r_brace:                   return "}";
            case token_kind::l_paren:                   return "(";
            case token_kind::r_paren:                   return ")";
            case token_kind::l_bracket:                 return "[";
            case token_kind::r_bracket:                 return "]";
            case token_kind::semicolon:                 return ";";
            case token_kind::comma:                     return ",";
            case token_kind::dot:                       return ".";
            case token_kind::dot_dot:                   return "..";
            case token_kind::dot_dot_eq:                return "..=";
            case token_kind::colon:                     return ":";
            case token_kind::double_colon:              return "::";
            case token_kind::arrow:                     return "->";
            case token_kind::fat_arrow:                 return "=>";
            case token_kind::underscore:                return "_";

            case token_kind::plus:                      return "+";
            case token_kind::minus:                     return "-";
            case token_kind::star:                      return "*";
            case token_kind::slash:                     return "/";
            case token_kind::percent:                   return "%";

            case token_kind::eq_eq:                     return "==";
            case token_kind::bang_eq:                   return "!=";
            case token_kind::lt:                        return "<";
            case token_kind::lt_eq:                     return "<=";
            case token_kind::gt:                        return ">";
            case token_kind::gt_eq:                     return ">=";

            case token_kind::amp_amp:                   return "&&";
            case token_kind::pipe_pipe:                 return "||";
            case token_kind::bang:                      return "!";

            case token_kind::amp:                       return "&";
            case token_kind::pipe:                      return "|";
            case token_kind::caret:                     return "^";
            case token_kind::tilde:                     return "~";
            case token_kind::lt_lt:                     return "<<";
            case token_kind::gt_gt:                     return ">>";

            case token_kind::eq:                        return "=";
            case token_kind::plus_eq:                   return "+=";
            case token_kind::minus_eq:                  return "-=";
            case token_kind::star_eq:                   return "*=";
            case token_kind::slash_eq:                  return "/=";
            case token_kind::percent_eq:                return "%=";
            case token_kind::amp_eq:                    return "&=";
            case token_kind::pipe_eq:                   return "|=";
            case token_kind::caret_eq:                  return "^=";
            case token_kind::lt_lt_eq:                  return "<<=";
            case token_kind::gt_gt_eq:                  return ">>=";

            case token_kind::question:                  return "?";

            case token_kind::count: break;
        }
        return "<unknown>";
    }

    // CLASS IMPLEMENTATIO   ============================================================================================

    // CLASS PUBLIC ====================================================================================================

    // CLASS PROTECTED =================================================================================================

    // CLASS PRIVATE ===================================================================================================

}


#pragma once

#include <cstdint>
#include <string>
#include <span>
#include <meta>



// FORWARD DECLARATIONS ================================================================================================

namespace rift::util {

    // CONSTANTS =======================================================================================================

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    // STATIC VARIABLES ================================================================================================

    // FUNCTION DECLARATION ============================================================================================

    std::uint64_t hash64(std::span<const std::byte> data, std::uint64_t seed = 0) noexcept;


    std::string format_bytes(std::uint64_t bytes);


    // Mix several 64-bit values into one. XOR + FNV-1a-style multiply; deterministic.
    std::uint64_t mix_u64(std::uint64_t acc, std::uint64_t value);


    // FNV-1a-64 over a string_view.
    std::uint64_t fnv1a_64(std::string_view s);

    // TEMPLATE DECLARATION ============================================================================================

    template <typename E>
    requires std::is_enum_v<E>
    constexpr std::string_view enum_to_string(E value);


    // Convert a STRING to an optional enum value.
    template <typename E>
    requires std::is_enum_v<E>
    constexpr std::optional<E> string_to_enum(std::string_view str);


    template <typename E>
    requires std::is_enum_v<E>
    inline constexpr auto enum_enumerators = std::define_static_array(std::meta::enumerators_of(^^E));


    template <typename E, std::size_t... Is>
    constexpr auto make_enum_values(std::index_sequence<Is...>) { return std::array<E, sizeof...(Is)>{ [: enum_enumerators<E>[Is] :]... }; }


    template <typename E>
    requires std::is_enum_v<E>
    inline constexpr auto enum_values = make_enum_values<E>(std::make_index_sequence<enum_enumerators<E>.size()>{});


    template <typename E, std::size_t... Is>
    constexpr auto make_enum_name_ptrs(std::index_sequence<Is...>) {
        return std::array<const char*, sizeof...(Is)>{
            std::define_static_string(std::meta::identifier_of(GLT::util::enum_enumerators<E>[Is]))...
        };
    }


    template <typename E>
    requires std::is_enum_v<E>
    inline constexpr auto enum_name_ptrs = make_enum_name_ptrs<E>(std::make_index_sequence<GLT::util::enum_enumerators<E>.size()>{});


    // Appends the bytes at the end of the vector
    template<std::unsigned_integral T>
    constexpr void set_bytes(std::vector<std::byte>& array, T value) noexcept;


    template<typename T> requires(std::is_arithmetic_v<T> && sizeof(T) <= 8)
    constexpr T get_bytes(const std::vector<std::byte>& array, const size_t startIndex);

    // CLASS DECLARATION ===============================================================================================

}

#include "util.inl"

namespace std {

    // For standard containers with begin()/end() methods
    template <typename C, typename T>
    bool contains(const C& container, const T& item) { 
        
        return std::find(container.begin(), container.end(), item) != container.end();
    }

    // Overload for C-style arrays
    template <typename T, size_t N>
    bool contains(const T (&array)[N], const T& item) { return std::find(array, array + N, item) != array + N; }

    template<>
    struct formatter<filesystem::path> : formatter<string_view> {
        
        auto format(const filesystem::path& p, format_context& ctx) const { return formatter<string_view>::format(p.string(), ctx); }
    };

}

#pragma once

#include <cstring>
#include <cstdint>
#include <string>
#include <span>
#include <meta>


// FORWARD DECLARATIONS ================================================================================================

namespace rift::util {

    // CONSTANTS =======================================================================================================

    inline constexpr std::uint64_t                    P0 = 0xa0761d6478bd642fULL;

    inline constexpr std::uint64_t                    P1 = 0xe7037ed1a0b428dbULL;

    inline constexpr std::uint64_t                    P2 = 0x8ebc6af09c88c6e3ULL;

    inline constexpr std::uint64_t                    P3 = 0x589965cc75374cc3ULL;

    inline constexpr std::uint64_t                    P4 = 0x1d8e4e27c47d124fULL;

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    // STATIC VARIABLES ================================================================================================

    // INTERNAL TEMPLATE DECLARATION ===================================================================================

    // INTERNAL FUNCTION DECLARATION ===================================================================================

    std::uint64_t mum(std::uint64_t a, std::uint64_t b) noexcept;


    std::uint64_t r8(const std::byte* p) noexcept;


    std::uint64_t r4(const std::byte* p) noexcept;


    std::uint64_t r3(const std::byte* p, std::size_t k) noexcept;

    // INTERNAL TEMPLATE IMPLEMENTATION ================================================================================

    // INTERNAL FUNCTION IMPLEMENTATION ================================================================================

    std::uint64_t mum(std::uint64_t a, std::uint64_t b) noexcept {

        __extension__ unsigned __int128 r = (unsigned __int128)a * b;
        return static_cast<std::uint64_t>(r) ^ static_cast<std::uint64_t>(r >> 64);
    }


    std::uint64_t r8(const std::byte* p) noexcept {

        std::uint64_t v;
        std::memcpy(&v, p, 8);   // compiles to a single mov on x86/ARM
        return v;
    }


    std::uint64_t r4(const std::byte* p) noexcept {

        std::uint32_t v;
        std::memcpy(&v, p, 4);
        return v;
    }


    std::uint64_t r3(const std::byte* p, std::size_t k) noexcept {

        return (std::uint64_t(std::to_integer<u8>(p[0])) << 16) | (std::uint64_t(std::to_integer<u8>(p[k>>1])) <<  8) | std::uint64_t(std::to_integer<u8>(p[k-1]));
    }

    // FUNCTION IMPLEMENTATION =========================================================================================

    std::uint64_t hash64(std::span<const std::byte> data, std::uint64_t seed) noexcept {

        const std::byte* p = data.data();
        std::size_t len = data.size();
        seed ^= P0;

        // 32-byte stripes
        while (len >= 32) {
            seed = mum(r8(p) ^ P1, r8(p +  8) ^ seed) ^ mum(r8(p + 16) ^ P2, r8(p + 24) ^ P3);
            p += 32;
            len -= 32;
        }

        // 8-byte stripes
        while (len >= 8) {
            seed = mum(r8(p) ^ P1, seed);
            p += 8;
            len -= 8;
        }

        // 1..7 byte tail
        if (len) {
            std::uint64_t a, b;
            if (len >= 4) {
                a = r4(p);
                b = r4(p + len - 4);
            } else {
                a = r3(p, len);
                b = 0;
            }
            seed = mum(P1 ^ len, mum(a ^ P1, b ^ seed));
        }
        return mum(seed ^ len, P4);
    }


    std::string format_bytes(std::uint64_t bytes) {

        constexpr const char* units[] = { "B", "KB", "MB", "GB", "TB" };
        double v = static_cast<double>(bytes);
        int u = 0;
        while (v >= 1024.0 && u < 4) { 
            v /= 1024.0; 
            u++;
        }

        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.2f %s", v, units[u]);
        return buf;
    }
    

    // Mix several 64-bit values into one. XOR + FNV-1a-style multiply; deterministic.
    std::uint64_t mix_u64(std::uint64_t acc, std::uint64_t value) {

        acc ^= value;
        acc *= 0x100000001B3ull;
        return acc;
    }


    // FNV-1a-64 over a string_view.
    std::uint64_t fnv1a_64(std::string_view s) {

        std::uint64_t h = 14695981039346656037ull;
        for (const char c : s) {
            h ^= static_cast<u8>(c);
            h *= 0x100000001B3ull;
        }
        return h;
    }
    
    // TEMPLATE IMPLEMENTATION =========================================================================================

    template <typename E>
    requires std::is_enum_v<E>
    constexpr std::string_view enum_to_string(E value) {

        constexpr std::size_t N = std::meta::enumerators_of(^^E).size();
        return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            std::string_view result = "<unnamed>";
            ((value == [:std::meta::enumerators_of(^^E)[Is]:]
                ? (result = std::meta::identifier_of(std::meta::enumerators_of(^^E)[Is]), true)
                : false) || ...);
            return result;
        }(std::make_index_sequence<N>{});
    }


    template <typename E>
    requires std::is_enum_v<E>
    constexpr std::optional<E> string_to_enum(std::string_view str) {

        constexpr std::size_t N = std::meta::enumerators_of(^^E).size();
        return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            std::optional<E> result;
            ((str == std::meta::identifier_of(std::meta::enumerators_of(^^E)[Is])
                ? (result = [:std::meta::enumerators_of(^^E)[Is]:], true)
                : false) || ...);
            return result;
        }(std::make_index_sequence<N>{});
    }


    template<std::unsigned_integral T>
    constexpr void set_bytes(std::vector<std::byte>& array, T value) noexcept {

        static_assert(sizeof(T) <= 8, "Only up to 64-bit unsigned integers are supported");

        const auto buffer = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);

        if constexpr (std::endian::native == std::endian::little) {

            /** Native layout is already little-endian: emit bytes in memory order */
            for (const auto b : buffer)
                array.push_back(b);

        } else {

            /** Serialize as little-endian: emit low byte first */
            for (size_t i = sizeof(T); i > 0; --i)
                array.push_back(buffer[i - 1]);
        }
    }


    template<typename T> requires(std::is_arithmetic_v<T> && sizeof(T) <= 8)
    constexpr T get_bytes(const std::vector<std::byte>& array, const size_t startIndex) {

        static_assert(sizeof(T) <= 8, "Only up to 64-bit unsigned integers are supported");

        const auto minSize = sizeof(T);
        if (startIndex + minSize > array.size())
            throw std::out_of_range("Not enough bytes in vector");

        T value = 0;
        if constexpr (std::endian::native == std::endian::little)
            std::memcpy(&value, array.data() + startIndex, sizeof(T));      /** Direct copy for little endian systems */

        else {

            /** Byte reversal for big endian systems */
            for (size_t i = 0; i < sizeof(T); ++i)
                value |= static_cast<T>(array[startIndex + i]) << (i * 8);
        }
        return value;
    }

    // TEMPLATE CLASS IMPLEMENTATION ===================================================================================

    // TEMPLATE CLASS PUBLIC ===========================================================================================

    // TEMPLATE CLASS PROTECTED ========================================================================================

    // TEMPLATE CLASS PRIVATE ==========================================================================================

}

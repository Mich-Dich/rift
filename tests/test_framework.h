
#pragma once

// Minimal, dependency-free test harness. Each `TEST(name) { ... }` registers a function that throws `test::failure` on the
// first failed EXPECT. `run_all()` executes them in registration order and returns a process exit code
//
// Not trying to compete with Catch2/doctest — just enough for M1's lexer unit tests. We can swap in a real framework later
// if test metadata (tags, fixtures, generators) becomes valuable

#include <concepts>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>



// FORWARD DECLARATIONS ================================================================================================

namespace test {

    // CONSTANTS =======================================================================================================

    // MACROS ==========================================================================================================

    #define TEST(name)                                                                                                  \
        static void rift_test_##name();                                                                                 \
        static const int rift_test_##name##_reg = (::test::registry().push_back({#name, rift_test_##name}), 0);         \
        static void rift_test_##name()


    #define EXPECT_TRUE(expr)                                                                                           \
        do {                                                                                                            \
            if (!(expr))                                                                                                \
                ::test::report_true_failure(__FILE__, __LINE__, #expr, expr);                                           \
        } while (0)

    #define EXPECT_FALSE(expr) EXPECT_TRUE(!(expr))


    #define EXPECT_EQ(a, b)                                                                                             \
        do {                                                                                                            \
            auto&& _a = (a);                                                                                            \
            auto&& _b = (b);                                                                                            \
            if (!(_a == _b))                                                                                            \
                ::test::report_eq_failure(__FILE__, __LINE__, #a, #b, _a, _b);                                          \
        } while (0)

    // TYPES ===========================================================================================================

    // A type is "streamable" if it can be written to std::ostream
    template <typename T>
    concept streamable = requires(std::ostream& os, const T& v) { os << v; };


    struct test_case {

        std::string                             name;
        std::function<void()>                   body;
    };


    struct failure : std::exception {

        std::string                             message;
        explicit failure(std::string m) : message(std::move(m)) {}
        const char* what() const noexcept override { return message.c_str(); }
    };

    // STATIC VARIABLES ================================================================================================

    // FUNCTION DECLARATION ============================================================================================

    inline std::vector<test_case>& registry() {

        static std::vector<test_case> r;
        return r;
    }


    inline int run_all() {

        int failed = 0;
        for (const auto& tc : registry()) {
            try {
                tc.body();
                std::cout << "[PASS] " << tc.name << '\n';
            } catch (const failure& f) {
                std::cout << "[FAIL] " << tc.name << '\n' << "       " << f.message << '\n';
                failed++;
            } catch (const std::exception& e) {
                std::cout << "[FAIL] " << tc.name << '\n' << "       unexpected exception: " << e.what() << '\n';
                failed++;
            }
        }
        if (failed == 0) {
            std::cout << "All " << registry().size() << " tests passed.\n";
            return 0;
        }
        std::cout << failed << " of " << registry().size() << " tests failed.\n";
        return 1;
    }

    // TEMPLATE DECLARATION ============================================================================================

    template <typename V>
    [[noreturn]] void report_true_failure(const char* file, int line, const char* expr, const V& v) {

        std::ostringstream os;
        os << file << ':' << line << ": EXPECT_TRUE(" << expr << ")\n";
        if constexpr (streamable<V>)
            os << "         value: " << v << '\n';
        throw failure(os.str());
    }


    template <typename A, typename B>
    [[noreturn]] void report_eq_failure(const char* file, int line, const char* expr_a, const char* expr_b, const A& a, const B& b) {

        std::ostringstream os;
        os << file << ':' << line
           << ": EXPECT_EQ(" << expr_a << ", " << expr_b << ")\n";
        if constexpr (streamable<A>)
            os << "         left:  " << a << '\n';
        if constexpr (streamable<B>)
            os << "         right: " << b << '\n';
        throw failure(os.str());
    }

    // CLASS DECLARATION ===============================================================================================

}

#pragma once
// rift_test: a tiny, dependency-free unit test framework.
//
//   TEST_CASE("lexer: integer literals") {
//       REQUIRE(tokens.size() == 3);           // stops this test case on failure
//       CHECK_EQ(tokens[0].kind, TokenKind::int_literal);   // records failure, continues
//   }
//
// The test binary accepts:
//   <substring>...   run only test cases whose name contains one of the substrings
//   --list           list test case names
//   -v, --verbose    print every test case, not just failures
//
// Output ends with a machine-readable line the test runner parses:
//   summary: <passed> passed, <failed> failed, <checks> checks

#include <concepts>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace rift_test {

using TestFn = void (*)();

struct TestCase {
    std::string_view name;
    std::string_view file;
    int              line;
    TestFn           fn;
};

std::vector<TestCase>& registry();

struct Registrar {
    Registrar(std::string_view name, std::string_view file, int line, TestFn fn) {
        registry().push_back(TestCase{name, file, line, fn});
    }
};

/// Thrown by REQUIRE to abort the current test case.
struct AbortTest {};

void record_check();
void record_failure(std::string_view file, int line, std::string_view expression,
                    std::string_view detail);

template <typename T>
concept Streamable = requires(std::ostream& os, const T& v) { os << v; };

template <typename T>
std::string show(const T& value) {
    std::ostringstream out;
    if constexpr (std::is_same_v<T, bool>) {
        out << (value ? "true" : "false");
    } else if constexpr (std::is_same_v<T, char>) {
        out << '\'' << value << '\'';
    } else if constexpr (std::is_convertible_v<const T&, std::string_view>) {
        out << '"' << std::string_view(value) << '"';
    } else if constexpr (Streamable<T>) {
        out << value;
    } else if constexpr (std::is_enum_v<T>) {
        out << "enum(" << static_cast<long long>(value) << ")";
    } else {
        out << "<unprintable>";
    }
    return out.str();
}

template <typename A, typename B>
bool check_eq(const A& a, const B& b, std::string_view file, int line, std::string_view expr) {
    record_check();
    if (a == b) {
        return true;
    }
    record_failure(file, line, expr, "left:  " + show(a) + "\nright: " + show(b));
    return false;
}

inline bool check_true(bool ok, std::string_view file, int line, std::string_view expr) {
    record_check();
    if (!ok) {
        record_failure(file, line, expr, "");
    }
    return ok;
}

} // namespace rift_test

#define RIFT_TEST_CAT2(a, b) a##b
#define RIFT_TEST_CAT(a, b) RIFT_TEST_CAT2(a, b)

#define TEST_CASE(name)                                                                          \
    static void RIFT_TEST_CAT(rift_test_fn_, __LINE__)();                                        \
    static const ::rift_test::Registrar RIFT_TEST_CAT(rift_test_reg_, __LINE__){                 \
        name, __FILE__, __LINE__, &RIFT_TEST_CAT(rift_test_fn_, __LINE__)};                      \
    static void RIFT_TEST_CAT(rift_test_fn_, __LINE__)()

#define CHECK(expr) ::rift_test::check_true(static_cast<bool>(expr), __FILE__, __LINE__, "CHECK(" #expr ")")
#define CHECK_EQ(a, b) ::rift_test::check_eq((a), (b), __FILE__, __LINE__, "CHECK_EQ(" #a ", " #b ")")

#define REQUIRE(expr)                                                                            \
    do {                                                                                         \
        if (!::rift_test::check_true(static_cast<bool>(expr), __FILE__, __LINE__,               \
                                     "REQUIRE(" #expr ")"))                                      \
            throw ::rift_test::AbortTest{};                                                      \
    } while (false)

#define REQUIRE_EQ(a, b)                                                                         \
    do {                                                                                         \
        if (!::rift_test::check_eq((a), (b), __FILE__, __LINE__, "REQUIRE_EQ(" #a ", " #b ")"))  \
            throw ::rift_test::AbortTest{};                                                      \
    } while (false)

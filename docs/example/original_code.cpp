
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <expected>
#include <fstream>
#include <iostream>
#include <print>
#include <ranges>
#include <stdfloat>
#include <string>
#include <string_view>
#include <vector>

// The closest equivalents the standard provides:
using i8  = std::int8_t;    using u8  = std::uint8_t;
using i16 = std::int16_t;   using u16 = std::uint16_t;
using i32 = std::int32_t;   using u32 = std::uint32_t;
using i64 = std::int64_t;   using u64 = std::uint64_t;
using f32 = std::float32_t; using f64 = std::float64_t;
// NOTE: i128 / u128 / f128 do NOT exist in the standard library.
//       __int128 is a GCC/Clang extension. std::float128_t exists only
//       if __STDCPP_FLOAT128_T__ is defined (rare).

struct employee {
    std::string         name;
    u32                 age = 0;
    f64                 salary = 0.0;
};

enum class parse_error : u8 {
    bad_line   = 0,
    bad_number,       // implicitly 1
};

enum class exit_code : i32 {
    success = 0,
    failure,          // implicitly 1
};


std::expected<employee, parse_error> parse_line(std::string_view line) {

    auto parts = line | std::views::split(',') | std::views::transform([](auto r) {
        return std::string(r.begin(), r.end());
    }) | std::ranges::to<std::vector<std::string>>();

    if (parts.size() != 3)
        return std::unexpected(parse_error::bad_line);

    u32 parsed_age = 0;
    auto [p1, ec1] = std::from_chars(
        parts[1].data(), parts[1].data() + parts[1].size(), parsed_age);
    if (ec1 != std::errc{})
        return std::unexpected(parse_error::bad_number);

    f64 parsed_salary = 0.0;
    auto [p2, ec2] = std::from_chars(
        parts[2].data(), parts[2].data() + parts[2].size(), parsed_salary);
    if (ec2 != std::errc{})
        return std::unexpected(parse_error::bad_number);

    return employee{std::move(parts[0]), parsed_age, parsed_salary};
}


std::expected<std::vector<employee>, std::string> load(const std::string& path) {

    std::ifstream in(path);
    if (!in) return std::unexpected("cannot open " + path);

    std::vector<employee> employees;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto r = parse_line(line);
        if (!r) {
            std::cerr << "skipping bad line: " << line << '\n';
            continue;
        }
        employees.push_back(std::move(*r));
    }
    return employees;
}


int main() {                       // ← C++ requires int, not an enum
    auto employees = load("employees.csv");
    if (!employees) {
        std::println(std::cerr, "error: {}", employees.error());
        return int(exit_code::failure);
    }

    auto filtered = *employees
        | std::views::filter([](const employee& e) { return e.age >= 30; })
        | std::ranges::to<std::vector<employee>>();

    std::ranges::sort(filtered,
        [](const employee& a, const employee& b) {
            return a.salary > b.salary;
        });

    for (const auto& e : filtered) {
        std::println("{:<20} {:>3} {:>10.2f}", e.name, e.age, e.salary);
    }
    return int(exit_code::success);
}

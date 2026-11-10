#include "rift_test.hpp"

#include <exception>
#include <iostream>
#include <string>

namespace rift_test {

namespace {

struct State {
    std::size_t checks          = 0;
    std::size_t failures_in_case = 0;
    std::string current;
    std::string failure_text;
};

State& state() {
    static State s;
    return s;
}

} // namespace

std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

void record_check() { ++state().checks; }

void record_failure(std::string_view file, int line, std::string_view expression,
                    std::string_view detail) {
    State& s = state();
    ++s.failures_in_case;
    s.failure_text += "    " + std::string(file) + ":" + std::to_string(line) + ": " +
                      std::string(expression) + " failed\n";
    // Indent every detail line under the failing expression.
    std::string_view rest = detail;
    while (!rest.empty()) {
        std::size_t      nl   = rest.find('\n');
        std::string_view text = rest.substr(0, nl);
        s.failure_text += "      " + std::string(text) + "\n";
        rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
    }
}

} // namespace rift_test

int main(int argc, char** argv) {
    using namespace rift_test;
    bool                          verbose = false;
    bool                          list    = false;
    std::vector<std::string_view> filters;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        } else if (arg == "--list") {
            list = true;
        } else {
            filters.push_back(arg);
        }
    }

    auto selected = [&](std::string_view name) {
        if (filters.empty()) {
            return true;
        }
        for (std::string_view f : filters) {
            if (name.find(f) != std::string_view::npos) {
                return true;
            }
        }
        return false;
    };

    std::size_t passed = 0;
    std::size_t failed = 0;
    for (const TestCase& tc : registry()) {
        if (!selected(tc.name)) {
            continue;
        }
        if (list) {
            std::cout << tc.name << '\n';
            continue;
        }
        State& s = state();
        s.failures_in_case = 0;
        s.failure_text.clear();
        try {
            tc.fn();
        } catch (const AbortTest&) {
            // REQUIRE failed; already recorded.
        } catch (const std::exception& e) {
            record_failure(tc.file, tc.line, "test case", std::string("uncaught exception: ") + e.what());
        } catch (...) {
            record_failure(tc.file, tc.line, "test case", "uncaught unknown exception");
        }
        if (s.failures_in_case == 0) {
            ++passed;
            if (verbose) {
                std::cout << "PASS  " << tc.name << '\n';
            }
        } else {
            ++failed;
            std::cout << "FAIL  " << tc.name << "  (" << tc.file << ":" << tc.line << ")\n"
                      << s.failure_text;
        }
    }
    if (list) {
        return 0;
    }
    std::cout << "summary: " << passed << " passed, " << failed << " failed, "
              << state().checks << " checks\n";
    return failed == 0 ? 0 : 1;
}

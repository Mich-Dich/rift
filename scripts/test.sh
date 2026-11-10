#!/usr/bin/env bash
# Build the compiler (configuring on first use) and run the test suites.
#
#   scripts/test.sh                       build + run everything
#   scripts/test.sh --suite feature       one suite (unit | feature | action), repeatable
#   scripts/test.sh lexer                 only tests whose name/path contains "lexer"
#   scripts/test.sh --update              re-record action-test golden files
#   scripts/test.sh -v                    list every test, not only failures
#
# Environment:
#   BUILD_DIR   build directory          (default: build)
#   CXX         C++ compiler for a fresh configure (e.g. CXX=clang++)
#   BUILD_TYPE  CMake build type         (default: Debug)
#
# Exit status is 0 only if the build succeeds and every test passes.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
BUILD_DIR="${BUILD_DIR:-build}"

if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    generator=()
    if command -v ninja >/dev/null 2>&1; then
        generator=(-G Ninja)
    fi
    cmake -S . -B "$BUILD_DIR" "${generator[@]}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Debug}" >/dev/null
fi

if ! build_log="$(cmake --build "$BUILD_DIR" 2>&1)"; then
    echo "$build_log"
    echo "BUILD FAILED"
    exit 1
fi
# Surface compiler warnings even when the build succeeds: the compiler must stay warning-clean.
if grep -E "warning:" <<<"$build_log" >/dev/null; then
    grep -E -A3 "warning:" <<<"$build_log"
    echo "BUILD HAS WARNINGS"
fi

exec python3 tests/run_tests.py --build-dir "$BUILD_DIR" "$@"

cmake_minimum_required(VERSION 3.25)

project(rift
    VERSION 0.0.1
    DESCRIPTION "The Rift programming language compiler"
    LANGUAGES CXX
)

# Toolchain: C++26 -----------------------------------------------------------------------------------------------------
# Rift targets C++26 (-std=c++2c). We rely on Clang 18+ or GCC 14+.
# CMake 3.30+ recognizes CMAKE_CXX_STANDARD 26; older CMake needs
# the flag injected manually.
# Older CMake knows the flag for some compilers (Clang 18) but not others (GCC 13/14) and then
# fails at generate time, so with old CMake we skip CMAKE_CXX_STANDARD and inject the flag only.
set(CMAKE_CXX_EXTENSIONS                                                OFF)

if (CMAKE_VERSION VERSION_LESS                                          "3.30")
    if (CMAKE_CXX_COMPILER_ID MATCHES                                   "Clang|GNU")
        add_compile_options(-std=c++2c)
    endif()
else()
    set(CMAKE_CXX_STANDARD_REQUIRED                                     ON)
    set(CMAKE_CXX_STANDARD                                              26)
endif()

# Export compile_commands.json for clangd / editors
set(CMAKE_EXPORT_COMPILE_COMMANDS                                       ON)

# Build resource limits (RAM-friendly build) ---------------------------------------------------------------------------
# Ninja job pools cap concurrent link jobs independently of compile jobs Linking is by far the biggest RAM spike
# Compile jobs are already capped by CMAKE_BUILD_PARALLEL_LEVEL in the build preset
set_property(GLOBAL PROPERTY JOB_POOLS                                  link_pool=24)

# Warnings -------------------------------------------------------------------------------------------------------------
# The compiler itself should be warning-clean. These are strict on purpose - they catch the type-confusion and lifetime
# bugs that matter for a compiler
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
)

option(RIFT_WERROR "Treat compiler warnings as errors (used by CI)" OFF)
if (RIFT_WERROR)
    add_compile_options(-Werror)
endif()

# Subprojects ----------------------------------------------------------------------------------------------------------
add_subdirectory(compiler)

enable_testing()
add_subdirectory(tests)

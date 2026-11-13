# librift: the compiler as a library (everything except main). Unit tests link against it.
add_library(rift STATIC
    src/source.cpp
    src/diagnostics.cpp
    src/token.cpp
    src/lexer.cpp
    src/parser.cpp
    src/ast_printer.cpp
    src/ast_clone.cpp
    src/modules.cpp
    src/resolve.cpp
    src/types.cpp
    src/check.cpp
    src/compile_time.cpp
    src/ownership.cpp
    src/llvm_backend.cpp
    src/llvm_runtime.cpp
    src/driver.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/std_library.cpp
)
target_include_directories(rift PUBLIC include)

# The standard library's Rift source is compiled into riftc. CMake re-runs when it changes.
set(RIFT_STD_HEADER ${PROJECT_SOURCE_DIR}/library/std/std.rifth)
file(READ ${RIFT_STD_HEADER} RIFT_STD_HEADER_TEXT)
configure_file(src/std_library.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/std_library.cpp @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RIFT_STD_HEADER})
target_compile_definitions(rift PRIVATE RIFT_VERSION="${PROJECT_VERSION}")

# LLVM (the backend). Only the backend touches LLVM headers; the front end does not.
find_package(LLVM REQUIRED CONFIG)
if (NOT LLVM_VERSION_MAJOR EQUAL 18)
    message(FATAL_ERROR "riftc needs LLVM 18 (found ${LLVM_PACKAGE_VERSION} in ${LLVM_DIR})")
endif()
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")
llvm_map_components_to_libnames(RIFT_LLVM_LIBS
    core support irreader linker target analysis passes codegen native)
target_include_directories(rift SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})
target_link_libraries(rift PRIVATE ${RIFT_LLVM_LIBS})

# riftc: the command-line driver.
add_executable(riftc driver/main.cpp)
target_link_libraries(riftc PRIVATE rift)

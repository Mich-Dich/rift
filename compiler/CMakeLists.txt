# librift: the compiler as a library (everything except main). Unit tests link against it.
add_library(rift STATIC
    src/source.cpp
    src/diagnostics.cpp
    src/token.cpp
    src/lexer.cpp
    src/parser.cpp
    src/ast_printer.cpp
    src/modules.cpp
    src/resolve.cpp
    src/types.cpp
    src/check.cpp
    src/ownership.cpp
    src/llvm_backend.cpp
    src/llvm_runtime.cpp
    src/driver.cpp
)
target_include_directories(rift PUBLIC include)
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

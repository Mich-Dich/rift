# librift: the compiler as a library (everything except main). Unit tests link against it.
add_library(rift STATIC
    src/source.cpp
    src/diagnostics.cpp
    src/token.cpp
    src/lexer.cpp
    src/driver.cpp
)
target_include_directories(rift PUBLIC include)
target_compile_definitions(rift PRIVATE RIFT_VERSION="${PROJECT_VERSION}")

# riftc: the command-line driver.
add_executable(riftc driver/main.cpp)
target_link_libraries(riftc PRIVATE rift)

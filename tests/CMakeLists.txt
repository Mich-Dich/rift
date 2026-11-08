
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# C++ unit tests -------------------------------------------------------------------------------------------------------
add_executable(rift_lexer_tests
    lexer_tests.cpp
)
target_link_libraries(rift_lexer_tests PRIVATE librift)
target_include_directories(rift_lexer_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(
    NAME lexer_tests
    COMMAND rift_lexer_tests
)

add_executable(rift_parser_tests
    parser_tests.cpp
)
target_link_libraries(rift_parser_tests PRIVATE librift)
target_include_directories(rift_parser_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(
    NAME parser_tests
    COMMAND rift_parser_tests
)

# Python end-to-end tests ----------------------------------------------------------------------------------------------
add_test(
    NAME rift_tests
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/run_tests.py
        --riftc $<TARGET_FILE:riftc>
)

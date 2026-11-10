# --- unit tests (C++, in-process) -------------------------------------------------------------
# Every tests/unit/test_*.cpp is picked up automatically.
file(GLOB RIFT_UNIT_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_*.cpp)

add_executable(rift_unit_tests
    unit/framework/rift_test_main.cpp
    ${RIFT_UNIT_TEST_SOURCES}
)
target_include_directories(rift_unit_tests PRIVATE unit/framework)
target_link_libraries(rift_unit_tests PRIVATE rift)

add_test(NAME unit COMMAND rift_unit_tests)

# --- feature + action tests (Python runner driving riftc) --------------------------------------
find_package(Python3 3.10 COMPONENTS Interpreter)
if (Python3_FOUND)
    foreach (suite feature action)
        add_test(
            NAME ${suite}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_tests.py
                    --suite ${suite} --color never
                    --riftc $<TARGET_FILE:riftc>
        )
    endforeach()
else()
    message(WARNING "Python 3.10+ not found: feature and action tests are not registered with ctest")
endif()

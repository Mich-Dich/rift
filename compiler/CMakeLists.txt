
# librift: the frontend library ----------------------------------------------------------------------------------------

set(RIFT_LIB_NAME                                                       librift)
add_library(${RIFT_LIB_NAME}
    src/version.cpp
    src/source.cpp
    src/diagnostic.cpp
    src/diagnostic_engine.cpp
    src/token.cpp
    src/lexer.cpp
)
set_target_properties(${RIFT_LIB_NAME}                                  PROPERTIES
    BUILD_RPATH                                                         "/opt/gcc-16.2/lib64"
    INSTALL_RPATH                                                       "/opt/gcc-16.2/lib64"
    RUNTIME_OUTPUT_DIRECTORY                                            "${COMPILE_OUTPUT_DIR}"
    CXX_VISIBILITY_PRESET                                               default
    VISIBILITY_INLINES_HIDDEN                                           OFF
    JOB_POOL_LINK                                                       link_pool
)
# PUBLIC (not INTERFACE) so librift's own .cpp files also get these flags.
target_compile_options(${RIFT_LIB_NAME}                                 PUBLIC
    # $<$<COMPILE_LANGUAGE:CXX>:-freflection>
    $<$<COMPILE_LANGUAGE:CXX>:-ftemplate-depth=512>
    $<$<COMPILE_LANGUAGE:CXX>:-fdiagnostics-color=always>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Debug>>:-g3>
)
target_include_directories(${RIFT_LIB_NAME}                             PUBLIC include)



# riftc: the driver binary ---------------------------------------------------------------------------------------------

set(RIFT_BIN_NAME                                                       riftc)
add_executable(${RIFT_BIN_NAME}
    src/main.cpp
    src/driver.cpp
)
set_target_properties(${RIFT_BIN_NAME}                                  PROPERTIES
    BUILD_RPATH                                                         "/opt/gcc-16.2/lib64"
    INSTALL_RPATH                                                       "/opt/gcc-16.2/lib64"
    RUNTIME_OUTPUT_DIRECTORY                                            "${COMPILE_OUTPUT_DIR}"
    CXX_VISIBILITY_PRESET                                               default
    VISIBILITY_INLINES_HIDDEN                                           OFF
    JOB_POOL_LINK                                                       link_pool
)
target_link_libraries(${RIFT_BIN_NAME}                                  PRIVATE ${RIFT_LIB_NAME})

add_custom_target(${RIFT_BIN_NAME}_run
    COMMAND $<TARGET_FILE:${RIFT_BIN_NAME}> --version
    DEPENDS ${RIFT_BIN_NAME}
    COMMENT "Running ${RIFT_BIN_NAME} --version"
    VERBATIM
)

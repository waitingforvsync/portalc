# ---- the Beeb port's host tool (PLAN.md 3.4, 5.4) ----
#
# beebtool is the whole engine in one translation unit (it includes every
# engine .c itself), linked only against richc.

add_executable(portalc_beebtool beebtool.c)
target_include_directories(portalc_beebtool PRIVATE ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/test)
target_link_libraries(portalc_beebtool PRIVATE richc m)
if(NOT MSVC)
    target_compile_options(portalc_beebtool PRIVATE -Wall -Wextra)
endif()

# the coverage gate (PLAN.md 5.5): the same tool with clang's source-based
# coverage, run over the golden streams by beeb/host/coverage.sh
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    add_executable(portalc_beebtool_cov beebtool.c)
    target_include_directories(portalc_beebtool_cov PRIVATE ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/src/test)
    target_link_libraries(portalc_beebtool_cov PRIVATE richc m)
    target_compile_options(portalc_beebtool_cov PRIVATE -fprofile-instr-generate -fcoverage-mapping)
    target_link_options(portalc_beebtool_cov PRIVATE -fprofile-instr-generate)
endif()

# the pose search: the same tool with trace-pc-guard, whose callbacks
# (in beebtool.c) flag a frame that reaches a new basic block
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    add_executable(portalc_beebtool_search beebtool.c)
    target_include_directories(portalc_beebtool_search PRIVATE ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/src/test)
    target_link_libraries(portalc_beebtool_search PRIVATE richc m)
    target_compile_definitions(portalc_beebtool_search PRIVATE BEEBTOOL_SEARCH)
    target_compile_options(portalc_beebtool_search PRIVATE -O2 -g -fno-inline
        -fsanitize-coverage=trace-pc-guard)
    target_link_options(portalc_beebtool_search PRIVATE -no-pie)
endif()

# the traced build (PLAN.md D1c, D1d, 5.6): host/trace.py writes copies of
# render.c and arith.c with the unit-replay wrappers and the cost counters in,
# and beebtool includes them instead of the originals, with host/trace_hooks.c
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(TRACE_DIR ${CMAKE_CURRENT_BINARY_DIR}/trace)
    set(TRACE_OUT ${TRACE_DIR}/render_traced.c ${TRACE_DIR}/arith_traced.c ${TRACE_DIR}/trace_counters.h)
    add_custom_command(OUTPUT ${TRACE_OUT}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/trace.py ${PROJECT_SOURCE_DIR}/src ${TRACE_DIR}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/trace.py ${PROJECT_SOURCE_DIR}/src/render.c ${PROJECT_SOURCE_DIR}/src/arith.c
        COMMENT "Generating the traced render.c and arith.c")
    set_source_files_properties(${TRACE_OUT} PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE)
    add_executable(portalc_beebtool_trace beebtool.c ${TRACE_OUT} trace_hooks.c)
    set_source_files_properties(trace_hooks.c PROPERTIES HEADER_FILE_ONLY TRUE)
    target_include_directories(portalc_beebtool_trace PRIVATE ${TRACE_DIR} ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/src/test)
    target_link_libraries(portalc_beebtool_trace PRIVATE richc m)
    target_compile_definitions(portalc_beebtool_trace PRIVATE BEEBTOOL_TRACE)
    target_compile_options(portalc_beebtool_trace PRIVATE -O2 -fno-inline -fsanitize-coverage=trace-pc-guard)
    target_link_options(portalc_beebtool_trace PRIVATE -no-pie)
endif()

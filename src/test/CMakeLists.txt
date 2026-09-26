# ---- portal engine tests ----

add_executable(portalc_test
    main.c
    test_arith.c
    test_map.c
    test_mode5.c
    test_palette.c
    test_render.c
    probes/probe_collision.c
    probes/probe_fan.c
)

target_link_libraries(portalc_test PRIVATE portal)

if(MSVC)
    target_compile_options(portalc_test PRIVATE /W3)
else()
    target_compile_options(portalc_test PRIVATE -Wall -Wextra)
endif()

# run the tests as a post-build step, richc style
add_custom_command(
    TARGET portalc_test POST_BUILD
    COMMAND $<TARGET_FILE:portalc_test>
    WORKING_DIRECTORY $<TARGET_FILE_DIR:portalc_test>
    COMMENT "Running portalc tests"
    VERBATIM
)

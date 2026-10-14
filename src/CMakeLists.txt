add_subdirectory(richc)

# ---- portal engine library (pure C, links only richc core) ----

set(PORTAL_SOURCES
    arith.c
    camera.c
    map.c
    mode5.c
    palette.c
    plot.c
    render.c
    sprite.c
    vcache.c
    arith.h
    camera.h
    map.h
    mode5.h
    palette.h
    plot.h
    render.h
    sprite.h
    vcache.h
)

# portal_depth3 is the same with the walk's depth capped at 3, for the walk's check (walk3ref)
foreach(lib portal portal_depth3)
    add_library(${lib} STATIC ${PORTAL_SOURCES})
    target_link_libraries(${lib} PUBLIC richc)
    target_include_directories(${lib} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    if(MSVC)
        target_compile_options(${lib} PRIVATE /W3)
    else()
        # -Wconversion: every narrowing written out, in the type the 6502 holds
        target_compile_options(${lib} PRIVATE -Wall -Wextra -Wconversion)
    endif()
endforeach()
target_compile_definitions(portal_depth3 PUBLIC PT_MAX_DEPTH=3)

# ---- windowed app ----

add_executable(portalc main.c)
target_link_libraries(portalc PRIVATE portal richc_app)
if(NOT MSVC)
    target_compile_options(portalc PRIVATE -Wall -Wextra)
endif()

# ---- offline frame dumper (renders PPM frames, no window needed) ----

add_executable(portalc_dump tools/dump.c)
target_link_libraries(portalc_dump PRIVATE portal)
if(NOT MSVC)
    target_compile_options(portalc_dump PRIVATE -Wall -Wextra)
endif()

# ---- verification tools: byte-exact sweep dumps, the map dump, and the
# comparison against a floating-point ray cast ----

foreach(tool sweep fb2ppm mapdump refcmp tables mapasm arithref vcacheref captureref plotref clipref
             walkref)
    add_executable(portalc_${tool} tools/${tool}.c)
    target_link_libraries(portalc_${tool} PRIVATE portal)
    if(NOT MSVC)
        target_compile_options(portalc_${tool} PRIVATE -Wall -Wextra)
    endif()
endforeach()
if(NOT MSVC)
    target_link_libraries(portalc_refcmp PRIVATE m)
endif()
add_executable(portalc_walk3ref tools/walkref.c)
target_link_libraries(portalc_walk3ref PRIVATE portal_depth3)
target_compile_definitions(portalc_walk3ref PRIVATE WALK_STRESS)
if(NOT MSVC)
    target_compile_options(portalc_walk3ref PRIVATE -Wall -Wextra)
endif()

# ---- tests ----

add_subdirectory(test)

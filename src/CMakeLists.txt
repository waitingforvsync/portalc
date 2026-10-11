add_subdirectory(richc)

# ---- portal engine library (pure C, links only richc core) ----

add_library(portal STATIC
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

target_link_libraries(portal PUBLIC richc)
target_include_directories(portal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(portal PRIVATE /W3)
else()
    # -Wconversion: every narrowing written out, in the type the 6502 holds
    target_compile_options(portal PRIVATE -Wall -Wextra -Wconversion)
endif()

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

foreach(tool sweep fb2ppm mapdump refcmp tables mapasm arithref)
    add_executable(portalc_${tool} tools/${tool}.c)
    target_link_libraries(portalc_${tool} PRIVATE portal)
    if(NOT MSVC)
        target_compile_options(portalc_${tool} PRIVATE -Wall -Wextra)
    endif()
endforeach()
if(NOT MSVC)
    target_link_libraries(portalc_refcmp PRIVATE m)
endif()

# ---- tests ----

add_subdirectory(test)

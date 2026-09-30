add_subdirectory(richc)

# ---- portal engine library (pure C, links only richc core) ----

add_library(portal STATIC
    arith.c
    camera.c
    map.c
    mode5.c
    palette.c
    render.c
    sprite.c
    arith.h
    camera.h
    map.h
    mode5.h
    palette.h
    render.h
    sprite.h
)

target_link_libraries(portal PUBLIC richc)
target_include_directories(portal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(portal PRIVATE /W3)
else()
    target_compile_options(portal PRIVATE -Wall -Wextra)
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

# ---- verification tools: byte-exact sweep dumps and the map dump ----

foreach(tool sweep fb2ppm mapdump)
    add_executable(portalc_${tool} tools/${tool}.c)
    target_link_libraries(portalc_${tool} PRIVATE portal)
    if(NOT MSVC)
        target_compile_options(portalc_${tool} PRIVATE -Wall -Wextra)
    endif()
endforeach()

# ---- tests ----

add_subdirectory(test)

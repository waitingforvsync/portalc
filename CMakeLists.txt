cmake_minimum_required(VERSION 3.21)

# clang unless told otherwise (CC or -DCMAKE_C_COMPILER): the Beeb port's
# coverage, search and traced host tools need it
if(NOT DEFINED CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
    find_program(PORTALC_CLANG clang)
    if(PORTALC_CLANG)
        set(CMAKE_C_COMPILER ${PORTALC_CLANG})
    endif()
endif()

project(portalc C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# richc's own tests still build, but only portalc's tests run automatically
set(RICHC_RUN_TESTS OFF CACHE BOOL "" FORCE)

add_subdirectory(src)
add_subdirectory(beeb/host)

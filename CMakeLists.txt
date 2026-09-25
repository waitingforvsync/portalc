cmake_minimum_required(VERSION 3.21)
project(portalc C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# richc's own tests still build, but only portalc's tests run automatically
set(RICHC_RUN_TESTS OFF CACHE BOOL "" FORCE)

add_subdirectory(src)

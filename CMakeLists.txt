# stl-grid — static grid viewer for all .stl files in a directory.
# Standalone GTK3 tool (no Qt, no OpenGL):
#   cmake -S stl-grid -B stl-grid/build && cmake --build stl-grid/build
cmake_minimum_required(VERSION 3.10)
project(stl-grid C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(STL_GRID_DEBUG "Build with debug flags (-g -O0 -DDEBUG)" OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)

add_executable(stl-grid main.c)
target_include_directories(stl-grid PRIVATE ${GTK3_INCLUDE_DIRS})
target_link_libraries(stl-grid ${GTK3_LIBRARIES} m)
if(STL_GRID_DEBUG)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O0 -DDEBUG")
else()
  target_compile_options(stl-grid PRIVATE -O2 -Wall)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})



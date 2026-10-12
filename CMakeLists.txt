# stlpreview -- static grid viewer for the 3D models in a directory.
#
#   cmake -S . -B build && cmake --build build     # configure and build
#   ctest --test-dir build --output-on-failure     # run the tests
#
# A standalone GTK3 tool: no Qt, no OpenGL, no mesh library. The only
# external dependencies are GTK3 (the UI) and zlib (inflating the ZIP
# container a 3MF file is).
cmake_minimum_required(VERSION 3.10)
project(stlpreview
  VERSION 1.0
  LANGUAGES C
  DESCRIPTION "Thumbnail grid viewer for STL and 3MF models")

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
# POSIX declarations the code relies on (strncasecmp, S_IRUSR, ...) come from
# the C library's default extensions, so build with -std=gnu99, not -std=c99.
set(CMAKE_C_EXTENSIONS ON)

# Optimisation and debug info come from CMAKE_BUILD_TYPE (-O3 -DNDEBUG for
# Release, -g for Debug) rather than being hardcoded here, so a packager can
# override them the usual way.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(STLPREVIEW_WERROR "Treat compiler warnings as errors (used by CI)" OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
find_package(ZLIB REQUIRED)

# One translation unit per concern; stlview.h documents the module layout.
# model.c, render.c and cache.c deliberately do not include GTK, so the
# headless PNG export and the tests can use them with no toolkit involved.
add_executable(stlpreview
  main.c
  model.c
  render.c
  cache.c
  ui.c
)
target_include_directories(stlpreview PRIVATE ${GTK3_INCLUDE_DIRS})
target_link_libraries(stlpreview PRIVATE ${GTK3_LIBRARIES} m ZLIB::ZLIB)

# The tree builds clean with these; -Wconversion is deliberately not enabled
# because the rasteriser does a lot of intentional float/int narrowing.
target_compile_options(stlpreview PRIVATE -Wall -Wextra -Wshadow)
if(STLPREVIEW_WERROR)
  target_compile_options(stlpreview PRIVATE -Werror)
endif()

enable_testing()
add_subdirectory(tests)

install(TARGETS stlpreview RUNTIME DESTINATION bin)

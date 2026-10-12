# Unit tests for stlpreview.
#
# Each test #includes the .c file under test directly, so it can exercise the
# static helpers too; that is why none of them link objects from the
# stlpreview target. All three avoid GTK entirely -- model.c, render.c and
# cache.c do not include it, which is exactly the separation the module split
# was meant to make testable.

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED glib-2.0)
pkg_check_modules(CAIRO REQUIRED cairo)

# Common include paths, link libraries and warning flags for every test.
add_library(stlpreview_test_support INTERFACE)
target_include_directories(stlpreview_test_support INTERFACE
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${GLIB_INCLUDE_DIRS}
  ${CAIRO_INCLUDE_DIRS}
)
target_link_libraries(stlpreview_test_support INTERFACE
  ${GLIB_LIBRARIES}
  ${CAIRO_LIBRARIES}
  ZLIB::ZLIB
  m
)
target_compile_options(stlpreview_test_support INTERFACE -Wall -Wextra)

foreach(test_name model render cache)
  add_executable(test_${test_name} test_${test_name}.c)
  target_link_libraries(test_${test_name} PRIVATE stlpreview_test_support)
  # Run each test in its own temporary XDG cache so it cannot touch, or be
  # disturbed by, the developer's real ~/.cache/stlpreview.
  add_test(NAME ${test_name} COMMAND test_${test_name})
endforeach()

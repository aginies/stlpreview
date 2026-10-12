#!/usr/bin/env bash
# stlpreview — build script
#
# Usage:
#   ./build.sh            → release build (release/)
#   ./build.sh debug      → debug build   (debug/)
#   ./build.sh test       → debug build + run the unit tests
#   ./build.sh clean      → remove every build directory
#
# Extra CMake arguments are passed through, e.g.
#   ./build.sh release -DSTLPREVIEW_WERROR=ON

set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"

build() {
  local out="$1"
  shift
  echo "→ Building $out ..."
  # Drop a stale CMake cache, e.g. after the directory was renamed.
  rm -rf "$DIR/$out/CMakeCache.txt" "$DIR/$out/CMakeFiles"
  cmake -S "$DIR" -B "$DIR/$out" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "$@"
  cmake --build "$DIR/$out" --parallel "$(nproc)"
  # Let LSP tools (clangd) find the pkg-config include paths.
  ln -sf "$DIR/$out/compile_commands.json" "$DIR/compile_commands.json"
  test -x "$DIR/$out/stlpreview"
  echo "  ✓ $DIR/$out/stlpreview"
}

case "${1:-release}" in
debug)
  build debug -DCMAKE_BUILD_TYPE=Debug "${@:2}"
  ;;
release)
  build release -DCMAKE_BUILD_TYPE=Release "${@:2}"
  ;;
test)
  # Warnings are errors here: a test build is the one that CI runs, so a new
  # warning should stop it rather than scroll past.
  build debug -DCMAKE_BUILD_TYPE=Debug -DSTLPREVIEW_WERROR=ON "${@:2}"
  echo "→ Running unit tests ..."
  # `cd` rather than `ctest --test-dir`, which needs CMake >= 3.20 while this
  # project still supports 3.10.
  (cd "$DIR/debug" && ctest --output-on-failure)
  ;;
clean)
  echo "→ Cleaning ..."
  rm -rf "$DIR/debug" "$DIR/release" "$DIR/build"
  # The symlink build() drops in the source tree would otherwise dangle.
  rm -f "$DIR/compile_commands.json"
  echo "  ✓ done"
  ;;
*)
  echo "Usage: $0 [release|debug|test|clean]"
  exit 1
  ;;
esac

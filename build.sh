#!/usr/bin/env bash
# stlpreview — build script
# Usage:
#   ./build.sh          → release (release/)
#   ./build.sh debug    → debug   (debug/)
#   ./build.sh clean    → remove all build dirs

set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"

build() {
  local out="$1"
  shift
  echo "→ Building $out ..."
  # Clean stale CMake cache (e.g. after directory rename)
  rm -rf "$DIR/$out/CMakeCache.txt" "$DIR/$out/CMakeFiles"
  cmake -S "$DIR" -B "$DIR/$out" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "$@"
  cmake --build "$DIR/$out" --parallel "$(nproc)"
  # Let LSP tools (clangd) find the pkg-config include paths
  ln -sf "$DIR/$out/compile_commands.json" "$DIR/compile_commands.json"
  test -x "$DIR/$out/stlpreview"
  echo "  ✓ $DIR/$out/stlpreview"
}

case "${1:-release}" in
debug)
  build debug -DSTL_GRID_DEBUG=ON -DCMAKE_BUILD_TYPE=Debug "${@:2}"
  ;;
release)
  build release -DCMAKE_BUILD_TYPE=Release "${@:2}"
  ;;
clean)
  echo "→ Cleaning ..."
  rm -rf "$DIR/debug" "$DIR/release"
  echo "  ✓ done"
  ;;
*)
  echo "Usage: $0 [release|debug|clean]"
  exit 1
  ;;
esac

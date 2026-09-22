#!/usr/bin/env bash
# sliceview — build script
# Usage:
#   ./build.sh          → release (build/)
#   ./build.sh debug    → debug   (build-debug/)
#   ./build.sh clean    → remove all build dirs

set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"

build() {
  local out="$1"
  shift
  echo "→ Building $out ..."
  # Clean stale CMake cache (e.g. after directory rename)
  rm -rf "$DIR/$out/CMakeCache.txt" "$DIR/$out/CMakeFiles"
  cmake -S "$DIR" -B "$DIR/$out" "$@"
  cmake --build "$DIR/$out"
  echo "  ✓ $DIR/$out/sliceview"
}

case "${1:-release}" in
debug)
  build debug -DSTL_GRID_DEBUG=ON -DCMAKE_BUILD_TYPE=Debug
  ;;
release)
  build release
  ;;
clean)
  echo "→ Cleaning ..."
  rm -rf "$DIR/build" "$DIR/build-release" "$DIR/build-debug"
  echo "  ✓ done"
  ;;
*)
  echo "Usage: $0 [release|debug|clean]"
  exit 1
  ;;
esac

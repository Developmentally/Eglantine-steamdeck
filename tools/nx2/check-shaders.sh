#!/usr/bin/env bash
# Build nx2-shader-check and run it over a corpus of dumped native shader programs
# (EDEN_NX2_DUMP_DIR, a .bin + .meta pair per program), so translator changes are validated
# against every program a title has ever used without launching it. Exits non-zero on failures
# (or, with a baseline, on new ones).
#
#   tools/nx2/check-shaders.sh [-b baseline] [-l] [-q] [corpus-dir | title]...
#
# Defaults: corpus $NX2_SHADERS (~/nx2-shaders), build dir $BUILD_DIR (build). A bare title
# name selects $NX2_SHADERS/<title>. Configure the build dir once with -DENABLE_DEBUG_TOOLS=ON.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
NX2_SHADERS="${NX2_SHADERS:-$HOME/nx2-shaders}"
CMAKE="${CMAKE:-$(sed -n 's/^CMAKE_COMMAND:INTERNAL=//p' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null || echo cmake)}"

opts=(); inputs=()
while [ $# -gt 0 ]; do
  case "$1" in
    -b) opts+=(-b "$2"); shift 2;;
    -l|-q) opts+=("$1"); shift;;
    *) if [ -e "$1" ]; then inputs+=("$1"); else inputs+=("$NX2_SHADERS/$1"); fi; shift;;
  esac
done
[ ${#inputs[@]} -gt 0 ] || inputs=("$NX2_SHADERS")

rg -q '^ENABLE_DEBUG_TOOLS:BOOL=ON' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null ||
  "$CMAKE" -S "$ROOT" -B "$BUILD_DIR" -DENABLE_DEBUG_TOOLS=ON >/dev/null
"$CMAKE" --build "$BUILD_DIR" --target nx2-shader-check 2>&1 | rg -v 'Wno-error-all|^\s*$' | rg 'error|FAILED' -A3 || true
exec "$BUILD_DIR/bin/nx2-shader-check" "${opts[@]}" "${inputs[@]}"

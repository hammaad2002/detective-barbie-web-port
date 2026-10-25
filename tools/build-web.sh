#!/bin/bash
# Build the web runtimes and stage the deployable player engine for the shell.
#   tools/build-web.sh          (run from anywhere)
# - work/build-web-W/detective_player.{js,wasm}: runtime without game code -> web/public/engine/
# - work/build-web-W/detectivebarbie.{js,wasm}: development engine with C-backend game code (stays in work/)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${DETECTIVE_BUILD_WEB:-$ROOT/work/build-web-W}"
EMSDK="${DETECTIVE_EMSDK:-$ROOT/work/tools/emsdk}"
[ -f "$EMSDK/emsdk_env.sh" ] || { echo "Emscripten not found: set DETECTIVE_EMSDK or install it at $EMSDK"; exit 1; }
source "$EMSDK/emsdk_env.sh" > /dev/null 2>&1
export EMCC_CFLAGS="${EMCC_CFLAGS:--O3 -msimd128}"
export EMCC_CPPFLAGS="${EMCC_CPPFLAGS:--O3 -msimd128}"
# Player build links web/runtime/web_game.cpp; keep it in sync with runtime/src/web/web_game.cpp.
mkdir -p "$ROOT/web/runtime"
sed 's|#include "../core/rt.h"|#include "core/rt.h"|' "$ROOT/runtime/src/web/web_game.cpp" > "$ROOT/web/runtime/web_game.cpp"
emcmake cmake -S "$ROOT/runtime" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DGEN_OPT=-O3 > /dev/null
ninja -C "$BUILD" detective_player ${WITH_DEV_ENGINE:+detectivebarbie}
mkdir -p "$ROOT/web/public/engine"
cp "$BUILD/detective_player.js" "$BUILD/detective_player.wasm" "$ROOT/web/public/engine/"
WASM="$ROOT/web/public/engine/detective_player.wasm"
if command -v wasm-opt >/dev/null 2>&1; then
  wasm-opt -O3 --simd128 "$WASM" -o "$WASM.opt" && mv "$WASM.opt" "$WASM"
fi
echo "player engine staged in web/public/engine"

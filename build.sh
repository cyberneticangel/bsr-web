#!/bin/sh
# Builds web/bsr.js + web/bsr.wasm. Requires Emscripten (emcc).
set -e
cd "$(dirname "$0")"
em++ -O2 -std=c++17 -Wall -Wno-unused-function \
  src/main.cpp src/game.cpp src/formats.cpp src/render.cpp src/physics.cpp src/race.cpp \
  -sUSE_WEBGL2=1 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 \
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=128MB -sFORCE_FILESYSTEM=1 \
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,FS,UTF8ToString \
  -sEXPORTED_FUNCTIONS=_main,_malloc,_free \
  -o web/bsr.js
echo "built web/bsr.js web/bsr.wasm"

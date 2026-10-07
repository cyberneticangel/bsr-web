#!/bin/sh
# Native headless test harness (Linux, Mesa EGL).
set -e
cd "$(dirname "$0")"
g++ -O2 -g -std=c++17 -Wall -o harness harness.cpp ../../src/game.cpp ../../src/formats.cpp ../../src/render.cpp \
  ../../src/physics.cpp ../../src/race.cpp -lEGL -lGLESv2 -lz

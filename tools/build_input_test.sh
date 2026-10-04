#!/bin/sh
# Build and run the unified-input harness (tools/input_test.cpp).  It runs
# headless (SDL_VIDEODRIVER=dummy set by the harness itself) and drives the real
# filter path with synthetic SDL axis/button values, so it works on a machine
# with no controller attached.
#
#   cmake --build build -j          # the game itself must still compile
#   tools/build_input_test.sh [dataDir]
#
# The harness links only src/app/Input.cpp (plus SDL2), so it does not depend on
# the rest of the game compiling (useful while other modules are being edited).
# See docs/input.md.
set -e
cd "$(dirname "$0")/.."
mkdir -p tools/out

echo "== building tools/out/input_test"
c++ -std=c++17 -O2 -Wall -Wextra -I src -o tools/out/input_test \
    tools/input_test.cpp src/app/Input.cpp \
    $(pkg-config --cflags --libs sdl2)

echo "== tools/input_test (headless, no pad required)"
SDL_VIDEODRIVER=dummy ./tools/out/input_test "${1:-original}"

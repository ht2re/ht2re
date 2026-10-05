#!/usr/bin/env bash
# Build and run the Economy harness (tools/economy_selftest.cpp) and then the
# independent Python reproduction (tools/economy_check.py).
#
#   cmake --build build -j          # the game itself must still compile
#   tools/build_economy_check.sh    # this script: C++ self-test + Python check
#
# The harness links only GameData/Sexp/Sdte/Way/Economy, so it does not depend on
# the rest of the game compiling (useful while other modules are being edited).
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p tools/out

echo "== building tools/out/economy_selftest"
c++ -std=c++17 -O1 -I src -o tools/out/economy_selftest \
    tools/economy_selftest.cpp \
    src/game/Economy.cpp src/script/Sexp.cpp src/script/Sdte.cpp \
    src/script/GameData.cpp src/world/Way.cpp

echo "== tools/economy_selftest (game module, shipped scripts + ENV/*.way)"
./tools/out/economy_selftest original | grep -v '^\[ht2re\] way:'

echo "== python3 tools/economy_check.py (independent reproduction)"
python3 tools/economy_check.py

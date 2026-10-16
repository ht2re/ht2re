#!/bin/sh
# Build and run the script-rules harness (tools/script_selftest.cpp).
#
#   cmake --build build -j          # the game itself must still compile
#   tools/build_script_check.sh     # this script: the `--script-report` producer
#
# The harness links only GameData/Sexp/Sdte/ScriptRules/Way, so it does not
# depend on the rest of the game compiling (useful while other modules are
# being edited).
set -e
cd "$(dirname "$0")/.."
mkdir -p tools/out

c++ -std=c++17 -O1 -I src -o tools/out/script_rng_test tools/script_rng_test.cpp
./tools/out/script_rng_test

c++ -std=c++17 -O1 -I src -o tools/out/load_guilt_test tools/load_guilt_test.cpp
./tools/out/load_guilt_test --self-test

echo "== building tools/out/script_selftest"
c++ -std=c++17 -O1 -I src -o tools/out/script_selftest \
    tools/script_selftest.cpp \
    src/script/ScriptRules.cpp src/script/Sexp.cpp src/script/Sdte.cpp \
    src/script/GameData.cpp src/world/Way.cpp

echo "== tools/out/script_selftest (game scripts + recovered rules)"
./tools/out/script_selftest "${1:-original}"
echo "== building tools/out/script_runtime_test"
c++ -std=c++17 -O1 -I src -o tools/out/script_runtime_test \
    tools/script_runtime_test.cpp \
    src/script/ScriptRules.cpp src/script/Sexp.cpp src/script/Sdte.cpp \
    src/script/GameData.cpp src/world/Way.cpp
./tools/out/script_runtime_test "${1:-original}"

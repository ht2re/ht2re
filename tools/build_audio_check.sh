#!/usr/bin/env bash
# Build and run the audio harness (tools/audio_test.cpp) plus the shipped-data
# probe (tools/audio_probe.py).
#
#   cmake --build build -j          # the game itself must still compile
#   tools/build_audio_check.sh      # this script: C++ harness + Python probe
#
# The harness links only Audio.cpp + Res.cpp - it does not need the rest of the
# game to compile (useful while other modules are being edited) - and forces
# SDL_AUDIODRIVER=dummy, so it runs headless.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p tools/out

echo "== building tools/out/audio_test"
c++ -std=c++17 -O1 -I src -DHT2_HAVE_SDL_MIXER=1 -o tools/out/audio_test \
    tools/audio_test.cpp src/audio/Audio.cpp src/res/Res.cpp \
    $(pkg-config --cflags --libs sdl2 sdl2_mixer)

echo "== tools/out/audio_test original (SDL_AUDIODRIVER=dummy)"
./tools/out/audio_test original | tee tools/out/audio_test.log
if grep -q '^FAIL' tools/out/audio_test.log; then exit 1; fi

n=$(grep -c "unknown sound 'definitely_not_a_sound'" tools/out/audio_test.log || true)
echo "== missing-sound log lines: $n (expected 1: play() x2 + playGlobal() x1, logged once)"
[ "$n" = "1" ] || { echo "FAIL: missing sound logged $n times"; exit 1; }

echo "== python3 tools/audio_probe.py"
python3 tools/audio_probe.py

echo "== all audio checks passed"

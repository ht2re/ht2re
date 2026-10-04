#!/usr/bin/env bash
# Build and run tools/render_bench.cpp against the engine's own object files.
#
#   tools/bench.sh [frames] [data-dir]
#
# Reuses build/CMakeFiles/ht2re.dir/*.o (everything except main.cpp/App.cpp) so
# the benchmark renders through exactly the code the game ships, then reports
# ms/frame for HT2_ANISO=1 (isotropic trilinear) and HT2_ANISO=8.
set -e
cd "$(dirname "$0")/.."
FRAMES=${1:-120}
DATA=${2:-original}
OBJDIR=build/CMakeFiles/ht2re.dir

[ -d "$OBJDIR" ] || { echo "build the engine first (cmake --build build -j)"; exit 1; }
OBJS=$(find "$OBJDIR" -name '*.o' | grep -v -e '/main\.cpp\.o' -e '/App\.cpp\.o' | tr '\n' ' ')

MIXER_LIB=$(ls -d /opt/homebrew/Cellar/sdl2_mixer/*/lib 2>/dev/null | tail -1)
clang++ -std=c++17 -O2 -Isrc -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 \
    tools/render_bench.cpp $OBJS \
    -L/opt/homebrew/lib ${MIXER_LIB:+-L$MIXER_LIB} \
    -Wl,-rpath,/opt/homebrew/lib ${MIXER_LIB:+-Wl,-rpath,$MIXER_LIB} \
    -lSDL2 -lSDL2_mixer -framework OpenGL -o build/render_bench

for A in 1 8; do
    HT2_ANISO=$A ./build/render_bench "$FRAMES" "$DATA" 2>/dev/null | grep '^BENCH'
done

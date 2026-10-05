#!/usr/bin/env bash
# Build and run tools/mirror_check.cpp (the cockpit + rear-view mirror harness)
# and check the mirror pixels.  See docs/mirror.md.
#
#   tools/mirror_check.sh [frames] [data-dir]
#
# It links the engine's own object files (build/CMakeFiles/ht2re.dir/*.o, minus
# main.cpp/App.cpp, which belong to the application shell) so the mirror renders
# through exactly the code the game ships.  tools/mirror_check.py reads the four
# BMPs the harness writes and asserts the mirror region behaviour; the harness
# prints the cost numbers.
set -euo pipefail
cd "$(dirname "$0")/.."
FRAMES=${1:-60}
DATA=${2:-original}
OBJDIR=build/CMakeFiles/ht2re.dir
BUILD=build/mirror_check

[ -d "$OBJDIR" ] || { echo "build the engine first (cmake --build build -j)"; exit 1; }
mkdir -p "$BUILD" shots

# Renderer/Mirror must be current: the CMake tree may be mid-edit elsewhere
# (another change owns App.cpp), so compile them here if their objects are
# older than the sources.
compile_if_stale() {
    local src="$1" obj="$OBJDIR/$1.o"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
        mkdir -p "$(dirname "$obj")"
        clang++ -std=c++17 -O2 -Isrc -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 \
            -c "$src" -o "$obj"
    fi
}
compile_if_stale src/gfx/Renderer.cpp
compile_if_stale src/gfx/Mirror.cpp

OBJS=$(find "$OBJDIR" -name '*.o' | grep -v -e '/main\.cpp\.o' -e '/App\.cpp\.o' | tr '\n' ' ')

MIXER_LIB=$(ls -d /opt/homebrew/Cellar/sdl2_mixer/*/lib 2>/dev/null | tail -1)
clang++ -std=c++17 -O2 -Isrc -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 \
    tools/mirror_check.cpp $OBJS \
    -L/opt/homebrew/lib ${MIXER_LIB:+-L$MIXER_LIB} \
    -Wl,-rpath,/opt/homebrew/lib ${MIXER_LIB:+-Wl,-rpath,$MIXER_LIB} \
    -lSDL2 -lSDL2_mixer -framework OpenGL -o "$BUILD/mirror_check"

"$BUILD/mirror_check" --frames "$FRAMES" --data "$DATA" --out shots/mirror \
    ${MIRROR_POS:+--pos "$MIRROR_POS"} ${MIRROR_SIZE:+--size "$MIRROR_SIZE"} \
    | tee "${MIRROR_LOG:-shots/mirror.log}"
python3 tools/mirror_check.py "${MIRROR_LOG:-shots/mirror.log}"

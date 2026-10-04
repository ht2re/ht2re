#!/bin/sh
# Build and run the movie/loading-screen harness (tools/video_check.cpp).
#
#   tools/make_video.sh          # first: produce build/media/*.rvid + .wav
#   tools/build_video_check.sh   # then this: off-screen render + API checks
#
# The harness links Renderer/Font/Video/Loading plus the world modules Renderer
# references, so it does not depend on the whole game compiling.  It writes
# screenshots to tools/out/*.bmp and exits non-zero when a check fails.
set -e
cd "$(dirname "$0")/.."
mkdir -p tools/out

SDL_CFLAGS=$(pkg-config --cflags sdl2)
SDL_LIBS=$(pkg-config --libs sdl2)
case "$(uname -s)" in
    Darwin) GL_LIBS="-framework OpenGL" ;;
    *)      GL_LIBS="-lGL" ;;
esac

echo "== building tools/out/video_check"
# shellcheck disable=SC2086
c++ -std=c++17 -O1 -I src $SDL_CFLAGS -o tools/out/video_check \
    tools/video_check.cpp \
    src/gfx/Renderer.cpp src/ui/Font.cpp src/ui/Video.cpp src/ui/Loading.cpp \
    src/world/Terrain.cpp src/world/Way.cpp src/world/B3D.cpp \
    src/world/SceneTextures.cpp src/world/GroundMap.cpp \
    src/res/Res.cpp src/res/Mask.cpp \
    $SDL_LIBS $GL_LIBS

echo "== running tools/out/video_check"
./tools/out/video_check build/media original

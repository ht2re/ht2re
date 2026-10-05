#!/usr/bin/env bash
# Serial runner: shared screenshot names must never race between suites.
set -uo pipefail
cd "$(dirname "$0")/.."
PYTHON=${HT2_TEST_PYTHON:-python3}
if ! "$PYTHON" -c 'import numpy' >/dev/null 2>&1 && [[ -x /opt/homebrew/bin/python3 ]]; then
    PYTHON=/opt/homebrew/bin/python3
fi
LOGDIR=$(mktemp -d "${TMPDIR:-/tmp}/ht2-verify.XXXXXX")
checks=0; failures=0
run() {
    local name=$1; shift
    checks=$((checks+1))
    echo "== $name"
    if "$@" >"$LOGDIR/$name.log" 2>&1; then
        echo "PASS $name"
    else
        failures=$((failures+1))
        echo "FAIL $name"
        tail -12 "$LOGDIR/$name.log"
    fi
}
summary() { echo "$checks suites, $failures failures; logs: $LOGDIR"; }
if [[ ${1:-} == --self-test-failure ]]; then
    run injected-failure false
    summary
    exit "$failures"
fi
run build cmake --build build -j 6
if (( failures )); then summary; exit 1; fi
run scripts tools/build_script_check.sh original
run economy tools/build_economy_check.sh
run input tools/build_input_test.sh original
run audio tools/build_audio_check.sh
run physics tools/physics_ab.sh ZIL
run road-safety-build c++ -std=c++17 -O2 -Isrc tools/road_safety_test.cpp src/world/GroundMap.cpp src/world/RoadSurface.cpp src/world/Terrain.cpp src/world/Way.cpp src/game/Vehicle.cpp src/game/Collision.cpp -o "$LOGDIR/road-safety-test"
run road-safety "$LOGDIR/road-safety-test"
run lane-build c++ -std=c++17 -O2 -Isrc tools/traffic_lane_test.cpp src/game/Traffic.cpp src/game/Collision.cpp src/world/Way.cpp -o "$LOGDIR/lane-test"
run lanes "$LOGDIR/lane-test"
run traffic-build c++ -std=c++17 -O2 -Isrc tools/traffic_test.cpp src/game/Traffic.cpp src/game/Collision.cpp src/world/Way.cpp -o "$LOGDIR/traffic-test"
run traffic "$LOGDIR/traffic-test" original/ENV 100 1800 1 1
# Use the same current engine objects; exclude the application entry points.
objects=()
while IFS= read -r object; do objects+=("$object"); done < <(find build/CMakeFiles/ht2re.dir -name '*.o' | sort)
linked=()
for object in "${objects[@]}"; do
    case "$object" in */main.cpp.o|*/App.cpp.o) ;; *) linked+=("$object");; esac
done
glflags=()
if [[ $(uname) == Darwin ]]; then glflags=(-framework OpenGL); else glflags=(-lGL); fi
run steering-build c++ -std=c++17 -O2 -Isrc tools/cockpit_steering_test.cpp "${linked[@]}" \
    $(pkg-config --cflags --libs sdl2 sdl2_mixer) "${glflags[@]}" -o "$LOGDIR/steering-test"
run steering "$LOGDIR/steering-test" "$LOGDIR/steering"
if [[ -f build/media/kotr.rvid ]]; then run video "$PYTHON" tools/video_probe.py build/media/kotr.rvid; fi
if [[ ${1:-} != --quick ]]; then
    run screenshots "$PYTHON" -u tools/shot_test.py
    run menu "$PYTHON" -u tools/menu_flow_test.py
    run mirror tools/mirror_check.sh 20 original
fi
summary
(( failures == 0 ))

#!/bin/sh
# Physics A/B: build tools/vehicle_bench.cpp against the pre-recovery
# src/game/Vehicle.cpp (from git HEAD, the commit the port shipped before the
# vehicle.tech recovery) and against the current one, and print both tables.
#
#   tools/physics_ab.sh            # both models, default vehicle
#   tools/physics_ab.sh ZIL        # pass a vehicle.tech section name through
set -e
cd "$(dirname "$0")/.."
mkdir -p tools/out tools/out/old/game

VEH="$1"

echo "== pre-recovery model (git HEAD:src/game/Vehicle.cpp)"
ln -sfn ../../../src/gfx   tools/out/old/gfx
ln -sfn ../../../src/world tools/out/old/world
ln -sfn ../../../src/util  tools/out/old/util
cp -f src/game/Collision.h tools/out/old/game/Collision.h
git show HEAD:src/game/Vehicle.h  > tools/out/old/game/Vehicle.h
git show HEAD:src/game/Vehicle.cpp > tools/out/old/game/Vehicle.cpp
# the old sources include "Collision.h"/"../world/GroundMap.h" by relative path,
# so stage the copy at the same depth as src/game/ and add it to the include path
c++ -std=c++17 -O2 -Itools/out/old -Isrc -o tools/out/vehicle_bench_old \
    tools/vehicle_bench.cpp tools/out/old/game/Vehicle.cpp src/game/Collision.cpp src/world/GroundMap.cpp src/world/B3D.cpp src/world/Terrain.cpp
./tools/out/vehicle_bench_old "$VEH" | grep -E '^RESULT|^#'

echo
echo "== vehicle.tech model (current src/game/Vehicle.cpp)"
c++ -std=c++17 -O2 -Isrc -DHT2_NEW_VEHICLE=1 -o tools/out/vehicle_bench_new \
    tools/vehicle_bench.cpp src/game/Vehicle.cpp src/game/Collision.cpp src/world/GroundMap.cpp src/world/B3D.cpp src/world/Terrain.cpp
./tools/out/vehicle_bench_new "$VEH" | grep -E '^RESULT|^#'

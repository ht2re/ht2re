#!/usr/bin/env bash
# Physics A/B: build tools/vehicle_bench.cpp against the pre-recovery
# src/game/Vehicle.cpp (from PHYSICS_BASE_REF, default 5973155, before the
# vehicle.tech recovery) and against the current one, and print both tables.
#
#   tools/physics_ab.sh            # both models, default vehicle
#   tools/physics_ab.sh ZIL        # pass a vehicle.tech section name through
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p tools/out

VEH=${1:-ZIL}
REF=${PHYSICS_BASE_REF:-5973155}
BASE=$(mktemp -d "${TMPDIR:-/tmp}/ht2-physics-base.XXXXXX")
git archive "$REF" src | tar -x -C "$BASE"

echo "== baseline $(git rev-parse --short "$REF") (dependencies from that revision)"
# Compile baseline dependencies from the same revision, never mix GroundMaps.
old_sources=("$BASE/src/game/Vehicle.cpp" "$BASE/src/game/Collision.cpp"
    "$BASE/src/world/GroundMap.cpp" "$BASE/src/world/B3D.cpp" "$BASE/src/world/Terrain.cpp")
if [[ -f "$BASE/src/world/RoadSurface.cpp" ]]; then old_sources+=("$BASE/src/world/RoadSurface.cpp"); fi
c++ -std=c++17 -O2 -I"$BASE/src" -o tools/out/vehicle_bench_old tools/vehicle_bench.cpp "${old_sources[@]}"
./tools/out/vehicle_bench_old "$VEH" | tee tools/out/physics_old.log

echo
echo "== vehicle.tech model (current src/game/Vehicle.cpp)"
c++ -std=c++17 -O2 -Isrc -DHT2_NEW_VEHICLE=1 -o tools/out/vehicle_bench_new \
    tools/vehicle_bench.cpp src/game/Vehicle.cpp src/game/Collision.cpp src/world/GroundMap.cpp src/world/RoadSurface.cpp src/world/B3D.cpp src/world/Terrain.cpp
./tools/out/vehicle_bench_new "$VEH" | tee tools/out/physics_new.log
echo "Baseline fixture retained at $BASE; not an original-game physics oracle."

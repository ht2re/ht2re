# World collision — what the truck can drive through

Update 2026-10-05: player driving passes its vertical body interval to
`resolveMove`. Wall triangles are kept in 32m spatial bins, clipped to that
interval and only then projected onto the ground plane. Geometry above a
truck or below its road no longer forms an infinite-height wall. Real walls
still stop sweeps. The interval height comes from the loaded truck mesh bbox
(4m fallback without a mesh). Existing heightless queries/capsule AI queries
retain the historical 2D grid; the description below refers to that path.
`tools/road_safety_test.cpp` covers overhead/underground walls, sloped clipping
and retention of real blocking geometry.

`src/game/Collision.{h,cpp}` answers one question against the *drawn* world geometry:

> can a body of radius `r`, centred at `(x, z)`, be here — and if not, where does it stop
> and how does it slide along the wall?

Everything is derived from the meshes the renderer already draws
(`B3D::buildScene()` → `std::vector<B3D::SubMesh>`, passed to `Collision::build()`); no
extra data file, no hand-placed boxes, no hard-coded coordinates.  The simulated world is
**two-dimensional** (x, z, the B3D ground plane); height only enters through the rule that
decides which triangles are walls and which are surfaces.

Contents: [1 rule](#1-the-blocking-rule) · [2 grid](#2-the-grid) · [3 API](#3-api)
· [4 numbers](#4-numbers-on-the-shipped-data) · [5 verification](#5-verification)
· [6 limitations](#6-known-limits) · [7 tools](#7-tools)

---

## 1. The blocking rule

A `SubMesh` is one `(face node × material)` batch, so the rule is applied in two stages and
is mirrored 1:1 by `tools/collision_probe.py`.

**Stage A — whole-SubMesh filters**

1. **Material.**  The lower-cased material name is matched against
   `GROUND_MAT` (`road grass beton concrete gravel zebra spl ground ter_ water cross mark
   asphalt sand dirt pavement sidewalk kerb curb trotoar trotuar stoika stoyanka parking
   runway taxiway apron swamp bshadow river tonnel zont bottom_bridge bridge_0`) — such a
   SubMesh is a drivable surface, apron, yard, water or decal and never blocks.  If the name
   *also* matches `SOLID_MAT` (`stolb pillar column kolonn wall fence confence ograjdenie
   rama beton_bridge side_beton`) the solid rule wins: `mat_beton_stolb` ("concrete
   **pillar**") and `mat_beton_stolb_top` block, plain `mat_beton` ground does not, and
   `mat_road_fence` is a fence, not a road.
2. **Flat decals.**  A SubMesh whose height extent is `< 0.5 m` is a painted marking,
   blob shadow or tiny poster — skipped.  (On the shipped data this rejects 4 994 of the
   23 889 SubMeshes; their triangles are mostly already horizontal.)

**Stage B — per-triangle filters**

3. **Sentinel vertices.**  A triangle with any vertex at `|x| > 20000`, `|z| > 20000` or
   `|height| > 1000` is dropped.  The tiles really do contain such vertices
   (`mat_forest0b` reaches `height = 1000012`, `mat_forest0p` `height = −10000`).
4. **Surface orientation.**  Let `n` be the triangle's world-space normal and `n_h` its
   height ("up") component.  If `|n_h| / |n| > 0.8` (shallower than ≈ 37°) the triangle is a
   *surface* — ground, roof, bridge deck, canopy top, container lid — and does not block.
   Everything else is wall-like and blocks.
5. The remaining triangles are rasterised into the grid (see below) with a conservative 2D
   triangle-vs-cell separating-axis test: a triangle that touches a cell marks it.

The orientation filter is what keeps *drive-in* geometry usable: the repair-shop bays of
`STOS`/`stos` warehouses are entered through door gaps in the wall ring, and gas-station
canopies and roads under overpasses are open because the deck above them is horizontal.
Blocking only the vertical triangles is also what makes the `.way` node
`node_car_repair_*` (which sits *inside* a small shop) reachable.

## 2. The grid

| property | value |
| --- | --- |
| cell size | **1.0 m** (default; `build(meshes, cell)` overrides) |
| storage | one bit per cell, `std::vector<uint64_t>` (LSB-first within a word) |
| origin | `floor((min − 2·cell) / cell)·cell` over every non-ground, non-flat SubMesh vertex |
| shipped world | `9447 × 10931` cells = 103.3 km², **12.9 MB**, 1 339 819 solid cells (**1.30 %**) |

The suggested cell range in the task brief was 2–4 m; 1 m was chosen because the smallest
drive-in bays are only ~8 m wide and a 2 m grid (plus the vehicle radius) inflated the walls
enough to block the `node_car_repair_*` parking node inside `STOS` (aa.b3d).  At 1 m the
bay stays reachable, the query is still a handful of cells, and the whole grid is 13 MB.
If some mod ever produces geometry that would need more than 256 M cells the builder doubles
the cell size instead of allocating.

Queries are constant time in the world size:

* `blocked(x, z, r)` — iterate the cells overlapped by the circle's bounding box and test the
  cell rectangle against the circle (`dx² + dz² <= r²`; `r = 0` tests the containing cell).
* `resolveMove(from, to, r)` — sweep the segment in `max(0.25 m, cell/2)` substeps capped at
  256 samples, bisect 10× between the last free and the first blocked sample (so a fast step
  cannot tunnel through a wall), then slide: take the contact normal of the solid cells
  around the stop point, remove the component pushing into the wall and sweep the remaining
  tangential motion, up to three times.
* Points outside the grid are free (the truck is not walled in by the map border).

## 3. API

```cpp
class Collision {
    void build(const std::vector<B3D::SubMesh>& meshes);              // default 1.0 m cells
    void build(const std::vector<B3D::SubMesh>& meshes, float cell);  // explicit cell size
    void clear();
    bool empty() const;  size_t cells() const;  float cellSize() const;
    float minX() const;  float minZ() const;  int width() const;  int height() const;

    bool blocked(float x, float z, float r) const;                    // circle test
    bool resolveMove(float fromX, float fromZ, float toX, float toZ, float r,
                     float& outX, float& outZ) const;                 // sweep + slide

    // --- capsules (vehicles, src/game/Traffic.cpp contact resolution) -----
    struct Body { float radius = 1.0f; float length = 4.5f; };        // half width / bumper to bumper
    static void bodyAxis(float x, float z, float yawDeg, const Body& b,
                         float& ax, float& az, float& bx, float& bz);
    bool blockedBody(float x, float z, float yawDeg, const Body& b) const;
    bool resolveBody(float fromX, float fromZ, float fromYaw,
                     float toX, float toZ, float toYaw, const Body& b,
                     float& outX, float& outZ) const;
    static float bodyOverlap(float ax, float az, float ayawDeg, const Body& a,
                             float bx, float bz, float byawDeg, const Body& b,
                             float& nx, float& nz);                   // overlap + normal A->B

    struct Stats { size_t submeshes, ground, flat, triangles, cells, gridCells; };
    const Stats& stats() const;                 // build counters, for logging
    bool solidCell(int ix, int iz) const;       // tool/test hook
    static float defaultCellSize();             // 1.0f
};
```

`resolveMove` returns `true` when the whole segment is free (the body ends at `toX/toZ`);
otherwise it returns `false` and `outX/outZ` is the furthest free point after sliding —
always a position where `blocked(outX, outZ, r)` is `false`.

**Wiring still missing in App.**  `App` does not yet own a `Collision`.  To use it:

```cpp
#include "game/Collision.h"
Collision collision_;                       // member
// after loadWorldTiles() has filled worldMeshes_:
collision_.build(worldMeshes_);             // ~60 ms for the shipped 39 tiles
// in the vehicle step:
float nx, nz;
if (!collision_.resolveMove(v.x(), v.z(), v.x() + dx, v.z() + dz, bodyRadius, nx, nz))
    v.setPosition(nx, nz);                  // blocked: keep the slid position
```

`B3D::SubMesh::group` (added by the parent) is not consulted by `Collision`: all groups are
static world geometry.  If dynamic groups (roadworks, people, floods) should ever be
non-solid, `build()` can skip them with one `if` on `sm.group`.

## 4. Numbers on the shipped data

Measured with `tools/collision_check` (C++) and `tools/collision_probe.py` (Python) over all
`original/ENV/*.b3d` (39 tiles).  "disc r" = fraction of a disc of that radius whose points
fall in a solid cell, sampled at 0.5 m.

| place | x | z | disc r=3 | disc r=12 | blocked(r=0) |
| --- | ---: | ---: | ---: | ---: | --- |
| airport apron | −3063 | −4658 | 0.0 % | 0.0 % | free |
| highway | −2330 | −3760 | 0.0 % | 0.4 % | free |
| player spawn (ENV/GAME.CNF) | −3020 | −4625 | 0.0 % | 0.3 % | free |
| warehouse `STOS` repair bay | 4750.5 | −5675 | 14.2 % | 25.6 % | free (floor) |
| warehouse `STOS` interior wall | 4748.1 | −5675 | 21.2 % | 24.7 % | **blocked** |
| repair-shop `af` parking bay | −2400 | 2758.5 | 26.5 % | 17.0 % | **blocked** |

Build: 39 tiles → 23 889 SubMeshes (5 458 ground + 4 994 flat skipped) → 211 730 solid
triangles → 1 339 819 solid cells.  B3D assembly 66 ms, collision build 60 ms.

**`origin/ENV/*.way` sanity checks** (the truck has to be able to stand on the road nodes):

* nodes: **762 total, 53 blocked at `r = 0` (7.0 %)**, 96 blocked at `r = 1 m`.
  Breakdown of the 53: 19 `debris_*` (a debris mesh is modelled exactly there), 12
  `*car_repair*` bays inside the small shops, and 22 mixed (map-border posts `mat_post_*` /
  `mat_county`, hidden-car markers in forests, `mat_fon*` lamp poles, store parking rows).
* road centreline (`RSEG`/`VDAT` polylines, sampled 9× per segment): **68 346 samples,
  3 687 blocked at `r = 0` (5.39 %)**.  The blocking materials are dominated by man-made
  structures — `mat_confence`, `mat_bricks`, `mat_concrwr`, `mat_wall1/2`, `mat_fence*`,
  the border `mat_post_*`, `stos_*` shop interiors — with steep terrain
  (`mat_mount3/4`, `mat_slope1/2` ≈ 1.2 k of ≈ 30 k triangle hits) a small minority.

**Driveable-area spot check:** the `STOS` interior is a drive-in repair bay, so it is
deliberately *not* filled in (the whole-map solid fraction is 1.3 %, not a building area
fraction); its walls, the small shop walls and every fence/pole still block.

### Capsules

A vehicle is not a disc: `Body` is the car's rectangle with rounded corners - a
segment of `length - 2*radius` along the heading (both in metres, `yaw` in
degrees with forward = `(sin, cos)`, the `Vehicle`/`Renderer` convention) with
`radius` around it.  `blockedBody()` samples the axis with circles at most
`radius` apart, so a pole can never slip between two samples; `resolveBody()`
bisects the last free point on the way to the target pose and then slides along
the contact normal (three iterations).  `bodyOverlap()` is the capsule-vs-capsule
test used by the traffic contact resolution: it returns the overlap (> 0 when the
bodies touch) and the unit normal from A to B.

## 5. Verification

* `cmake --build build -j` — builds clean (the module is already in `CMakeLists.txt`).
* `tools/collision_check --unit` — 15 deterministic checks on a synthetic wall quad:
  circle clear/touching/reaching, ground submesh never solid, head-on stop, long-step
  tunnelling, and a diagonal move into the wall that **slides from (−5,−5) to (1.50, 5.00)**
  instead of stopping dead.  **0 failures.**
* `tools/collision_probe.py --compare-cpp tools/out/cpp_grid.bin` — an independent Python
  re-implementation (parser `tools/b3d_full.py`, own walker mirroring `B3D::buildFrom`)
  rasterises the same rule and is diffed cell-by-cell against the C++ grid:
  **23 889 SubMeshes and 138 210 blocking triangles in both, 263 differing cells out of
  103.2 M (0.0003 %)**.  The residue is float32-vs-float64 rounding: triangles sitting within
  0.001 of the 0.8 orientation threshold, and coordinates landing on opposite sides of a
  cell boundary.  (Before `mat_forest*` became scenery - see 6 - the same measurement gave
  211 730 triangles / 546 cells; the three numbers move together.)
  (The Python probe computes the normal in float32 too, which removes the triangle-count
  difference; the residual 546 cells are coordinates landing on opposite sides of a cell
  boundary.)
* Sanity on the required points: apron/highway/spawn free, warehouse walls blocked — see the
  table above.

A gotcha worth remembering for anyone writing another walker: in
`ht2::B3D::buildFrom` the `depth` argument passed to children is **not** incremented — only
a followed `subtag 18` link does `depth + 1`.  `depth` is therefore the *link nesting level*
(used by the `depth == 0` "skip linked containers / `hit_*` proxies" tests), not the tree
depth; an early version of the probe incremented it per child and silently lost ~4 000
SubMeshes on link-heavy tiles (aa, ad, aj, al, am).

## 6. Known limits

* **Legacy capsule queries are 2D.** A vertical face projected onto an AI route
  may still block another level. Player circle/sweep queries now accept
  `bottom, top` after the existing arguments and clip the actual wall triangles;
  callers omitting these parameters retain the old occupancy-grid behavior.
* **The authored walls are a list, not cells (2026-10-16).** `Collision::setWalls()`
  keeps the `hit_*` proxies' subtag-9 planes with their subtag-23 extents as bounded
  zero-thickness walls and `resolveMove()` tests them by crossing
  (`crossedWall`), before the grid - so a car stops *at* the authored surface
  instead of a cell short of it.  `--proxy-collision` enables it (5 993 walls from
  2 034 proxies); the default path is unchanged.  See
  [hit_proxies.md](hit_proxies.md).
* **Steep terrain blocks.**  `mat_mount*`, `mat_slope*`, swamp billboards and individual
  `mat_tree*` billboards with `|n_h| ≤ 0.8` are solid, so hillsides and lone trees are
  natural barriers.  This follows the brief's "block the rest"; adding `mount|slope` to
  `GROUND_MAT` would open them up.
* **`mat_forest*` does not block (2026-10-16).**  Those are scenery billboards: a single
  vertical quad tens of metres across used to paint a forest block.  Solid they put
  invisible walls straight across roads - `ap.b3d`'s `obj_ap_048` spans
  `x[-3295..-3200] z[-4701..-4600]` and crossed the apron road 80 m west of the
  GAME.CNF start, so the truck could not leave the airport.  The engine's real collision
  is the authored `hit_obj_*` / `hit_road_*` geometry, which the port still does not use:
  those containers are linked (subtag 18) and carry subtag-40 records
  (`AAhitTerrain`), so their semantics are a separate reverse-engineering task.
  Individual `mat_tree*` objects keep blocking.  Reproduce with
  `tools/out/wall_probe original/ENV -3100,-4625` (see 7).
  **The proxies are decoded now** - see [hit_proxies.md](hit_proxies.md): a
  `hit_obj_*` container holds footprint polygons (subtag 20, count x (x,z,y)),
  face planes (subtag 9, `n·p + d = 0` with the normal stored `(x,z,y)`), the
  vertical bottom (subtag 12, `params[7] = -y_min`) and links (subtag 18) to real
  containers (`STOS_walls`, `benzo_hit`, `store_*_hit`, ...).  Validated
  statistically: 99.2 % of the polygons touch their object's bbox edge, 86.0 % of
  the planes are tangent to it.
* **How much of the road really is blocked - measured (2026-10-16).**
  `tools/out/road_block_audit original` queries every `.way` polyline sample against the
  blocking triangles (the same two filters) and splits the hits by whether the authored
  asphalt is under them:

  | | samples | share |
  |---|---|---|
  | all `.way` samples | 9 853 | 100 % |
  | inside blocking geometry | 725 | 7.36 % |
  | ... **on authored asphalt** | 205 | **2.08 %** |

  So most `blocked` samples are `.way` polylines crossing yards, parking rows and depots,
  where a fence/pillar legitimately stands.  The 205 on-asphalt cases are thin objects
  whose geometry clips the carriageway - `mat_confence` 21 %, `mat_fence*` 29 %,
  `mat_tun_dn`/`mat_tun_w0` 8 % (tunnel walls), `tree*`/`mat_tree89` 13 %,
  `mat_bricks` 4 %, bridge rails (`mat_bridge_7`, `mat_road_fence`, `mat_rfence`,
  `mat_ograjdenie_bridge`, `mat_br_*`) 5 %.  With the ~1 m conservative cell inflation
  plus the body radius a truck can catch those, which is the remaining "invisible wall"
  report.  The authored `hit_obj_*` proxies are the faithful fix.
  **How much of it is authored after all (2026-10-16):** the same tool now also loads
  the `hit_*` proxies' wall planes and asks whether an authored plane passes within
  2 m of each on-asphalt hit - **116 of 205 (56.6 %) do** (1506 planes world-wide).
  Those walls are the authors' own; the original blocks there too, with a
  zero-thickness plane instead of the port's inflated cell.  The remaining 43 % are
  real obstacles without a plane (debris, parked/hidden cars) or port artifacts.
  See [hit_proxies.md](hit_proxies.md) §10.
* **Wall inflation.**  Conservative cell marking makes every wall ~1 m thicker than it is
  (plus the body radius in the query), so a truck needs `1 m + r` clearance to pass.
* **`hit_*` proxies are not used.**  `B3D::buildScene` skips the game's own collision-only
  containers (`hit_obj_*`, `hit_road_*`), so the collision geometry here is derived from the
  visible meshes.  Using them would be more faithful but requires them in the render list.
* **Absurd geometry** is dropped by the sentinel filter; a mod with |x| > 20 km is ignored.

## 7. Tools

```bash
# C++ harness (not part of CMakeLists; build it by hand)
c++ -O2 -std=c++17 -Isrc tools/collision_check.cpp src/game/Collision.cpp \
    src/world/B3D.cpp src/world/Way.cpp -o tools/out/collision_check
tools/out/collision_check --unit                 # deterministic blocked/slide tests
tools/out/collision_check                        # build stats, points, .way checks
tools/out/collision_check --dump tools/out/cpp_grid.bin   # raw grid for the Python diff
tools/out/collision_check --only aa.b3d --list out.txt    # per-SubMesh dump
tools/out/collision_check --probe X Z            # SubMeshes whose bbox contains (X,Z)

# Which authored triangle makes a point solid?  Mirrors the C++ filters and prints
# every blocking triangle covering the point (material + footprint + up component):
c++ -O2 -std=c++17 -Isrc tools/wall_probe.cpp src/world/B3D.cpp -o tools/out/wall_probe
tools/out/wall_probe original/ENV -3100,-4625     # -> mat_forest1 obj_ap_048
tools/out/wall_probe original/ENV --hits          # hit_* proxy containers per tile
tools/out/wall_probe original/ENV --dump original/ENV/ap.b3d   # raw proxy params

# Which material blocks the road network, and how much of it is really on asphalt:
c++ -O2 -std=c++17 -Isrc tools/road_block_audit.cpp src/world/B3D.cpp src/world/Way.cpp \
    -o tools/out/road_block_audit
tools/out/road_block_audit original

# Python reproduction over original/ENV/*.b3d (parser: tools/b3d_full.py)
python3 tools/collision_probe.py                 # the table above + grid stats
python3 tools/collision_probe.py --ways          # every .way node at r = 0/1/1.5/2 m
python3 tools/collision_probe.py --materials     # triangles vs blocking per material
python3 tools/collision_probe.py --compare-cpp tools/out/cpp_grid.bin
python3 tools/collision_probe.py --png tools/out/grid.png    # block map
```

The probe caches the flattened world in `tools/out/collision_tris_all_v2.npz` (delete it or
pass `--refresh` after changing the assembler).

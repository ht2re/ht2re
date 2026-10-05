# The road breaking apart on bends ("дорога разваливается на поворотах")

Update 2026-10-05 (repeat report): physics now prefers the containing authored
asphalt triangle over the graph height. `RoadSurface::heightAt` reports zero
horizontal distance for a contained point, rather than distance to a triangle
edge: wide road polygons previously blended with terrain or disappeared from
the contact layer at their centre. Height queries no longer mutate the vehicle
reference while sampling motion/individual wheels. The 32m graph profile/ramp
remains a fallback where no authored road triangle answers. The historical
layer-precedence descriptions below are superseded by this paragraph.

`road-probe` on the same 170190 tile points: single-surface p95 error reduced
from 0.250m to 0.000m; on-network p95 from 0.450m to 0.000m; off-network missing
layer answers 8 → 0. Residual max 12m at tier/overlap ambiguities is still open.
Physics/camera pose fixes do not prove that all temporal texture aliasing or
depth fighting reported in the user's exact location is gone.

Root cause, fix and the measurements behind it.  Everything below is
reproducible from the commands in §6; the frames are
`shots/road_break_before.png` / `shots/road_break_after.png`.

## 1. Symptom

In the world view the asphalt and the guard-rail running around a bend are cut
off: a flat green sheet lies across the road and the road only reappears on the
far side.  Worst on the mountain highway (the `bd`/`ba` tiles), where the road
runs in a cutting with the surrounding country 30..60 units higher.

## 2. Which layer is it

Three candidates were measured, not guessed:

| candidate | verdict |
|---|---|
| gaps between the strip triangles of the road quads (`B3D::triangulate`) | **no** - see §5 |
| the tile geometry itself missing there | **no** - the same camera with `--no-ground` (which drops *only* the generated ground mesh) still draws the road, the guard rail and the embankment |
| the generated ground mesh | **yes** - with `--no-ground` the green sheet disappears and the road is continuous |

So the generated ground mesh (`Renderer::buildTerrainMesh`) was covering the
road.  The proof is the pair of frames:

```sh
./build/ht2re --data original --scene world --screenshot --out /tmp/full.bmp \
  --spawn -2886,-4643,70 --camera -1740.5,31,3960,80.5,-14 --traffic 0
./build/ht2re --data original --scene world --screenshot --out /tmp/noground.bmp \
  --spawn -2886,-4643,70 --camera -1740.5,31,3960,80.5,-14 --traffic 0 --no-ground
```

## 3. Why it happened (the geometry)

The shipped `MTR/terrain*.raw` is 257x257 samples at **415 units/sample**, i.e.
the generated ground was a grid of 415-unit triangles ~106 km across.  The old
code clamped every *vertex* of that grid to

```
y = min(y, tileGround(vertex) - 2)
```

where `tileGround` was the lowest ground-map cell that rounds to that vertex,
grown by two samples.  That bounds the surface **at the vertices** and nowhere
else: between two vertices the mesh is a linear interpolation, and the ground
under it is not.  On a bend the road sits 30..60 units below the country on one
side, so the neighbouring grid vertex was tens of units higher and the triangle
lifted straight back above the asphalt a few metres away from the clamped
vertex.  The clamp could not win: no per-vertex bound can bound the
interpolation.

Two more defects in the same code path:

* the clamp value was `min(terrain, tileGround - 2)` where `tileGround` was the
  minimum over a 415-unit block, i.e. the *lowest* thing anywhere in a 400 m
  radius.  Every road therefore pressed the country flat over a whole cell -
  the source of the flat "shelves" with hard straight edges that were reported
  separately.  (Measured on the first attempt at a fix: 39.8% of a landscape
  band could be flat slab.)
* the clamp was never applied where the terrain grid had no ground cell inside
  its dilation radius, which is most of the island's edge.

## 4. Fix

The generated ground is now built from a **fine** grid (16 units) whose height
is the raw calibrated heightmap clamped under a proper **tile-surface
ceiling**:

```
y = min( rawHeightmap(x,z), limit(x,z) )        with
limit = near + (raw - near) * smoothstep(dist / 450)  - margin
```

* `near` and `dist` come from `GroundMap` (src/world/GroundMap.{h,cpp}):
  - the ceiling is seeded from the **coverage of every tile triangle**, not from
    its vertices.  A runway or apron is one quad a kilometre wide with four
    corners; a vertex-based field covers four cells and leaves the rest
    unclamped.  Each 12-unit cell centre is barycentric-tested against every
    triangle whose bounding box contains it and keeps the *lowest* surface
    (buildings stand on the ground, the ground under them is the ceiling).  Both
    xz windings count - the tiles mix them - and a triangle smaller than a cell
    is caught by a vertex seed.  299,224 of the world's 641,593 triangles took
    part and covered 81,310 of 728,458 cells.
  - the field is dilated by 3 cells (36 units) so it covers a triangle's whole
    footprint, then a chamfer transform gives, for every cell of the map, the
    distance to the nearest covered cell and the surface height there.
* the **ramp** is what turns the clamp into a cut slope instead of a plateau:
  right at the tiles the ground is pinned just under the tile surface, and it
  rises to the natural height over 450 units.  A hard `min()` leaves a straight
  cliff across the landscape; a *constant* slope leaves a wall right next to
  every road (tried: the terrain reached a 40-unit bank within 50 m of the
  road).  The smoothstep ramp reaches the heightmap at a fixed distance
  whatever the local relief is.
* the **margin** is slope adaptive: 1 unit flat, plus `(|dy/dx| + |dy/dz|) *
  cell` capped at 4:1, computed from the *pass-1* heights so the clamp cannot
  feed its own slope estimate (that ran away to -56 km on the first attempt).

`--terrain` forces the old 415-unit mesh; it is the "before" side of every
measurement below.

## 5. The strip triangulation was checked and is fine

`B3D::triangulate()` expands a polygon either as a fan (`strip == false`) or as
a triangle strip with alternating winding when the `0x80` bit of the polygon's
`w0` word is set, which is what the original loader at king.exe 0x5f2800 does.
The two implementations were compared rule by rule against the independent
reader in `tools/collision_probe.py` (`triangulate(idx, (w0 & 0x80) != 0)`,
lines 142-153): same flag, same `t % 2` alternation, same `(t-2, t-1, t)` /
`(t-1, t-2, t)` winding - identical triangle lists.

And the measurement above rules it out as the cause of *this* bug anyway: the
road is complete in the `--no-ground` frame, which draws exactly the same road
triangles from the same buffers.

## 6. Numbers

Metric: render the view twice, once with the ground and once with
`--no-ground`, and count the pixels that are **asphalt (r~g~b) in the scene-only
frame but terrain-green in the full frame**.  A road that is not buried scores
0; `--terrain` is the control that must still score.

| camera (the bend unless noted) | asphalt px | covered by terrain, `--terrain` (before) | after | change |
|---|---|---|---|---|
| `-1740.5,31,3960,80.5,-14` (bd cutting bend) | 45,948 | **411 (0.89%)** | **12 (0.03%)** | **-34x** |
| `-2250.5,48,3700,0,-14` (ba highway) | 59,339 | 16 | 16 | - |
| `-2250.5,48,3700,0,-60` (ba highway, top down) | 152,911 | 17 | 17 | - |
| `4443.5,479,-5492,-26.4,-14` (aa depot road) | 4,705 | 12 | 12 | - |
| `4750,478,-5670,10,-20` (aa depot) | 39,011 | 72 | 72 | - |
| `-2915,30,-4724,0,-10` (ap apron) | 136,263 | 73 | 73 | - |
| `-3085,30,-4724,180,-10` (ap apron) | 90,221 | 14 | 14 | - |
| **total** | 528,398 | **615 (0.116%)** | **216 (0.041%)** | **-2.8x** |

The residual 216 pixels are silhouette pixels (1-2 px of terrain edge moving
with the finer mesh) and are identical before/after on five of the seven
cameras; the bend itself drops from 411 to 12.

The test suite carries the check:
`road continuity: asphalt not buried by the terrain at a bend` (14/45,946 =
0.03%) plus the control `the pre-fix mesh (--terrain) still breaks it`
(413 px).

### The airport apron is tile geometry, not the terrain

The large flat grey polygon right of the road at `-2900,60,-4600` (reported as
a "grey slab lying across the landscape") is present with `--no-ground` as
well, so it is the `ap` tile's own apron quad - a 1 km concrete slab at h = 20
that the shipped tile data ends abruptly.  It is not produced by the terrain
mesh and no terrain fix can remove it.  The green around it is the new ground,
which is textured (`grass1`, the tile's own ground texture) rather than the flat
dark sheet it used to be.

## 7. Cost

The fine mesh is 798x885 cells (16 units) = 1,410,270 triangles over the mapped
world plus a 1600-unit skirt.  Wall clock is dominated by the *tile ceiling*
build: 299k triangles rasterised into 728k cells plus four chamfer sweeps,
~0.2 s once at startup.  Per frame the ground is one `glDrawElements` range, as
before (the extra triangles cost about 1 ms on an Apple M4; the ENV scene is
651k triangles by comparison).  The startup logs are

```
ground: tile ceiling seeded by 299224 triangles -> 81310 of 728458 cells covered
ground: tile ceiling 797x914 cells, radius 3 -> 152363 cells on the surface, fade 450 units
ground mesh: 798x885 cells of 16 units, 1410270 tris (471181 vertices clamped -80.4..480.1)
```

## 8. Why the cars were leaving the road (and the road "shaking")

The ground rebuilt in §4 is a **terrain** field: it is the raw heightmap clamped
*under* the tile surface, and the clamp is what makes it hug the asphalt (1 unit
at the tightest point, and only within a few metres of a tile).  Three consumers
were reading it as if it were the *road*:

| consumer | what it did | measured error |
|---|---|---|
| the player's truck (`Vehicle::update`) | took its wheel contact height from `GroundMap::heightAt()` at five points | the wheel plane sat **4.5..8.8 m below the asphalt** (the distribution of the ground-mesh gap, below) |
| the AI cars (`App::drawTraffic`) | replaced `Car::h` with `heightAt()` whenever the two were within 4 m | up to 4 m of lift/sink for the cars in that band |
| the parked cars / roadblocks | used the `.way` node height, which *is* the road height | correct (0.00 m median) - left alone, with the road profile kept as a `0` fallback |

The road network carries its own profile - every `RSEG` polyline point of every
tile's `.way` - and it agrees with the authored asphalt to a **0.00 m median**
(66,653 vertex pairs within 25 m; only 2.4% of them differ by more than 3 m,
which is the width of a carriageway and the ramp of a junction).  That profile is
now kept as a separate field of the ground map (`GroundMap::road_`, built from
**every** `.way` of every tile instead of `aa.way` alone, averaged per cell,
grown by one 12-unit cell so the whole carriageway is covered, and given its own
chamfer transform) and exposed as `surfaceOnRoad()` / `surfaceAt()`.  The terrain
field is no longer smoothed across a road cell either (that pull towards the
off-road ground in the same cell was a second, smaller source of error).

`Vehicle::setRoads(&ground_)` makes the truck follow the road profile wherever it
is on a road and the terrain field everywhere else; `App::drawTraffic` keeps the
graph height (`Car::h`, the `.way` profile blended across junctions by
`Traffic`), asking the map only when the graph carries no height at all.

### The road does not z-fight the ground - and how that was established

The obvious suspect for "дороги всё ещё дрожат" after §4 was depth fighting
between the asphalt and the ground sheet a metre below it, so it was measured
rather than assumed.  `tools/road_fight.py` renders the same view from two camera
poses 0.03 deg apart (~0.4 px) and asks, per pixel:

* the asphalt mask comes from **geometry**, not colour: `--only-mat mat_road
  --no-ground` against a material prefix that matches nothing;
* `flip` = asphalt pixels where the full frame stops drawing the authored road
  at one pose but not the other (the z-fighting / popping signature);
* `ground_over` = asphalt pixels the ground mesh demonstrably paints (full frame
  vs `--no-ground`, same pose).

Two identical renders differ in **0 px** (the control), so every number is caused
by the camera move.

| camera | asphalt px | flip | buried | ground_over | ground px |
|---|---|---|---|---|---|
| `-1740.5,31,3960,80.5,-14` (bd cutting bend) | 104,652 | **0** | 0 | **0** | 261,939 |
| `-2250.5,48,3700,0,-14` (ba highway) | 60,988 | 6 (0.010%) | 0 | **0** | 42,412 |
| `-2250.5,48,3700,0,-60` (ba, top down) | 160,235 | 0 | 0 | 0 | 0 |
| `-2915,30,-4724,0,-10` (ap apron) | 557,097 | 552 (0.099%) | 0 | **0** | 42,879 |
| `-3020,60,-4625,0,-1` (start, grazing) | 353,314 | 6,809 (1.93%) | 0 | **6** | 75,222 |
| `-1953,50,-3111,0,-5` (al valley road) | 206,915 | 4 | 0 | **0** | 0 |

The ground mesh **never** covers an asphalt pixel (0 everywhere, 6 px of 353,314
at the worst grazing pose; the `buried` column is asphalt the *scene* never draws
at either pose, which is 0 at every pose but the two whose mask includes the road
mesh's silhouette from a low camera), and the residual `flip` is the asphalt silhouette
moving 0.4 px with the camera, concentrated exactly there: the 552 px of the
apron are its outline, and the grazing pose's 1.93% is the road edge crossing a
pixel.  With the ground switched off the same measurement gives the same
residual, i.e. **the ground mesh is not what moves the road**.  Two independent
hardware-free checks agree: the geometric gap under the asphalt never drops below
1.0 m (the pre-fix margin; 3.5 m after the clearance below) and the 24-bit depth
buffer resolves ~0.03 m at 400 m with the 2/1800 near/far planes, three orders of
magnitude finer.  What this does **not** say is which layer produced the reported
symptom when it was filed: the measurement rules out the asphalt-vs-ground sheet
z-fight on every pose probed, so the remaining candidates are the texture-sampling
shimmer of §4/`docs/road_filtering.md` and the ride itself (the truck used to
follow the terrain field under the asphalt, with a 3 m step where the road network
ends - both fixed by the road profile described above).

Two defensive changes were still made, because a 1 m margin at a grazing angle is
one bad driver away from a visible fight and both are free:

* `Renderer::profileMargin()` raises the clamp's clearance to **2.5 m** within
  25 m of a tile, fading out over 100 m (`1.0 + 2.5 = 3.5 m` at the tightest
  point, measured `min gap 3.500 m`; the median gap is unchanged at 8.05 m
  because the clamp only bites where the terrain was already close);
* the ground pass in `Renderer::draw()` gets `glPolygonOffset(1.0, 4.0)`
  (positive = away from the camera), so where the two surfaces are within a
  depth unit the authored tiles always win.  The tiles are drawn after the ground
  (`drawScene`), so the offset only ever decides a tie in the road's favour.

### Numbers (`--road-probe`, the whole road network)

`--road-probe [STEP]` samples every `.way` polyline (23,598 samples at 8 m, four
wheel contact points each) and ray casts the shipped B3D triangles downwards.
`--road-probe-site x,z[,r]` dumps the worst samples near a site, and
`--traffic-wheel-probe [SEC]` does the same on the **running simulation** at
10/25/40/64 cars.

```
road-probe: 23598 samples on the road network (8 m step, 4 wheel points each), 1331 skipped
road-probe: A graph height vs the ASPHALT under the wheels ... mean +0.000 m, median +0.000 m,
            p95 0.200 m, MAX 9.000 m, |e|>5 m: 13
road-probe: B asphalt-to-ground-mesh gap: min 3.500 m, median 8.050 m, mean 14.504 m;
            < 0.5 m: 0; ground above the asphalt: 0
traffic-wheel-probe (8 s): 83740 wheel samples, mean +0.004 m, median 0.000 m,
            p95 0.000 m, max |wheel - asphalt| 1.612 m, over5=0, over20=0
drive (player, 12 s, no traffic): wheel plane vs asphalt max 0.000 m over 1729 samples
```

`--road-probe` also reports the graph's own continuity from t=9.25 s at the aa
warehouse after the first (per-cell) profile was replaced by the exact one:

```
ground: road profile index: 2259 segments, 9853 points, 2440 buckets
drive: player vertical step per frame: max 0.133 m (the AI's road-profile limit is 0.50 m)
```

The test suite (`python3 tools/shot_test.py`) is **169 checks, all green**; eight
of them are new: the traffic wheel bound over the simulated densities, the
junction case, the player wheel plane, two `road_fight` poses (material flip and
ground overlap), the road/ground clearance, the graph-vs-asphalt control and the
`--terrain` control of the road-continuity measurement.

The player's number has an A/B control built in: `HT2_NO_ROAD_PROFILE=1` makes the
truck follow the terrain field again, i.e. the behaviour before this change.  Same
run, same binary, same route (`--drive 12 --traffic 0 --no-collision`):

| player wheel plane vs asphalt | before (`HT2_NO_ROAD_PROFILE=1`) | after |
|---|---|---|
| samples | 1,729 | 1,729 |
| mean | +0.962 m | **+0.000 m** |
| p95 | 4.850 m | **0.000 m** |
| max | **+8.235 m** | **+0.000 m** |
| vertical step per frame | 0.086 m | 0.133 m (rate-limited ride) |

The AI cars need no such control: `--traffic-wheel-probe 8` reads `Car::h`, the
height the graph hands to `drawTraffic`, and it is the asphalt to a 0.000 m
median (p95 0.000 m, max 1.612 m over 83,740 simulated wheel samples - the probe
is what the test suite runs).

Two further details of the profile are worth recording, because the first cut of
it (a per-cell average, and an exact lookup without a fallback) stepped:

* **The profile is exact, not a cell average.** `GroundMap::setRoadSegments()`
  indexes every `.way` segment in a 64-unit bucket grid and
  `roadProfileAt(x, z)` answers with the nearest point of the polyline,
  interpolated inside the segment.  The cell-averaged `road_` field alone stepped
  by up to 26 m between neighbouring 12-unit cells (843 steps > 1 m of 27,069
  cells) on the sparse stretches of the network.
* **It fades out where the network ends.** The `.way` data stops short of a few
  driveable tiles (measured at the aa warehouse, where the carriageway continues
  to z = -4575 but the network ends at z = -4615): the profile is blended into the
  terrain field over 32..80 m from the network, and off it the ride is
  rate-limited to 8 m/s so a single frame cannot drop the truck into the cut
  under an unmapped road.  Before that, the ride stepped 3.028 m in one frame
  there; now the worst step on the route is **0.133 m**.

The 13 samples with |e| > 5 m are all **two asphalt surfaces in the same column**
(a junction where the graph is on the lower carriageway while the ray cast lands
on the deck above, or the reverse): the median error over the whole network is
0.000 m and the p95 is 0.20 m - the road graph *is* the asphalt, to the
centimetre, wherever there is only one surface in the column.

## 9. The tiles' own asphalt: the surface where the `.way` network ends

§8 left one case open: the `.way` graph is the authored centreline and matches the
asphalt to a 0.000 m median, but it **stops short of a few driveable tiles** - the
aa warehouse carriageway runs to `z = -4575` while the network ends at
`z = -4615`.  There the terrain field took over, and at that spot the terrain mesh
is a cut 12 m above the carriageway (it is clamped to the tile ceiling), which
made the ride discontinuous.

### What it is

`src/world/RoadSurface.{h,cpp}`: an index over the **tiles' own carriageway
triangles**, used as the middle layer between the `.way` profile and the terrain.

* **Materials.**  Enumerated from the shipped data (a census of every
  `original/ENV/*.b3d` face, the same method as `tools/road_points.cpp`): the
  carriageway materials are `mat_road`, `mat_road0..4`, `mat_troad`,
  `mat_troad2..4`, `mat_tunnel_road`, `mat_mount_road`, `mat_crkroad2` (cracked
  asphalt) and `mat_roadgrn` (the road family's graded shoulder), plus
  `stos_asphalt` (the depot/apron surface).  **Rejected**: `mat_road_fence` (a
  vertical fence whose name contains "road"), `*_stolb*` (posts), `*_bridge*`,
  any triangle whose normal is more than 60 degrees from vertical, and any
  triangle that does not span at least two 2-unit cells (a lane is 12..24 units
  wide, a tile-sized sliver is not a surface).  Result: **57,028 road triangles,
  56,730 of them horizontal carriageway.**
* **Structure.**  The triangles are bucketed into a 32-unit 2D grid (287x335,
  ~1.7 triangles per bucket) and a point query intersects the triangles of its
  bucket - O(1), no rasterised grid.  A 1-unit heightfield over the island would
  be 10^8 cells (2.5 GB); the index is **6.43 MB and builds in 5.6 ms** at
  startup, on top of the 1.4 M-triangle ground mesh.  It is not lazy because
  there is nothing to defer at that size.
* **The height query** collects **every** surface in the column, not just the top
  one, and returns the one **closest to a reference height** (the height the
  vehicle is riding at).  That is what separates a viaduct deck from the
  carriageway under it, and it is the rule that fixes the 13 network samples of
  §8 that "saw" two asphalt surfaces.  A "not found" answer is **NaN**, never 0:
  the first cut returned 0 for "no surface", and the ride consumed that zero as a
  height - the truck stopped dead at the end of the tile asphalt with a 20 m
  "drop" under its front wheels.
* **Graph-node filter.**  23.7% of the `.way` VDAT points are more than 40 m from
  any *node* of their own polyline: they are the straight chords the format
  leaves across a curve, and the shipped data has chords with `y = 0` spanning
  tens of metres.  Those points are no longer usable as a surface (a chord far
  from its nodes is an interpolation artefact, not a road).
* **Horizontal cull.**  A surface whose column is close in height but whose
  triangle is tens of metres away horizontally (a ground-level asphalt quad
  picked up as the road under a viaduct at `y = 20`) is rejected: the query
  measures the horizontal distance to the nearest triangle edge as well.

### The layering (GroundMap::drivableY)

```
1. the `.way` profile        within 32 m of the network      (authored centreline)
2. the tiles' own asphalt    within 12 m horizontally, and the surface the
                              layer picked is within 20 m of the reference
                              (a cut slope can stand that far above the road)
3. the terrain field         everything else
```

Layer 1 wins on the network; layer 2 answers where the network has ended or runs
beside the carriageway.  Layers are entered through a distance fade (4..12 m of
the tile, and the terrain beyond), and the vehicle additionally rate-limits its
ride (8 m/s) so a genuine step cannot teleport it.

**The hand-over ramp is gradient-capped.**  From 32 m out the `.way` profile
blends into the terrain, but at most `0.5 m` of rise per metre (26.6 degrees,
under the vehicle's 3.5 m climb limit), reaching the terrain over the distance the
height difference actually needs.  Both extremes were measured, not guessed:

| variant | drive test (`--drive 30 --no-collision`) |
|---|---|
| hard cut at 32 m (no ramp) | **stuck at 42.1 m** - the apron terrain is 12 m above the profile there and the climb check refuses the step |
| the old ramp (32..80 m, base = the caller's own height) | stuck/wandering: the base moved with the vehicle, so the surface was path-dependent |
| **gradient-capped ramp (this one)** | **471 m in 30 s**, 76 km/h, wheel error 0.000 m, worst step 0.133 m |

### Numbers (`--road-probe`)

```
road-surface: 57028 road triangles (56730 horizontal carriageway) from
              [stos_asphalt, mat_road2, mat_road, mat_road0, mat_road3, mat_road1,
               mat_road4, mat_roadgrn, mat_crkroad2, mat_troad2, mat_troad4,
               mat_troad, mat_troad3, mat_troad0, mat_tunnel_road, mat_mount_road,
               mat_gravii_road]
road-surface: 287x335 buckets of 32 units, 96854 triangle slots (1.71/tri),
              245759 edge slots, 56730 triangles + 170190 edges, 6.43 MB, 5.6 ms
ground: road graph nodes: 2335 points (23.7%) more than 40 m from any node
road-probe: C tile asphalt (56730 triangles, 3 points each): 170190 points,
            125517 on the network and 44673 off it; 11682 multi-surface columns
road-probe: C off-network tile asphalt: 43209 points, 43201 answered by the tile
            layer, 8 with the wrong surface, 120 >5 cm off; |error| median 0.000 m,
            p95 0.000 m, MAX 16.057 m at (-2908, -4866)
            (the 16 m outlier is the aa/apron seam: the shipped tile data ends a
             1-km concrete slab abruptly - see §6 - not a layering error)
road-probe: C single-surface columns only: median 0.000 m, p95 0.250 m,
            MAX 16.057 m ...; on-network agreement p95 0.450 m
            (max 12.000 m, the two-tier junctions)
```

The metric walks **every carriageway triangle the tiles contain**, at three points
of each, and asks the layered surface there with the triangle's own height as the
reference: those 170,190 points are exactly the surface the `.way`-only profile
could not serve.  **43,201 of 43,209 off-network points are answered by the tile
layer** (8 exceptions and a single 16 m outlier at the aa/`ap` apron seam, where
the shipped tile data ends a 1 km concrete slab abruptly - `docs/terrain_road.md`
§6 already documented that slab as tile geometry, not terrain).

`--traffic-wheel-probe` reports which layer every simulated wheel point was on:
the AI cars are **81,101 `.way` / 1,472 tile / 0 terrain**, and the driven truck's
own route (started on the network and on the warehouse cut) is 194 `.way` /
126 terrain / 0 tile - the important number is the **0 terrain under the AI**,
which is the layer that used to sink the cars, and the 3,420 tile-answered
network columns, which are the ones where the `.way` chord and the deck disagree.

`--ride-probe x,z` prints all of this for one site.

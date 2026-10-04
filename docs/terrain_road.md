# The road breaking apart on bends ("дорога разваливается на поворотах")

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

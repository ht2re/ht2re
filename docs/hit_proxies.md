## 8. The authored collision proxies (`hit_obj_*` / `hit_road_*`) — decoded 2026-10-16

`B3D::buildFrom` deliberately skips every top-level container named `hit_*`
(they are never drawn).  They are the engine's own collision data, and they are
**not meshes**: each container holds small records describing the *shape* of the
thing it belongs to.  Decoded with `tools/out/hit_proxy_probe` (see 9):

| container | contents |
|---|---|
| `hit_obj_<tile>_<NNN>` | the object's collision body (footprint polygon and/or face planes) |
| `hit_road_<tile>_<NNN>` | the same object's road-side record (vertical extent, a plane, or a link) |
| `obj_<tile>_<NNN>` | the **visible** object the two above describe (same tile+id) |

Child record types, with their field layout (all fields are 4-byte):

| subtag | n | layout | meaning |
|---|---|---|---|
| 20 | 1537 | 8 header dwords, then `count` x 3 floats `(x, z, y)`; **count = header[4]** | a **footprint polygon** of the object (header[0..3], [5..7] are 0 in every sample) |
| 9 | 1506 | params[4..6] = normal `(x, z, y)`, params[7] = `d`; equation `n.x*x + n.z*z + n.y*y + d = 0` | an oriented **plane** = one **face** of the object (axis-aligned ones appear too, e.g. `(0,0,-1, 8.5)`) |
| 12 | 222 | params[6] = 1.0, **params[7] = -y_min**, params[8] = 1, params[9] = 1 or 10 | the object's **vertical extent** (bottom) |
| 18 | 86 | params[4..6] = 12-byte **anchor** name, params[12..14] = 12-byte **container** name | a **link** to a real authored container |
| 40 | 1 | 32 params, params[4..8] = `"$$CollisionOfTerrain"`, params[22] = 10 | terrain-collision marker |
| 23 | 377 | 28-byte payload, different header | second shape type (not decoded yet) |
| 10 / 21 | 223 / 18 | as elsewhere in B3D | LOD record / script key group |

### Evidence (not guesses)

* **subtag 20**: `hit_obj_ap_048` (the forest patch that crossed the apron road)
  carries 4 points `(-2900,-4635) (-2940,-4595) (-3060,-4595) (-3100,-4635)`, and
  the visible `obj_ap_048` bbox is `x[-3100..-2900] z[-4695..-4595]` - the polygon
  *is* the object's southern band.  Statistically over the whole ENV set
  (`hit_proxy_probe --verify`, 261 polygons whose object resolves):
  **99.2 % touch the object's bounding-box edge and 84.3 % lie entirely inside it**
  (8 m tolerance) - the field order `(x, z, y)` and `count = header[4]` are right.
* **subtag 9**: `hit_obj_ab_012` carries the plane `(0.7071, 0.7071, 0)` with
  `d = +1237.437`, and `obj_ab_012` sits at `x[630..670] z[-2420..-2380]`:
  `0.7071*650 + 0.7071*(-2400) + 1237.437 = 0.0` - the plane passes exactly through
  the object's centre, i.e. the normal is stored `(x, z, y)`.  `hit_obj_ap_048`'s
  sibling plane is `(0, 1, 0)` with `d = 4647` -> `z = -4647`, inside the object's
  `z[-4695..-4595]`.  Statistically: **86.0 % of the 315 checkable planes are
  tangent to the object's own bounding box** (13.7 % cut through it, 0.3 % far) -
  so a plane is a *face* of the object, not a map-wide wall.
* **subtag 12**: `hit_road_ab_012` gives `params[7] = -240.0` and the object's
  `y_min = 240.0`; `hit_road_ap_048` gives `-20.0` for `y_min = 20.0`.  Two exact
  matches, and no other candidate field fits.
* **subtag 18**: the inline 12-byte names decode to real containers -
  `link_hit_store_ap` -> anchor `StoreAPSpace`, container **`store_ap_hit`**;
  `link_sto_hit_bot_aa` -> container **`STOS_floor`**; and across the set the
  targets are `STOS_walls` (19), `STOS_floor` (14), `benzo_hit` (11),
  `benzo_bottom` (10), `store_*_hit`/`store_*_hi`, `Combinat_col`, `Airport_hit`
  (27 distinct names).  Every one of those names exists in its tile's name table.

### Coverage (why this replaces the visible-mesh rule)

| | count |
|---|---|
| visible object containers `obj_<tile>_<NNN>` | **857** |
| `hit_obj_<tile>_<NNN>` collision bodies | **853** (99.5 % of the objects) |
| `hit_road_<tile>_<NNN>` road-side records | 1589 (~2 per object) |

So the authored collision covers practically the whole world, which is why
switching to it can *replace* the visible-mesh heuristic in §5 instead of adding
to it: every object with a proxy stops contributing its rendered geometry.

### What the port still has to do

The proxies are the *authored* collision, so using them replaces the guesswork in
§5: a forest patch then blocks exactly its authored band, a fence exactly its
footprint, and the extra metre of "conservative cell marking" disappears.  The
open questions before wiring it in are (a) how a plane is bounded (it is a face,
so the footprint polygon or the object bbox must clip it - subtag 12 gives the
bottom, but no top has been found yet) and (b) subtag 23's second shape type.
`hit_road_*` records exist for objects that are *not* in the same tile, so the
lookup has to stay name-based rather than id-based.

## 9. Tools

```bash
c++ -O2 -std=c++17 -Isrc tools/hit_proxy_probe.cpp src/world/B3D.cpp -o tools/out/hit_proxy_probe
tools/out/hit_proxy_probe original/ENV --summary          # the vocabulary, per subtag
tools/out/hit_proxy_probe original/ENV --lines            # one line per proxy (aggregate)
tools/out/hit_proxy_probe original/ENV --verify           # statistical field validation
tools/out/hit_proxy_probe original/ENV ap.b3d _048        # one object, fully dumped
```

## 10. Who reads them in king.exe (traced 2026-10-16)

The executable names its own readers, and the name scheme falls out of them:

| diagnostic string | VA | referenced at | what it does |
|---|---|---|---|
| `get_road_collision()` | 0x673cc8 | **0x4e5cce** | `sprintf(buf, "road%s", nodeName + 4)`, then looks the container up |
| `get_obj_collision()` | 0x673d2c | **0x4e5e22** | `sprintf(buf, "obj%s", nodeName + 4)` |
| `get_sort_collision()` | 0x673d60 | **0x4e5f29** | `sprintf(buf, "sort%s", nodeName + 4)` |
| `CollisionCenter` / `CollisionRadius` | 0x674e44 / 0x674e34 | 0x5090c3/0x50ab9b, 0x5090e5/0x50abbf | the collision object's centre and radius |
| `CollisionPlane%d` | 0x674d34 | **0x50990b / 0x50b0fd** | the collision object's **planes** (numbered) |
| `CollisionKey` | 0x678450 | 0x52adf2 / 0x540d06 | the collision object's key |
| `hit_%s` | 0x678460 | 0x52add2 / 0x540cd6 | builds the `hit_*` container name |
| `perform_wall_collision` | 0x67d1b8 | 0x57e514 / 0x57e52b | the wall collision routine |
| `hit_flags_object` | 0x679ef8 | 0x543d75 | the flags object of a hit container |
| `$$CollisionOfTerrain` | 0x67c268 | 0x5726a2 | the terrain-collision marker (subtag 40) |

One object's vocabulary is therefore:

```
obj_<tile>_<NNN>       visible object            get_obj_collision  -> "obj"  + tail
road_<tile>_<NNN>      its road-side companion   get_road_collision -> "road" + tail
sort_<tile>_<NNN>      a sorting companion       get_sort_collision -> "sort" + tail (none in ENV)
hit_obj_<tile>_<NNN>   the object's collision shape
hit_road_<tile>_<NNN>  the road companion's collision shape
```

Every `<prefix>` is exactly 4 characters - which is what the `len < 4` guard checks.
`road_<tile>_<NNN>` containers really exist (15 in `ap.b3d`); `sort_*` do not.

**Consequence: the polygons are not the solid volume.**  The engine gives its
collision objects a **centre, a radius and numbered planes** (`CollisionPlane%d`)
and applies them through the *wall* path (`perform_wall_collision`) - i.e. the solid
is a **convex volume bounded by the subtag-9 planes**, and collision is a
plane-crossing test, not an occupancy grid.  The subtag-20 polygon is something
else (a trigger/hit polygon).

### The experiment that settled it

`--proxy-collision` builds the solids from the polygons instead of the visible
meshes (`Collision::build(meshes, proxies)`), so the wrong primitive could be
measured instead of argued about:

| | solid cells | apron | **start** | STOS wall | repair bay |
|---|---|---|---|---|---|
| default (visible meshes) | 927 081 | free | **free** | blocked | blocked |
| `--proxy-collision` | 4 337 009 | free | **BLOCKED** | blocked | blocked |

Five of six reference points agree, the start does not: `GAME.CNF`'s start
`(-3020,-4625)` lies inside `obj_ap_048`'s polygon (`x[-3100..-2900] z[-4635..-4595]`),
which the shipped game cannot have.  491 footprints painted 3.59 M cells and 6 757
submeshes were skipped as "covered" - the footprints are far coarser than the
rendered geometry.  The flag therefore stays **off by default**.

### The planes are thin walls, not volumes (measured 2026-10-16)

Two more facts close the model:

* **Planes per proxy:** 1129 proxies carry exactly **one** plane, 130 carry two, only
  32 carry three or more (7, 8 max).  A single plane cannot bound a volume, so the
  subtag-9 record is a *surface* - a wall the car must not cross - not a face of a
  convex body.
* **`perform_wall_collision` (0x57e430)** does exactly that: it works on the plane
  **normal** `[ebx], [ebx+4], [ebx+8]`, builds a tangent basis from the largest
  component (`fabs` + the two `fmul`s at 0x57e437..0x57e44d), and intersects it with
  the **car's motion vector** `[ebp+0xce4], [ebp+0xce8], [ebp+0xcec]` - a
  plane-crossing test, i.e. an infinitely thin wall.
* **Agreement with the port's blocking geometry** (`road_block_audit`, which now also
  loads the proxies): of the 205 `.way` samples that are on authored asphalt *and*
  inside blocking geometry, **116 (56.6 %) lie within 2 m of an authored wall plane**
  (1506 planes in total).  So more than half of what looked like "invisible walls on
  the road" are **authored** walls - the original blocks there too, with a
  zero-thickness plane instead of the port's ~1 m inflated cell.  The remaining 89
  samples are not explained by a plane (real obstacles without one, or port
  artifacts) and stay on the open list.

Consequence for the port: the grid cannot represent a zero-thickness wall without
inflating it, so the faithful step is a **plane wall list** (normal + offset, bounded
by the object's `CollisionCenter`/`CollisionRadius` and the subtag-12 bottom) with a
plane-crossing test in the vehicle sweep - `perform_wall_collision` above.  The
subtag-20 polygons are *not* that: 1545 proxies carry none, the ones that do carry
2-11 records each, and the GAME.CNF start lies inside one, so they describe a
*drivable/occupied area* (consistent with `get_road_collision()`), not an obstacle.


Collision is **planes + a bounded volume + wall/sweep resolution**, not a bit grid.
Porting it means: read the subtag-9 planes of each `hit_*` container, bound them by
the object's centre/radius and the subtag-12 bottom, and resolve the car against
those planes - a walk of the loader at `0x4e5c80..0x4e6000` (and its caller, which
passes the object node in) plus `perform_wall_collision` (0x57e4f0 area).  Until
then the visible-mesh grid stays the default and its documented ~1 m inflation
(§6 of collision.md) is the known cost.


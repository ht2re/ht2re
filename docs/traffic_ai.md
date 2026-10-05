# AI traffic (`src/game/Traffic.{h,cpp}`, `src/game/Collision.{h,cpp}`)

Road-network AI traffic for the ENV world: cars follow the real `.way` road
polylines, keep to the right-hand lane, route to the game's own named RNOD
waypoints, take junctions one at a time, queue behind each other and behind the
player, and resolve contacts as capsules that deal damage to both sides.

Everything below is reproducible offline:

```bash
# build the game
cmake -B build && cmake --build build -j

# the *world AI* report: runs the real simulation headlessly for N seconds with
# the player parked in a lane and prints one machine-readable RESULT line
./build/ht2re --scene world --spawn -3020,-4625,0 --traffic 16 --traffic-report 25
./build/ht2re --scene world --spawn -3020,-4625,0 --traffic 40 --traffic-report 25
./build/ht2re --scene world --spawn -3020,-4625,0 --traffic 40 --traffic-report 25 \
    --traffic-report-oncoming          # park the player against the flow
./build/ht2re --scene world --spawn -3020,-4625,0 --traffic 40 --traffic-report 25 \
    --camera -3019,55,-4647,0,-78 --screenshot --out shots/traffic_ai_junction.bmp

# standalone harness (same metrics, builds against the old module too)
c++ -std=c++17 -O2 -Isrc tools/traffic_report.cpp src/game/Traffic.cpp \
    src/game/Collision.cpp src/world/Way.cpp -o build/traffic_report
./build/traffic_report original/ENV 30 40
```

Contents: [1 graph](#1-graph-from-the-road-network) ·
[2 lane path](#2-the-lane-path-the-convention) ·
[3 junctions](#3-junction-resolution-and-reservation) ·
[4 routing](#4-routing) · [5 spacing](#5-spacing) ·
[6 height](#6-height-the-road-profile) · [7 collisions](#7-collisions-car-bodies-contacts-damage) ·
[8 player](#8-the-player) · [9 population](#9-population) ·
[10 report](#10-the-traffic-report) · [11 damage skins](#11-why-every-ai-car-looked-damaged) ·
[12 numbers](#12-measured-numbers) · [13 API](#13-api)

---

## 1. Graph from the road network

`Traffic::build(segs, nodes)` consumes **all** `Way::Seg` of every
`original/ENV/*.way` (39 files, 2259 RSEG) plus `name -> Way::Node`:

* every RSEG polyline is split into **consecutive-point edges**, so the graph
  keeps the real road shape instead of chords;
* the two endpoints of every edge are **welded** by rounding `x/y/z` to 0.01
  (`llround(v*100)` in an `std::array<long long,3>` key). The shipped RSEG end
  points coincide exactly, so this is lossless; 8 degenerate pairs (< 0.5 m) are
  dropped;
* **duplicate edges are welded too**: 422 vertex pairs carry two or more
  identical edges (the same road is drawn twice in the shipped data, 1134 of the
  7586 edges). They are *not* two roads - a car on each of the "two" edges sat in
  the same lane at the same coordinates, so they interpenetrated at every
  junction and doubled the junction degree. The widest width and the first
  name/kind win;
* each `Edge` owns its polyline (`poly_`) plus the RSEG **width** (`WDTH`,
  clamped to 2.5..40 m, 8 m fallback), **attr** (`ATTR` road kind) and **name**
  (`RTEN`);
* adjacency is `verts_[v].edges` (incident edge ids), and every edge is
  bidirectional: a car travels with `dir = +1` (`a -> b`) or `-1` (`b -> a`);
* heights: `Way::Pos` stores `x, y = height, z`; `sample()` interpolates the
  height along the polyline.

Graph size after welding the duplicates: **6172 vertices / 6447 edges**
(was 6172 / 7586).

## 2. The lane path (the convention)

Travel direction is the polyline tangent `f = (fx, fz)` (yaw = `atan2(fx, fz)`,
the same convention as `Vehicle`/`Renderer::rotY`). Right-hand traffic puts the
car at

```
right = (fz, -fx)                  // original D3D orientation, mirrored GL projection
pos   = centreline + right(heading) * (lane / cos(heading - tangent) + shift)
lane  = min(laneOffset, width/3)    // laneOffset() defaults to 2 m
```

Oncoming cars use the opposite `dir`, hence the opposite `right` vector, so they
pass on their own side (with `dir = +1` the car is at `+x` when driving towards
`+z`).

The cosine correction keeps the perpendicular lane distance while its normal
rotates through a bend (capped at 2x for sharp turns). This is a bounded miter
offset, not a spline reconstruction of the authored asphalt or its markings.
`tools/traffic_lane_test.cpp` checks both travel directions, the actual mirrored
projection and continuity through left/right 45- and 90-degree bends.

`shift` is the *lateral pushback* a contact leaves behind (clamped to ±1.2 m,
relaxed at 2.5 /s once the car is free again). It is the only way a car ever
leaves its own lane, and it is what lets two bodies that touch separate without
being driven off the road.

## 3. Junction resolution and reservation

The graph vertex is the junction. For a car arriving on edge `e` at vertex `v`
the outgoing edge is chosen in this priority order (`Traffic::chooseNext`):

0. **the next hop of the destination's shortest-path field** (see 4), when it is
   ready and legal;
1. otherwise an edge that **shortens the straight line** to the destination (the
   literal rule of the brief), the reducing candidate with the smallest such
   distance;
2. otherwise the **straightest forward continuation**.

A candidate is illegal when it is the incoming edge, an edge back to the vertex
we came from, or a heading reversal (`dot < -0.85`, ~148°); the module counts
every illegal pick in `Stats::uturns`, which stays **0 by construction**.

**Rounded corner.** The heading used for the body yaw *and* for the lane offset
is blended over a window `B = min(6 m, len/2)` at both ends of every edge
(`Traffic::blendWeights`):

```
wPrev = 0.5 * (1 - s/B)            // towards the edge we came from
wNext = 0.5 * (1 - (len-s)/B)      // towards the edge we are going to take
heading = cur + wPrev*short(heading(prevEdge) - cur) + wNext*short(heading(nextEdge) - cur)
```

Because `B <= len/2` the two windows never overlap and the value **at the
vertex is exactly the midpoint of the two road headings** - which is also what
the edge on the other side of the vertex computes. The lane offset therefore
rotates continuously through the junction instead of stepping sideways by
`2*lane*sin(turn/2)` (up to 2.8 m on a 90° turn) in one frame, and the car
sweeps a rounded corner of roughly `2*B` metres. `Stats::jumps` counts the
frames where a pose moved more than `speed*dt + 0.5 m`; at the default traffic
it is **0**.

The outgoing edge is planned when the car *enters* an edge (`planTurn`), so the
approach blend knows where it is going. If the plan turns out to be occupied the
car re-plans (skipping that edge) and otherwise waits.

**Reservation: one car at a time in a junction.** A car *occupies* vertex `v`
while the nearest point of its body is still inside a conflict disc of
`kVertexZone = 5 m` around `v`
(`dist(centre, v) < zone + length/2`). A car that is about to cross waits with
its *bumper* `kVertexMargin = 1 m` outside that disc:

```
hold = max(0, len - zone - margin - length/2)
```

Two waiting cars therefore never count as occupying the junction and cannot
block each other; on an edge shorter than the hold distance the car waits at its
entry. The approach speed is capped by the stopping distance to the hold point.
Cars already inside a block keep their position (they are not snapped
backwards); the following traffic waits outside.

## 4. Routing

Each car gets a destination waypoint (`Car::dest`, `Car::destVertex`) and drives
to it. Two mechanisms, in priority order:

1. **Route field (default, exact).** A reverse Dijkstra from the destination
   fills `dist[]` and `next[]` (the edge to take at every vertex). Fields are
   cached (24-slot LRU) and computed lazily under a budget of 6 Dijkstras per
   `update()`, so a spawn burst never stalls a frame.
2. **Straight-line rule** (rule 1 of section 3) whenever no field is ready yet.
   `setPathfinding(false)` runs it on its own.

Reaching the destination (within `destinationRadius()`, 30 m, or on arrival at
its vertex) immediately picks a new one: a random resolved waypoint 300 m ..
`min(1800 m, 0.8 * activeRadius)` away, preferring vertices with degree ≥ 2. A
destination older than 240 s is dropped and replaced.

A genuine dead end (no legal continuation at all) is the *only* place a car
turns around: beyond `kDeadEndDist = 140 m` from the player it U-turns in place
(animated by the same heading blend, counted in `Stats::deadEndTurns`, never in
`uturns`); inside that distance it is recycled out of sight
(`Stats::recycles`).

## 5. Spacing

Per tick, cars are sorted per `(edge, dir, s)`; the car directly ahead is the
leader. Following uses a desired headway and a **stopping-distance cap**, so a
follower can always stop inside the free gap instead of overshooting when the
leader brakes:

```
gap    = leader.s - car.s - (len_car + len_leader)/2      // bumper to bumper
want   = 3.0 m + 0.7 s * car.speed
target = min(cruise, leader.speed * gap/want, leader.speed + gap)
target = min(target, sqrt(2 * 7 m/s^2 * (gap - 2 m)))      // stopCap()
speed += clamp(target - speed, -7 dt, +3 dt)
```

A hard post-pass clamps the cars of one lane group: `back.s <= front.s -
(len_back+len_front)/2 - 2 m` (front first), and it runs twice per frame - once
after the contacts and once after the stream relocations. Spawn and relocation
refuse positions closer than 20 m (`s`) or 12 m (world space) to another car and
never within 60 m of the car's own previous position or in the player's view.

## 6. Height (the road profile)

`sample()` walks the edge polyline's cumulative arclengths and interpolates
`x, h, z`; `Car::h` is the road height. At a junction the height is blended with
the **same weights as the heading**:

```
h = h(current) + wPrev*(h(prevEdge end) - h) + wNext*(h(nextEdge start) - h)
```

so a car crossing between two roads that sit at different heights climbs the
difference over the blend window (a ramp) instead of stepping up or dropping
through the map. `Stats::heightJumps`/`maxHeightStep` count vertical pops > 0.5 m
per frame (streaming relocations are excluded - the height continuity is measured
on the frames that stay put). The shipped `.way` data is continuous to within
0.00 m (median) between consecutive polyline points, but two *different* RSEGs
that meet at a vertex need not agree on the height, and the raw spawn/relocation
could place a car at a completely different height: the blend makes the profile
continuous by construction, and a relocated car resets its own height reference.
Measured: 0 steps > 0.5 m, max 0.05 m per frame at every density.

`c.h` is also the height the car is **drawn** at.  `App::drawTraffic` used to
replace it with `GroundMap::heightAt()` whenever the two were within 4 m; that
map is a *terrain* field which, under a carriageway, sits several metres below the
asphalt (docs/terrain_road.md §8), so the substitution was the "cars fly / sink"
complaint.  It now keeps the graph height and only falls back to the map's **road
profile** (`GroundMap::surfaceAt()`, the `.way` points averaged per cell) when the
graph carries no height at all.  Measured with `--traffic-wheel-probe 8` (the real
simulation at 10/25/40/64 cars, each car's four wheel contact points ray cast
against the authored asphalt): 83,740 wheel samples, mean +0.004 m, median 0.000 m,
p95 0.000 m, max 1.612 m, none over 5 m.  The player's truck got the same
treatment: `Vehicle` follows `GroundMap::roadProfileAt()`, the exact `.way`
profile, where it is on a road (measured max 0.000 m over a driven route and a
worst vertical step of 0.133 m per frame; with `HT2_NO_ROAD_PROFILE=1`, i.e. the
old terrain-field ground following, the same route measures a mean of +0.962 m,
a p95 of 4.850 m and a max of 8.235 m).

## 7. Collisions (car bodies, contacts, damage)

A car body is a **capsule**: a segment of `length - 2*radius` along the heading
with `radius` around it (`Collision::Body`, `blockedBody`, `resolveBody`,
`bodyOverlap`). The app fills `Traffic::setBody(model, length, radius)` from each
model's bounding box in `COMMON/TRUCKS.B3D` (e.g. Zil 6.92 x 3.23 m, BmwM5
4.94 x 2.45 m, Fiat 4.53 x 2.07 m); the defaults (4.5 x 2.2 m) are used for a
model that was never described.

Every `update()` runs one contact pass (`resolveContacts`):

* pairs are found by an x-sorted sweep (`kPairReach = 16 m`), so no per-frame
  allocation and no O(n²) blow-up;
* for every overlapping pair the two bodies are separated along their **own
  paths**: the world displacement `-n*d / +n*d` is projected onto the car's
  tangent (moves `s`) and onto its lateral axis (moves `shift`). The pushback is
  capped at 0.6 m per frame, so a deep overlap is shoved apart over a few frames
  rather than teleporting;
* the impulse along the contact normal is `(1 + 0.15) * closing / 2`, applied to
  both cars' speeds;
* **damage** is `min(0.30, 0.028 * closing speed)` for every contact that
  closes faster than 0.4 m/s - the same value is dealt to both cars and the
  numbers are summed in `Stats::damageDealt`; a car's damage caps its cruise
  speed and switches it to the dented skin (section 11);
* contacts between cars standing on *different* edges are counted as
  `junctionHits` (crossing traffic); same-edge contacts are rear-end shunts;
* `Stats::minGap` tracks the closest surface-to-surface distance seen,
  `overlapFrames` the pair-frames in which the bodies interpenetrated.

Oncoming traffic on a road too narrow for the two bodies (and cars on two
*different* edges that the shipped data draws on top of each other at a shallow
angle) are let through instead of being shoved: no impulse, no damage. The
alternative is two trucks deadlocked in the middle of a one-lane street forever.

## 8. The player

`Traffic::setPlayer(Player)` is refreshed by `App` every frame (position, yaw,
velocity, body radius/length of the player's truck, 12.04 x 3.23 m for the ZilR).
The player is then

* a **solid leader**: a car that finds the truck in its path within 30 m caps its
  speed by the same stopping-distance rule, so it brakes and stops behind it
  (`queued` in the report counts exactly those cars);
* a **collidable body**: the same capsule contact pass resolves car-vs-player,
  accumulating the pushback the player has to take (`PlayerHit::pushX/pushZ`),
  the damage to both sides and the strongest closing speed
  (`PlayerHit::impact`). `App::applyTrafficHits()` applies the nudge and the
  damage and plays `snd\crash9.wav` above `crashSpeed_`, exactly like a wall
  impact.

`--drive SECONDS` (full throttle into the traffic) exercises this end to end:
`traffic: car impact 39 km/h, player damage 0.30, car damage 0.30`, 3 car-vs-player
contacts and 4 overlap pair-frames in 30 s (the bodies separate again).

## 9. Population

* `spawn(px, pz, radius, count)` - candidate edges are those whose distance to
  `(px, pz)` is `<= radius`; cars get a deterministic random `s`, a direction,
  cruise 11..17 m/s, a model index and a fresh destination.
* `update(dt, px, pz, radius)` - advances everyone; a car further than `radius`
  from the player is relocated to a random free spot on an edge near the player
  (`Stats::respawns`). Relocations are never inside the player's view cone and
  never within 60 m of the car's own previous position, so a car cannot visibly
  pop in or out.
* **Stall / gridlock recovery** - a car below 0.4 m/s for 3 s (10 s while queued
  or yielding at a junction) is first re-routed and, if that fails twice,
  re-spawned near the player. A parked queue is never counted as stuck, but a jam
  that does not move *is* broken up.

## 10. The traffic report

`--traffic-report SECONDS` runs the real module headlessly (60 Hz) with the
player parked **in the lane of a long edge** (the raw spawn point sits beside the
lane line; `--traffic-report-oncoming` parks it against the flow) and prints:

```
traffic-report: RESULT cars=40 mean=8.49 uturns=0 deadend=0 offroad=0 frames=59960
  jumps=2 maxjump=1.88 hjumps=0 maxhstep=0.05 contacts=12 carcontacts=12
  playercontacts=0 junctionhits=12 overlaps=37 mingap=-0.80 damage=1.783
  playerdamage=0.000 queued=3 arrivals=0 respawns=18 recycles=0
```

| field | meaning |
|---|---|
| `mean` | metres driven / car-second |
| `uturns` | illegal direction reversals (invariant: 0) |
| `deadend` | legal turn-arounds at a genuine dead end |
| `offroad` | car-frames further than `offRoadLimit()` (2 m) from the lane polyline |
| `jumps` / `maxjump` | poses that moved more than `speed*dt + 0.5 m` in one frame |
| `hjumps` / `maxhstep` | height steps > 0.5 m in one frame (flying / sinking) |
| `contacts` | contact events (a pair closing faster than 0.4 m/s), split into car-vs-car and car-vs-player |
| `junctionhits` | contacts between cars on different edges (crossing traffic) |
| `overlaps` | pair-frames in which two bodies interpenetrated |
| `mingap` | closest surface-to-surface distance over the whole run |
| `damage` / `playerdamage` | damage dealt to the AI cars / to the player |
| `queued` | cars stopped in front of the parked player, in its lane |

Adding `--screenshot --out FILE` renders the *simulated* frame (the picture is
taken after the N seconds of AI), which is how `shots/traffic_ai_junction.bmp`
and `shots/traffic_ai_top.bmp` were made.

`tools/traffic_report.cpp` is the standalone harness; it measures the same
numbers from the public API (plus a breakdown of the overlapping pairs) and also
builds against the pre-change module with `-DHT2_TRAFFIC_OLD`.

## 11. Why every AI car looked damaged

Damage in `COMMON/TRUCKS.B3D` is a **material swap**, not a different container:
every body panel exists twice inside one vehicle container - the intact skin
(`Fiat_all1`, `BMW_all1`, `volga_all2`, ...) and the dented one
(`Fiat_alld1`, `BMW_alld1`, ...) - plus damaged-only parts such as
`common_BackLightsDmg`. Measured on the shipped data (built submeshes/triangles
per skin):

| model | intact skin | dented skin |
|---|---|---|
| Fiat | `Fiat_all1` 126 / 1847 | `Fiat_alld1` 306 / 4360 |
| BmwM5 | `BMW_all1` 131 | `BMW_alld1` 264 |
| Volga | `volga_all1` 312 | `volga_alld1` 240 |
| Gazelle | `Gazelle_all1` 120 | `Gazelle_alld1` 273 |
| Zil | parts only (`Zil_kabine1`/`kapot1`/`parts1`) | `common_BackLightsDmg` 24 |

`B3D::buildNamedScene()` includes **everything**, so every car drew its intact
*and* its dented skin on top of each other and looked pre-wrecked (20.9% of the
pixels of a close-up car changed when the split was introduced). `App` now
splits the materials (`damagedMaterial()`: `alld`, `damage`, `dmg`) and uploads
**two render batches per AI model**: common parts + intact skin, and common parts
+ dented skin. `Car::damage >= 0.35` selects the dented batch, so an undamaged car
is intact and a car that has taken contacts shows the dented skin - the damage
state only ever changes through the contact resolution of section 7. Parked cars
and the player's truck are built intact (the Zil's *progressive* damage parts
`Z_DAMAGE1..5` are not wired to the model yet). The `variant` field of
`evehicles.sch`/`etrashboxes.sch` is the paint/trim variant, not damage.

## 11b. Parked cars / roadblocks and the batch bounding sphere

Parked vehicles (`App::spawnParkedVehicles`, the `parking` forms of the
`mod_*.sch` scripts) and ambush roadblocks (`App::updateAmbushes`) do not live in
a B3D file of their own: each one is a copy of a `COMMON/TRUCKS.B3D` container
transformed into world space and appended to `worldMeshes_`. The renderer batches
the world by `(1024 m chunk, texture)` and culls a whole batch by the bounding
sphere built from `SubMesh::center`/`radius`, so the **center has to travel with
the geometry**:

```cpp
const float clx = t.center[0], clz = t.center[1];      // local values!
t.center[0] = px + clx * cs + clz * sn;
t.center[1] = pz - clx * sn + clz * cs;
t.center[2] = ph + yoff + t.center[2];
```

Reading the already-rotated `center[0]` when computing `center[1]` (the old code)
put the sphere thousands of metres away with a 3 m radius, which culled the car
away entirely - invisible parked cars and roadblocks even though their
triangles were uploaded and the "N of M submeshes drawn" counter included them.
Leaving the center at its local value (the ambush path at one point) puts the
sphere at the world origin with the same effect. Both paths are fixed.

## 12. Measured numbers

`tools/traffic_report` (real data, seed `0x51ED2701`, 60 Hz, **60 s**, the player
parked in the lane of edge 2619 at (-3019, -4647), the same pose for both
builds), **before** = the module as it was (`-DHT2_TRAFFIC_OLD`), **after** =
this change:

| metric | before (16) | after (16) | before (40) | after (40) |
|---|---|---|---|---|
| mean speed | 13.88 m/s | 10.34 m/s | 12.67 m/s | 7.16 m/s |
| U-turns | 0 | 0 | 0 | 0 |
| off-road frames | 0 | 0 | 0 | 0 |
| pose jumps (max) | 173 (2.49 m) | **1** (1.02 m) | 391 (2.69 m) | 157 (7.09 m) |
| height steps > 0.5 m (max) | 0 (0.00 m) | 0 (0.05 m) | 0 (0.00 m) | 0 (0.05 m) |
| overlap pair-frames | 80 | **0** | 1925 | 124 |
| min inter-car gap | -2.54 m | **+0.69 m** | -2.60 m | -0.96 m |
| contacts / damage dealt | 5 / 0.0 | 0 / 16.9 | 31 / 0.0 | 9 / 10.9 |
| damage to the player | 0.0 | 0.180 | 0.0 | 0.135 |
| cars queued behind the player | 0 | 0 | 0 | 3 |

The 40-car run is a *jam*: a 12 m truck parked in one lane of a two-lane road
blocks that direction, and over 60 s the queue backs up into the junctions
(hence the residual 124 overlap pair-frames of 0.9 m and the 7 m queue
re-spacing jumps).  The default traffic and the 25 s runs are clean.

`--traffic-report 25` (the game's own world, 5 models, the same parked player):

| metric | 16 cars | 40 cars |
|---|---|---|
| frames (car-frames) | 23984 | 59960 |
| mean speed | 9.55 m/s (34 km/h) | 8.49 m/s (31 km/h) |
| U-turns / dead-end turn-arounds | 0 / 0 | 0 / 0 |
| off-road frames | 0 | 0 |
| pose jumps (max) | 1 (0.77 m) | 2 (1.88 m) |
| height steps > 0.5 m (max) | 0 (0.05 m) | 0 (0.05 m) |
| contacts (car-car / player) | 116 / 0 | 12 / 0 |
| junction hits | 116 | 12 |
| overlap pair-frames | 169 | 37 |
| min inter-car gap | -0.05 m | -0.80 m |
| damage dealt | 5.45 | 1.78 |
| cars queued behind the player | 0 | 3 |

The 116 "contacts" at 16 cars are the real car bodies (up to 6.9 x 3.2 m, from
the model bounding boxes) brushing each other in a junction that the road data
draws as two overlapping carriageways: the surface gap never goes below -0.05 m,
i.e. they touch, they do not interpenetrate.  With the default bodies (4.5 x 2.2 m
- the harness without `setBody`) the same run has 0 contacts and 0 overlaps.

`--drive 30` into the traffic (head-on impact): `car impact 39 km/h, player
damage 0.30, car damage 0.30`, 3 car-vs-player contacts, 4 overlap pair-frames,
damage dealt 0.471.

`python3 tools/shot_test.py` carries 11 checks for this area (lane adherence,
U-turns, pose continuity, height continuity, interpenetration, contact damage,
the queue behind a stopped player, the head-on impact and the report numbers),
all green.

**Known limits.** (a) Saturated traffic (60+ cars in a 900 m bubble, or a truck
parked in the middle of a 15 m block for a minute) still produces a few shallow
overlaps (0.1-2.6 m) where the data draws two different roads within a lane of
each other; the pair breakdown is printed by `tools/traffic_report`. (b) AI cars
do not change lanes: a blocked lane is queued, not overtaken. (c) A queue that has backed up into a junction can re-space by up to
~7 m in one frame when the gridlock breaker fires.  (d) The road data is 2D - a viaduct and the road under it are the same `(x, z)`, so the height
blend of section 6 follows the *graph* profile, not the bridge deck above.
(e) `tools/traffic_sim.py` (the Python mirror) still models the old graph and
must not be used as the reference any more.

## 13. API

```cpp
// graph + waypoints
void   build(const std::vector<Way::Seg>&, const std::map<std::string, Way::Node>&);
void   clear();                  bool empty() const;
size_t vertices() const;         size_t edges() const;
size_t waypoints() const;        size_t waypointNames() const;
int    waypointVertex(size_t i) const;
const std::string& waypointName(size_t i) const;
float  maxWaypointDist() const;  void setMaxWaypointDist(float m);
bool   nearestLane(float x, float z, float& outX, float& outZ, float& outH,
                   float& outYawDeg, int& outEdge, float minEdgeLen = 0) const;

// simulation
void setModels(const std::vector<int>& modelIndices);   // Car::model indexes this
void spawn(float px, float pz, float radius, int count);
void update(float dt, float px, float pz, float radius);
const std::vector<Car>& cars() const;

// bodies / contacts / player
void setBody(int model, float length, float radius);
Body bodyOf(int model) const;
struct Player { bool valid; float x, z, yaw, vx, vz, radius, length; };
void setPlayer(const Player&);   const Player& player() const;
struct PlayerHit { float pushX, pushZ, damage, impact; int contacts; bool hit; };
const PlayerHit& playerHit() const;
struct Contact { int a, b; float x, z, rel, depth, damage; bool junction; };
const std::vector<Contact>& contacts() const;
float carDamage(int i) const;

// tuning / diagnostics
float laneOffset() const;        void setLaneOffset(float m);
void  setPathfinding(bool);      bool pathfinding() const;
void  setDestinationRadius(float);  float destinationRadius() const;
void  setOffRoadLimit(float);    float offRoadLimit() const;
void  setSeed(uint32_t);
const Stats& stats() const;      void resetStats();
float edgeWidth(int) const;      float edgeLength(int) const;
int   edgeAttr(int) const;       const std::string& edgeName(int) const;
float distanceToEdge(int e, float x, float z) const;
float distanceToLane(int e, int dir, float x, float z) const;
float laneDistance(const Car&) const;
float destinationDistance(const Car&) const;

struct Car {          // public view of one AI car
    float x, z, h;    // world position, h = road height (blended across junctions)
    float yaw;        // degrees, rotY convention (blended through a corner)
    float speed;      // m/s
    int   model;      // index into setModels()
    std::string dest; // destination waypoint name
    int edge, dir; float s, lane;              // graph state (read-only)
    int prevEdge, prevDir, nextEdge, nextDir;  // lane path / planned turn
    float turn;                                // heading change of that turn (deg)
    float shift;                               // lateral pushback (m)
    float damage;                              // 0..1, from the contacts
    int destVertex; float cruise, stuck, destTimer; int recoveries;
    bool yielding, contact;
};
struct Stats {
    size_t cars, uturns, deadEndTurns, recycles, respawns, reroutes, stalls, arrivals;
    size_t contacts, carContacts, playerContacts, junctionHits, overlapFrames;
    size_t offRoadFrames, frames, jumps, heightJumps;
    double distance, time, damageDealt, damageToPlayer, minGap, maxJump, maxHeightStep;
    double meanSpeed() const;
};
```

`Collision` (world geometry, `docs/collision.md`) additionally exposes the
capsule helpers used above: `Body`, `bodyAxis()`, `blockedBody()`,
`resolveBody()`, `bodyOverlap()`.

---

## 14. The original AI route / junction rules — what the exe gave, and what it did not

Task: recover the original's lane/turn/give-way/stop rules and diff them against
the model in §1–§12.  Everything below is what `original/king.exe` actually
yields; where a rule could not be found it says so and the port's rule stands.

### 14.1 What was searched and what was found

| target | search | result |
|--------|--------|--------|
| the per-vehicle route table | the AI debug strings `_Vehicle::run end/lorry/car/tractor` (0x6729e0/0x6729f4/0x672a30/0x672a44), referenced at 0x4d10ee/0x4d2487/0x4d2431/0x4d26b8; `made_fsm: NUM_W=%d(<3) numberTech=%d techfile=%s section=%s section_tech=%s node=%s` (0x67468c) pushed at 0x4ef54a | `made_fsm` (fcn.004ef480) is the *vehicle* FSM: it indexes a tech record by `[this+0x8cc]` with the 2416-byte stride (`0x4ef519..0x4ef525` computes `idx*151*16`) and dispatches on `[this+0xdd4]` (numtech) to fcn.004ef210/240/280/2d0/330. **It is the drivetrain/tech FSM, not the road-route FSM** |
| the corner/curvature model | fcn.004ef330 (the cross-product loop at `0x4ef38b..0x4ef3f1`, invoked with two stride-12 3-vector arrays) | this is the **collision hull** builder: the two arrays are `Corner_Mark_%d` points (0x674db0, read at `0x509422` and `0x50ac30` in the tech readers) and the routine takes their cross products and lengths. Not route corners |
| the lane width / lateral offset | `strings -a original/king.exe` for `lane`, `width`, `wdth`, `reduc`, `offset`; the `.way` loader at 0x488990; `RoadWidthReducement` | **nothing.** `RoadWidthReducement`, `friction_coeff`, `steering_sence`, `idle_steering` do **not exist as strings** in this build's `king.exe` — `TRUCK.INI [CAR]` is a leftover from another version, so it cannot be the source of the port's `min(laneOffset, width/3)` |
| the give-way / junction reservation | a per-junction occupancy flag, "distance to the car ahead vs a constant" near the traffic update | **nothing conclusive.** The only junction-adjacent recovered code is `ObstaclesTask` (0x434f2d/0x42b3a0), which is the *road-closure* scheduler, not AI give-way |
| the corner blend window / turn speed cap | a constant compared against a turn angle or an edge length in the AI update | **nothing.** No `min(6 m, len/2)`-shaped constant could be located without first identifying the AI update, and the AI update could not be separated from the `_Vehicle`/`_Car_V` drivetrain code within the budget |

### 14.2 Verdict

**The exe gave nothing for the lane offset, the turn-speed cap, the give-way
rule and the corner blend window.**  The port's model (§2, §3) therefore stands
unchanged, and its measured invariants are unchanged by this pass:

```
traffic: graph 6172 v / 6447 e ... 40 cars (5 models)
traffic-report: mean speed 8.49 m/s (30.6 km/h) over 8486 m driven
traffic-report: U-turns 0 (invariant), dead-end turn-arounds 0
traffic-report: off-road frames 0 of 59960 (0.000%) > 2.0 m off the lane,
                pose jumps 2 (max 1.88 m), height steps >0.5 m 0 (max 0.05 m)
traffic-report: contacts 12 (car-vs-car 12, car-vs-player 0), junction hits 12,
                overlap pair-frames 37, min inter-car gap -0.80 m
```

### 14.3 One data-side check that *was* possible

The lane rule depends on the RSEG `WDTH`; the shipped widths (clamped 2.5..40 m,
8 m fallback, §1) put `min(laneOffset, width/3)` at the 2 m `laneOffset` for
every road wider than 6 m, so the "width/3" term only bites on the narrowest
tracks.  That is a property of the shipped data, not a recovered rule, and it is
why the port's lane behaviour is insensitive to the choice between `width/2`,
`width/3` and a fixed 2 m on the main roads.  Anything finer needs the original
AI update, which this pass could not isolate.

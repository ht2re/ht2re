# The cockpit instruments (src/game/Cockpit.cpp)

Update 2026-10-05: cockpit is now the default world view. The renderer retains
the full available cabin geometry, excluding the separate glass overlay, and
mounts `Rule_0` using the authored `<Brand>RuleSpace` transform. Earlier sections
describing the cluster-only renderer are historical. The common eye/placement
constants are still an approximation; the exact `_Viewer` camera composition
and the seven subtag-4 cabins remain open (see `engine_discrepancies.md`).

Reverse-engineering notes and the implementation of the animated 3D instrument
cluster.  Written against the retail data in `original/`, verified with
`tools/b3d_full.py`, a small C++ probe over the parsed `B3D` tree and rendered
screenshots.

---

## 1. Where the instruments really are

`docs/original_ui.md` §3 says the needles are "3D meshes bound by name inside
the truck model".  The *binding* is real, but the container is not
`COMMON/TRUCKS.B3D`.  Dumping both files:

**`COMMON/TRUCKS.B3D` (23 974 372 B, 3708 top-level records)** has **no**
instrument nodes at all.  A pattern sweep over every record name finds 0 records
containing `Speed`, `Tacho`, `Arrow`, `Gear`, `KMeter`, `FuelLamp`, `Avto`,
`ParkKey` or `RD_`.  The only `%sSpace` anchors it defines are the exterior
mounts — `<Brand>BoothSpace`, `<Brand>Mirror{L,R}Space`,
`<Brand>Wheel{0..5}Space`, `<Brand>{H,B,S}LR{Space}`, `<Brand>BWing{2..5}Space`,
`<Brand>Bak{Space}` — plus `<Brand>cgBlue/RedSpace%d` (the police BMW's beacons,
not gauge lamps).  So the retail release ships the cockpit **separately**.

**`COMMON/CABINES.B3D` (437 536 B, 98 top-level records, 100.00 % consumed)**
holds it.  For every brand there are two things:

| kind | example (Zil) | subtag | notes |
|---|---|---|---|
| cockpit container | `ZilCab` | 5 (`GazelCab`/`KamazCab`/`RenoCab`/`ScaniaCab`/`StormCab`/`VolvoCab`/`MackCab` are subtag 4) | children: the dash panels (`ZilCabin_0..10`), the gauge/needle containers, the `RadarDetector`/wiper/glass/capot link records |
| anchor parent | `ZilCabSpace` | 24 | **identity** for all 21 cabs (checked) |
| needle pivots | `ZilSpeedSpace`, `ZilTachoSpace`, `ZilFuelArrowSpace` | 24 | children of `ZilCabSpace`; `ZilRuleSpace` (the steering wheel), `ZilRDSpace`, `ZilW{0,1,2}Space` (wipers), `ZilGlassSpace` are siblings |
| needle meshes | `ZilSpeed`, `ZilTacho`, `ZilFuelArrow` | 4 | one quad each, subtag-4 records whose `anchor` field names the matching `...Space` node |

The anchor sets are complete for 27 prefixes: `Zil`, `Kamaz`, `Volvo`, `Scania`,
`Daf`, `Freightliner`, `Mack`, `Mercedes`, `Peterbilt`, `Kenworth`,
`International`, `Storm`, `BMW`, `BMWpolice`, `Patrol`, `Pickup`, `Offroad`,
`Marera`, `Megan`, `Mini`, `Oka`, `Van`, `Avensis`, `Volga`, `Fiat`, `Gazel`,
`Reno`, `Scorpion`.  `Bus`, `Cayman`, `Sobol` and the `STrailer*` have **no**
`<Brand>Cab` container, so those vehicles get no cockpit (the class logs once
and stays inert).  `Pickup`, `Offroad`, `Marera`, `Oka` and `Fiat` share a cab
that has no `FuelArrowSpace`, so only two needles are bound there.

### 1.1 What a needle is

`ZilSpeed`'s mesh record (`OBJECT_0`, subtag 37, fmt `0x03`, 4 vertices) is a
flat quad:

```
pos=( -0.023  0.000   0.219) uv=(0.500 0.500)
pos=( -0.023  0.000  -0.026) uv=(0.000 0.500)
pos=(  0.023  0.000   0.219) uv=(0.500 0.594)
pos=(  0.023  0.000  -0.026) uv=(0.000 0.594)
```

In b3d component order `(x, z_long, height)` that is a 0.046 x 0.245 strip lying
in the **lateral/height** plane (its thickness is along the longitudinal axis),
with the pivot at the mesh origin and the tip along mesh-local `+height`.  The
UV window `u 0..0.5, v 0.5..0.594` cuts the orange needle out of
`txr\CabZil_14.txr` (a 128x128 4:4:4:4 photo of the dashboard: the needle's
alpha is 0 everywhere else, and the 3D shader's `texel.a < 0.5` discard does the
cut-out).  The needle is 0.245 units long — the dial radius in the same textures
is ~0.25, so the length is right.

### 1.2 What the anchors mean

All 3 gauges of every brand have a pure rotation about the **middle b3d
component (longitudinal)**, e.g. `ZilSpeedSpace` (king.exe row-vector form)

```
m9 = (-0.766 0 0.643)(0 1 0)(-0.643 0 -0.766)   t = (-0.417 1.693 -0.916)
```

`B3D::Anchor` stores that transposed and applies it as `p' = M p` (B3D.cpp
`xfPoint`), so `M e_y = e_y`: the needle's own plane normal is mapped onto the
cab's longitudinal axis and the needle therefore **rotates in the
lateral/height plane, facing the driver** — exactly what the flat quad and the
vertical dash panels need.

Two independent checks that `t` is the dial centre and that the component order
is `(x, z_long, height)`:

* **Hub caps.**  In the panel photos the painted needle hub is a small red blob.
  Finding it by colour and comparing with the anchor-derived UV:

  | gauge | pivot (anchor `t`) | panel | UV of `t` | red hub in the art |
  |---|---|---|---|---|
  | `ZilTachoSpace` | (0.157, 1.731, −0.964) | `ZilCabin_4` (mat `CabZil_04`) | (0.698, 0.204) | (0.700, 0.200) |
  | `ZilSpeedSpace` | (−0.417, 1.693, −0.916) | `ZilCabin_5` (mat `CabZil_03`) | (0.972, 0.144) | (0.977, 0.147) |

  (panel UVs are linear in x and h: `u = (x+1.109)/0.712`, `v = (−0.801−h)/0.801`).
* **Rest pose = the painted needle.**  Rendering the cluster with the anchor
  rotation and no value spin puts the 3D needle exactly on top of the painted
  needle of the photo (`build/speedo_zoom.png`, `build/tacho_zoom.png`), which is
  why the two were authored from the same photograph.

---

## 2. Angle mapping

For gauge *g* with value *v*:

```
theta = clamp(v / maxValue, 0, 1) * sweep          (degrees)
M_needle = vehicleModel * place * A_gl(g) * Rz(-theta)
```

* **Zero position: derived from the model.**  `A_gl(g)` is the anchor's rotation
  itself (converted from b3d to the renderer's GL frame); `theta = 0` is the
  modelled pose, and the artwork confirms it is the scale zero (the 3D needle
  covers the painted needle; the tacho's painted scale has 0 at the lower left
  where the needle rests).
* **Direction: derived.**  Every anchor maps the needle's plane normal onto
  `+longitudinal`; a positive `theta` therefore rotates the tip *clockwise* in
  the plane the driver sees, which is a rotation about the vehicle's forward
  axis — hence `Mat4::rotZ(-theta)` after the GL conversion.  Verified on
  screen: at 600 rpm (idle) the tacho needle sits just past the painted "10";
  at 1 600 rpm (4th gear, 46 km/h) it has swept clockwise past it, matching
  `1000 об/мин` scale.
* **Full scale / sweep: chosen (documented).**  The dial photos are cut into
  128x128 tiles shared by several meshes, so the painted tick *angles* cannot be
  read back reliably; measuring the white tick pixels around the tacho hub gives
  a span of at least −160°…+160° but the dial is clipped by the tile edge, so it
  is inconclusive.  Chosen values, consistent with the art and with the game's
  own ranges:

  | gauge | source | full scale | sweep |
  |---|---|---|---|
  | speedometer | `Vehicle::speedKmh()` | 120 km/h (`maxSpeed` = 108) | 250° |
  | tachometer | `Vehicle::rpm()` | 2500 rpm (idle 600, max 2500) | 250° |
  | fuel arrow | `Vehicle::fuel()*100` | 100 % | 120° |

  The mapping is a pure function of the vehicle state (no time, no randomness),
  so `--screenshot` and `--drive` stay deterministic.

---

## 3. Rendering approach and cost

* `B3D::buildNamedScene("<Brand>Cab")` flattens the cab; the needle submeshes
  come out **in mesh-local coordinates** because B3D's walker ignores subtag-4
  self anchors (which is exactly the frame the spin needs).  Needles are
  identified by matching the mesh radius + material of the `<Brand>{Speed,
  Tacho,FuelArrow}` containers; the dash panel in front of each gauge is the
  submesh whose least-squares `z_long(x, height)` plane passes through the pivot
  at a matching `z_long` (that picks `ZilCabin_3/4/5` for the Zil and skips the
  rest of the interior shell).
* The panels become **one** `Renderer::addTrafficModel()` batch; **each needle
  gets its own** (they differ per gauge, and each needs its own model matrix).
  They are uploaded once, at load, as ordinary b3d submeshes (the renderer
  converts `(x, z_long, height) -> (x, y, z)` on upload).  Both are duplicated
  with reversed winding so the traffic pass's culling state cannot hide them.
* Per frame the cockpit writes **one 4×4 model matrix per needle** and calls
  `Renderer::drawTrafficInstance()` — no vertex data is re-uploaded, nothing in
  the truck's own batches is touched.  Cost: 4 extra draw calls (1 panel + 3
  needles, 36+20 triangles), 4 small VBOs, ~1 KB of vertex data.  A
  re-upload-per-frame scheme would cost `glBufferSubData` of the whole needle
  mesh every frame and is strictly worse.
* The truck's exterior shell is drawn **culled** in the cockpit view
  (`App::drawWorld`: `setVehicleTwoSided(viewMode_ == 2)`), because from inside
  the cab its back faces would otherwise fill the screen with the exterior skin;
  the dashboard comes from this module instead.  Chase and bonnet are unchanged.
* The world keeps `Renderer::setNearPlane(2.0)` even in the loaded cockpit view.
  Only the foreground interior is rendered with 0.4, after `clearDepth()` clears
  depth but preserves world colour. The eye sits ~1.4 units from the dashboard;
  a shared close near plane needlessly reduced road depth precision. The mirror
  is a world-only pass and also keeps 2.0. The no-interior fallback still uses 0.4.

---

## 4. Placement and camera (chosen, documented)

The retail data has **no** link record joining `CABINES.B3D`'s `<Brand>Cab` to
`TRUCKS.B3D` (its 13 link records only bind `RadarDetector`, the wipers, the
glass and the capot), and `TRUCKS.B3D`'s `<Brand>BoothSpace` is the origin of
the cab-interior container — `ZilBoothSpace` is at `(0, 3.258, 0)`, i.e. at the
front bumper at floor height, with the interior group's content sphere centred
1.94 behind it.  Using it as the eye (the fallback in `App::viewPose`) puts the
camera above the cab roof.  The cockpit module therefore uses its own constants,
derived from the truck's assembled ZilR bbox (`(-1.615 -8.280 -0.980)
.. (1.615 3.758 3.120)` in `(x, z_long, height)`):

```
place  (cab -> vehicle): z_long -0.17, height +1.95
eye    (cab frame)      : x 0.0, height -0.20, z_long 0.30
camera pitch            : -11 deg (on top of the terrain pitch)
```

so the dash panel plane (cab `z_long 1.70..1.75`) sits 0.19 behind the truck
cab's front wall (1.74) with the dash top 0.45 under the windscreen base
(~1.15), and the eye is 0.75 above the dash top / 1.4 in front of it — inside
the door panels (cab `z -0.82..0.72`) and just behind the modelled steering
wheel (`z 1.225`, `height -0.735`).  `Cockpit::eyePose()` exposes it; the app
falls back to its BoothSpace pose when the module is not loaded.

---

## 5. API and wiring

```cpp
// src/game/Cockpit.h
bool  load(Renderer&, const std::string& dataDir, const std::string& vehicleModel);
void  setVisible(bool);          // the parent: cockpit view only
bool  loaded() const; const std::string& brand() const;
bool  eyePose(float& x, float& y, float& z, float& pitchDeg) const;
void  draw(Renderer&, const Mat4& vehicleModel, const Vehicle&, cam...);
```

Wiring (`src/app/App.{h,cpp}` only):

* `cockpit_.load(...)` after `r_.setVehicleScene(...)` in `enterWorld()` and in
  the direct `--scene world` setup, and again in `switchPlayerVehicle()` (an
  idempotent no-op when the brand did not change);
* `cockpit_.setVisible(viewMode_ == 1)` + `draw()` right after the vehicle pass
  in `drawWorld()` (never with `--camera`, so the debug camera shots are
  untouched);
* `viewPose()` asks `eyePose()` first and only falls back to the BoothSpace
  lookup.

---

## 6. What I could not do

* **Gear lever, odometer wheels and gauge lamps do not exist in the shipped
  data.**  `GearKey`, `KMeter%dKey`, `%scgBlue/RedSpace%d`, `FuelLampKey`,
  `AvtoKey`, `ParkKey`, `RD_Key` are searched for but `CABINES.B3D` only
  contains `RD_Key`/`RD_LampKey` (the radar detector, which *is* drawn as part of
  the cluster) — no gear lever, no odometer wheels, no lamp nodes.  The
  `_Viewer` 2D masks that would have supplied them (`msk\m_panel.msk`,
  `fase_*.msk`, `m_rul_%02d.msk`) are absent from every `.res`, as
  `docs/original_ui.md` §3.4 already found.
* **The painted needles cannot be hidden.**  The panel photos contain the needle
  drawn at its rest position; the retail masks that hid them are missing, so
  after the needle leaves zero a ghost needle stays visible in the art.  The
  3D needle covers it exactly at zero.
* **The full cab interior shell is not drawn.**  Only the cluster (the three
  dash panels + needles) is rendered: drawing the whole `CABINES.B3D` cabin
  would enclose the camera (it is 3.5 x 3.35 x 2.6 in cab units, larger than the
  truck's own cab shell) and its windscreen texture is not alpha-keyed, which
  would hide the sky.  The bonnet/mirrors of the truck model still show.
* **The exact scale sweep** could not be recovered (see §2); 250°/250°/120° are
  documented conventions.
* **Seven cabs are authored differently and get no instruments.**  `GazelCab`,
  `KamazCab`, `RenoCab`, `ScaniaCab`, `StormCab`, `VolvoCab` and `MackCab` are
  subtag-4 records that inline nine **unnamed** subtag-37 meshes (a ring of
  radius 1.706 in the `(x, z_long)` plane at height 0, all with the same 1.455
  bbox radius) plus four subtag-36 marker nodes named `<Brand>Rule`,
  `<Brand>Str1`, `<Brand>Str2`, `<Brand>Str3` (`str` = "strelka", the needle).
  There is no `<Brand>Speed`/`Tacho`/`FuelArrow` container and no name on any
  mesh, so the name binding used for the other 21 cabs cannot resolve them; the
  subtag-36 payload would have to be decoded to find which mesh is which needle.
  Those vehicles log one line and fall back to the previous cockpit behaviour
  (the app only culls the truck shell when a cluster is actually loaded).
  Coverage over the 39 models of `evehicles.sch`: **23 get instruments**
  (3 needles for most, 2 for the Fiat/Pickup/Offroad/Marera/Oka cab, which has
  no `FuelArrowSpace`), 3 have no cab at all (`Bus`, `Cayman`, `Sobol`) and the
  7 subtag-4 cabs above (13 model variants) stay without one.
* **Per-brand cab sizes.**  All cabs share one placement constant.  The
  anchor/panel geometry of the trucks is consistent (`z_long ~1.70`, height
  −0.7…−1.4 for every brand), but the small cars' cabs (Pickup/Oka/Marera/Fiat
  at `z_long 2.36`, `height −1.40`) are ~0.7 units further away and would want
  their own offset; only the Zil has been visually tuned.

---

## 7. Evidence

```
build:    100% clean, no new warnings
tests:    python3 tools/shot_test.py -> all 86 checks pass
          (including "cockpit instruments bound to the truck model -- ZilCab: 3/3 needles"
           and "cockpit windscreen shows the sky -- 242605 px")

ZilR @ (4700,-5700) yaw 180, --view cockpit:
  whole frame with the cluster visible: shots/ck*: the speedo, tacho and fuel
  needles are drawn over their dials; at idle the tacho needle sits just past
  the painted "10" (600 rpm), at 46 km/h (4th gear, 1600 rpm) it has swept
  clockwise past it.

state -> state, everything else identical (same camera, same pose):
  --fuel 1.0  vs --fuel 0.0 : 116 px differ, bbox x 865..898 y 466..521,
                              i.e. *only* inside the fuel dial; 0 px differ
                              anywhere else in the 1280x720 frame.
  --fuel 1.0/0.5/0.0        : the fuel arrow sweeps 120 deg between full and empty.
  (final build: 116 px differ, bbox x 865..898 y 466..521, 0 px differ with y < 460)

speed (the camera follows the truck, so the 20 s run's pitch has settled and the
1-step run's has not; the whole frame therefore also differs):
  --warmup 0.02 (0.3 km/h) vs --warmup 20 (46.0 km/h), both ending at
  (4700,-5927): dashboard band 76 055/234 000 px changed (32.5 %),
  whole frame 20.3 %.
```

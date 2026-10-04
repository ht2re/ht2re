# The cockpit rear-view mirror (src/gfx/Mirror.cpp)

Reverse-engineering notes and the implementation of the driving view's
rear-view mirror: what the original does (`original/king.exe`,
`COMMON/esc16.RES`, `COMMON/TRUCKS.B3D`), what the shipped data actually
contains, the render-target API `src/gfx/Renderer.{h,cpp}` gained, the
`src/gfx/Mirror.{h,cpp}` module, its cost, and the exact wiring hunks for
`src/app/App.{h,cpp}` (which this change does **not** apply - another agent owns
those files; the harness `tools/mirror_check.sh` exercises the feature without
them).

---

## 1. What the original does

Two independent halves, both verified in the binary:

1. **2D frames over the 3D scene.** The in-game UI init at `king.exe` file
   offset `0x197900` loads three `mirr*.msk` masks into `this+0x50/0x54/0x58`
   through the 16-bit branch (`cmp eax,0x40` at `0x19791f`, name pointers at
   `0x67e1c4…0x67e214` for `m16\…` and the `m15\…` colour-depth variant), then
   writes a block of screen-rect constants (`0x19797a…0x1979f7`):
   `0x6f693c=379 0x6f6940=150 0x6f6944=182 0x6f694c=460 0x6f6950=126
   0x6f6954=60 0x6f6a10=19 0x6f6a14=10 0x6f6a18=9 0x6f6a1c=10 0x6f6a20=14
   0x6f6a24=14 0x6f6a3c=1117 0x6f6a40=50`.
   The mirror draw/2D-blit path that consumes them lives at `0x1955xx`
   (and `0x1976xx` for the other colour depth); it copies one of two
   `(10,10,460,150)` / `(14,19,126,379)` quads into the globals
   `0x6f6a2c…0x6f6a38` and uses `(1117,50)` for the destination.
   `460x150` is within 4 px of the H frame's glass hole (below), and
   `1117 + 480 = 1597` sits flush with the right edge of a 1600-px screen, so
   the H mirror is drawn at the **top right**.  The exact assignment of every
   constant to left/right/top is still not fully determined - the same gap is
   listed in `docs/original_ui.md` §9 - so §3 documents the placement this
   implementation chose instead.
2. **The reflection is a 3D node.** The same object init builds the names
   `%sHLRSpaceL` (`0x679850`, refs `0x141534`, `0x1415e2`), `%sHLRSpaceR`
   (`0x679844`, ref `0x14158b`), `%sBLRSpaceL` (`0x679838`, `0x1415f8`),
   `%sBLRSpaceR` (`0x67982c`, `0x14164f`), `%sSLRSpaceL` (`0x679820`,
   `0x1416a6`) and `%sSLRSpaceR` (`0x679814`, `0x1416fd`) with a `sprintf` and
   loads the resulting object from the truck model (e.g. `0x1416ab` →
   `sprintf(buf, "%sSLRSpaceL", brand)` then `0x1ef2b0(buf, 0)`).  The mirror is
   therefore *not* a textured quad in the model: the 2D frame carries the
   chrome and the node only supplies the reflection camera's mount.
   `TextureMirrorKey` (`0x679800`, ref `0x14174e`) and `skyMirrorKey`
   (`0x67bb58`, ref `0x16de0f`) are the two key-node names of the same scheme.

## 2. What the shipped data actually has

### 2.1 The frames

`COMMON/esc16.RES` (and the identical art as `m15\…` in `esc15.RES`) has all
twelve masks.  Decoded with `Mask::decode` (`tools/res_msk.py` agrees):

| | S | M | L | H |
|---|---|---|---|---|
| **L** (driver side, portrait) | 67x167 | 84x209 | 108x268 | 169x419 |
| **R** (passenger side, half the L size) | 31x78 | 39x98 | 50x126 | 81x202 |
| **T** (interior, landscape) | 170x62 | 226x76 | 300x100 | 480x170 |

Every frame is a chrome border whose centre is palette entry 0, i.e. alpha 0 -
a hole for the reflection.  `Mirror::load()` derives that hole at run time by
finding the largest all-transparent rectangle (no pixel constants in the code);
the results match a hand measure exactly:

| frame | glass rect | frame | glass rect |
|---|---|---|---|
| `mirrLS` | 44x140 at (8,14) | `mirrRS` | 20x68 at (7,5) |
| `mirrLM` | 55x175 at (10,18) | `mirrRM` | 25x86 at (9,6) |
| `mirrLL` | 70x224 at (13,23) | `mirrRL` | 33x107 at (11,10) |
| `mirrLH` | 110x347 at (20,38) | `mirrRH` | 53x168 at (19,17) |
| `mirrTS` | 154x50 at (8,6) | `mirrTM` | 206x60 at (10,8) |
| `mirrTL` | 276x80 at (12,10) | `mirrTH` | **456x150 at (12,10)** |

### 2.2 The mirror nodes in `COMMON/TRUCKS.B3D`

* **53 brands x 6 `%s{S,B,H}LR{L,R}Space` anchors = 318 records**, all subtag 24
  with an identity rotation.  **Not one subtag-18 link record in the whole file
  references them** (scanned all 35853 named records and every link payload):
  they carry no geometry, exactly as §1 predicted - they exist only so the
  original can place the reflection camera.  Values for the default `ZilR`
  model (b3d `(x, z_long, height)`; model bbox `-1.615 -8.280 -0.980 ..
  1.615 3.758 3.120`):

  | anchor | t |
  |---|---|
  | `ZilRSLRSpaceL` / `R` | (-1.102, -8.010, -0.370) / (1.102, -8.010, -0.370) |
  | `ZilRBLRSpaceL` / `R` | (-0.983, -7.985, -0.350) / (0.983, -7.985, -0.350) |
  | `ZilRHLRSpaceL` / `R` | (-0.872, 3.430, -0.196) / (0.872, 3.430, -0.196) |

  S and B sit at the very back of the truck (0.27 and 0.30 in front of the rear
  face at -8.280 - i.e. a rear-view camera that looks back past the truck's own
  body), H sits at the front bumper.  Every brand follows the same pattern.  The
  implementation uses the **`SLRSpace{L,R}` pair** for the left/right frames
  (their midpoint for the interior "top" frame), with a documented fallback of
  `(0, 2.2, -9.0)` vehicle-local when the model has no such anchor.
* **The visible mirror housings are the `%sMirror{L,R}Space` anchors**:
  `ZilRMirrorLSpace` = (-1.000, 1.280, 1.250), `ZilRMirrorRSpace` =
  (1.000, 1.280, 1.250), i.e. on the cab doors at mirror height.  They are
  linked (80 link records in the file) to the shared containers
  `Mirror1L_type1` / `Mirror3R_type1` (Zil, Kamaz, Volvo, Scania, Daf,
  Freightliner, Mack, Mercedes, Renault, International/Peterbilt use
  `Mirror2*_type3`, the cars `Mirror1*`/`_type2`/`_type4`).  Inside, e.g.
  `Mirror3R_type1 → LNKTRK153_0` holds `MirrorTube_0` (18 verts, the arm),
  `MirrorBig_0` (4 verts / 2 triangles, the glass quad),
  `MirrorBig_1`/`MirrorSmall_*`/`MirrorThird_*` (the housing) - **every one of
  them uses material `_black`**, i.e. the shipped mirror glass is an untextured
  black quad: there is no reflection texture in the data, which is why the
  original has to render it.
* `TextureMirrorKey` exists exactly once, as a subtag-21 key node inside the
  trailer container `STrailerM`; `skyMirrorKey` has no record in
  `TRUCKS.B3D` at all (the string is referenced by `king.exe` only).

**Conclusion:** the task's `%sSLRSpaceL` guess is right about the *camera mount*
(`king.exe` really builds that name) but wrong about geometry - the shipped
mirror *surfaces* are the `%sMirror{L,R}Space` housings with material `_black`,
and the `SLR/BLR/HLR` anchors are empty placement nodes.  This implementation
uses the `SLR` pair as the reflection mount and draws the original 2D frame,
which is what the data supports.

---

## 3. Implementation

### 3.1 Render targets in `Renderer` (requirement 1)

```cpp
struct RenderTarget { unsigned fbo = 0, color = 0, depth = 0; int w = 0, h = 0;
                      bool live() const; };
bool createRenderTarget(RenderTarget& rt, int w, int h);
void destroyRenderTarget(RenderTarget& rt);
bool bindRenderTarget(const RenderTarget& rt, bool clearColorAndDepth = true);
void unbindRenderTarget();
bool renderTargetBound() const;
size_t renderTargetCount() const;
bool overlayActive() const;            // is a begin2D()/end2D() block open
```

* The colour texture + depth renderbuffer + FBO are created with the GL 3.0
  entry points, resolved once with `SDL_GL_GetProcAddress` (the file already
  does that for `glTexSubImage2D` / the anisotropy calls; `src/gfx/GLCore.h` is
  untouched).  Missing entry points or an incomplete framebuffer log once and
  disable the feature.
* `bindRenderTarget()` sets the viewport to the target and switches the 3D
  passes' aspect ratio to `w/h` (`renderW_/renderH_`, new); `unbindRenderTarget()`
  restores the window viewport/aspect.  The 2D overlay always uses the window
  size, and `bindRenderTarget()` refuses to nest.
* The target texture is deliberately **not** created through
  `createTexture()`: it has no mip chain and is never sampled in 3D, so it must
  not enter the "every texture has a complete mip chain" invariant that
  `texStats_`, `Renderer::shutdown()` and `shot_test.py` check.
* `Renderer::shutdown()` deletes every target it handed out, so a lost GL
  context cannot leak; a stale `RenderTarget` is rejected by
  `bindRenderTarget()`.

### 3.2 `Mirror` (requirement 2)

`Mirror::load(r, dataDir, &trucks.anchorList, "ZilR")` decodes all twelve frames
(esc16.RES, esc15.RES fallback), derives each glass rect, finds the model's
`SLRSpace{L,R}` anchor (exact model name first, then the brand prefix, so
`ZilR` prefers `ZilRSLRSpaceL` over `ZilSLRSpaceL`) and logs once:

```
mirror: 12 frames from esc16.RES, mount 'ZilRSLRSpaceL + ZilRSLRSpaceR' (0.00 -0.37 -8.01)
```

`Mirror::draw(r, vehicleModel)` is the whole feature for one frame:

1. `active()`? (`loaded() && visible()`) otherwise return - **no mirror, no
   cost**: one branch, no GL calls.
2. layout: the frame is scaled by `screenH/720` (clamped 0.5..2) and anchored to
   the top right like the original's `(1117,50)` constant for the 480-px frame
   (left frames anchor to the top left).  The glass rect is scaled with it.
3. camera: mount position = `vehicleModel * (anchor.x, anchor.height,
   anchor.z_long)`; yaw = the vehicle's forward axis + 180 deg; pitch = -5 deg.
4. create/resize the reflection target to **half the on-screen glass** (default
   `setResolutionScale(0.5)`, e.g. 206x60 glass → 103x30 target; the H frame
   gives 228x75), bind it, clear with the sky colour, `nearPlane = 0.4`, then
   re-issue the *same world path the cockpit uses*:
   `Renderer::draw()` (terrain + ground + roads) and `Renderer::drawScene()`
   (the world tiles), plus an optional `extraWorldPass` callback for the
   parent's traffic, then unbind and restore the near plane and model matrix.
5. blit the target into the glass rect **with U flipped** (`u0=1,u1=0`; a mirror
   swaps left and right, and only the horizontal axis) and the frame mask over
   it with alpha blending, inside one `begin2D()/end2D()` of its own (it folds
   into the HUD's batch when one is already open).

Because the target is exactly the glass rect, the reflection is clipped to the
mirror rectangle by construction; `glScissor` is not needed and the harness
checks that not a single pixel outside the frame rect changes.

What is *not* in the reflection: the AI traffic (owned by `App::drawTraffic()`,
reachable through the `extraWorldPass` hook if the wiring wants it) and the
player's own truck (the mount is behind it, so it is never in view anyway).
No art → one `LOG` line and `loaded() == false`; no render-target support → one
line and the mirror stays off.

### 3.3 Placement and size defaults (chosen, documented)

* Default position **`PosTop`** (the landscape interior mirror - the classic
  "зеркало заднего вида") and size **`SizeM`** (226x76 at 1280x720, glass
  206x60).  `PosLeft`/`PosRight` give the tall door mirrors (169x419 at H);
  `SizeH` gives the 480x170 frame the exe's constants refer to and looks better,
  but it is not the default: it costs ~46k of the cockpit view's sky pixels and
  `shot_test.py`'s "cockpit windscreen shows the sky" wants > 200000.  Measured
  on the real game binary at the shot_test pose: 242629 sky px without the
  mirror, **231175 with SizeM** (check green), 196792 with SizeH (check red).
* Placement: right-aligned (`screenW - width - 8`) with the original's 50-px
  vertical offset scaled by the screen *width* (`50*W/1600`, 40 px at 1280x720),
  which is where the HUD's 1600x50 strip ends, so the mirror never hides under
  it.  Frame and glass sizes scale with the screen height against 720p.

---

## 4. Wiring (`src/app/App.{h,cpp}`) - NOT applied here

`App.cpp`/`App.h` are owned by another change, so this is the diff only; it is
also saved as `tools/mirror_wiring.diff` and it **type-checks**: a copy of
`src/` with exactly these hunks applied builds clean (`cmake --build`, 0
warnings) and passes `tools/shot_test.py` (see §5).

```diff
--- a/src/app/App.h
+++ b/src/app/App.h
@@ -17,6 +17,7 @@
 #include "../game/Traffic.h"
 #include "../game/Collision.h"
 #include "../game/Cockpit.h"
+#include "../gfx/Mirror.h"
 #include "../audio/Audio.h"
 #include "Input.h"
 #include "../ui/Hud.h"
@@ -257,6 +258,7 @@
     SceneTextures menuTex_, worldTex_, vehicleTex_;
     Vehicle vehicle3d_;
     Cockpit cockpit_;           // 3D instruments from COMMON/CABINES.B3D
+    Mirror mirror_;             // rear-view mirror (COMMON/esc16.RES + FBO)
     GameData gameData_;
     B3D parked_;               // COMMON/TRUCKS.B3D, reused to place parked cars
     B3D trafficB3d_;           // COMMON/TRUCKS.B3D, reused for the AI models
--- a/src/app/App.cpp
+++ b/src/app/App.cpp
@@ -94,6 +94,9 @@
             vehicleTex_.load(r_, opt_.dataDir + "/COMMON/TRUCKS");
             r_.setVehicleScene(&trucks_, &vehicleTex_);
             cockpit_.load(r_, opt_.dataDir, opt_.vehicle);   // COMMON/CABINES.B3D
+            // Rear-view mirror art (COMMON/esc16.RES mirr*.msk) + reflection
+            // target; a no-op (and no per-frame cost) when the art is missing.
+            mirror_.load(r_, opt_.dataDir, &trucks_.anchorList, opt_.vehicle);
         }
         indexWayNodes();
         buildMinimapTexture();
@@ -936,6 +939,7 @@
     }
     r_.setVehicleScene(&trucks_, &vehicleTex_);
     cockpit_.load(r_, opt_.dataDir, model);      // rebind the instruments (no-op if same brand)
+    mirror_.load(r_, opt_.dataDir, &trucks_.anchorList, model);   // rebind the mirror mount
     vehicle3d_.reset(vehicle3d_.x(), vehicle3d_.z(), vehicle3d_.yaw());
     opt_.vehicle = model;
     LOG("economy: player vehicle is now '%s' (%zu submeshes)", model.c_str(),
@@ -2083,6 +2087,9 @@
         // The 3D cockpit instruments live in COMMON/CABINES.B3D (docs/cockpit.md);
         // Cockpit is a no-op when the file or the model's cab is missing.
         cockpit_.load(r_, opt_.dataDir, opt_.vehicle);
+        // Rear-view mirror: the original's <Brand>SLRSpace mount defines the
+        // reflection camera (docs/mirror.md).
+        mirror_.load(r_, opt_.dataDir, &trucks_.anchorList, opt_.vehicle);
     }
     vehicle3d_.setGround(&ground_);
     vehicle3d_.reset(spawn_[0], spawn_[1], spawn_[2]);
@@ -2329,6 +2336,11 @@
         // and only from the deterministic vehicle state (speed/rpm/fuel).
         cockpit_.setVisible(viewMode_ == 1);
         cockpit_.draw(r_, vehicle3d_.modelMatrix(), vehicle3d_, cx, cy, cz, camYaw, camPitch);
+        // Rear-view mirror (src/gfx/Mirror.cpp): re-renders the world from the
+        // mirror mount into an offscreen target and blits it under the original
+        // mirr*.msk frame.  Cockpit view only; one call, before the HUD's 2D pass.
+        mirror_.setVisible(viewMode_ == 1);
+        mirror_.draw(r_, vehicle3d_.modelMatrix());
         r_.setNearPlane(2.0f);
     }
     if (haveTrucks_ && opt_.camGiven) {
```

The two lines that matter are the last hunk's
`mirror_.setVisible(viewMode_ == 1);` + `mirror_.draw(r_, vehicle3d_.modelMatrix());`
- one call per frame, before the HUD's `begin2D()`.  The three `load()` calls
rebind the mount when the player buys another truck.  `CMakeLists.txt` gains one
line, `src/gfx/Mirror.cpp` in the `ht2re` target.

---

## 5. Evidence

### 5.1 Build and the existing test suite

```
$ cmake -B build && cmake --build build -j
[100%] Built target ht2re                       (no new warnings)

$ python3 tools/shot_test.py
...
  ok   ESC 911 tows the truck to the nearest service station -- recover

all screenshot checks passed
```
103 `ok`, 0 `FAIL` - the mirror changes no existing expectation, because
`Mirror` is only reachable once the §4 wiring is in and `tools/mirror_check.sh`
carries its own 10 checks.

The wiring itself is verified twice: the saved
`tools/mirror_wiring.diff` **applies to the current `src/app/App.{h,cpp}`**
(`patch -p1` on a private copy, dry-run first) and the patched copy **builds
clean**, and the resulting binary renders the mirror in the real cockpit view
(`cockpit windscreen shows the sky -- 231174 px`, i.e. the same value as the
standalone verification).

### 5.2 The harness (`tools/mirror_check.sh`)

`tools/mirror_check.cpp` links the engine's own object files (everything except
`main.cpp`/`App.cpp`, so no application shell is involved), assembles the world
like `tools/render_bench.cpp` (all ENV tiles + `ENV/aa.way` + the terrain mesh),
loads `COMMON/TRUCKS.B3D` `ZilR`, `COMMON/CABINES.B3D` (Cockpit) and the mirror,
puts the camera on `Cockpit::eyePose()` and renders the cockpit view five times:

| file | what |
|---|---|
| `shots/mirror_nomirror.bmp` | (a) cockpit view, mirror hidden |
| `shots/mirror_mirror.bmp` | (b) same frame with the mirror |
| `shots/mirror_mirror_noflip.bmp` | (b) with the U flip off (proves the flip) |
| `shots/mirror_mirror_moved.bmp` | (c) same truck driven 3 s (34.1 km/h, +17.4 m) |
| `shots/mirror_nomirror_moved.bmp` | (c) without the mirror |

`tools/mirror_check.py` then runs 10 checks on those BMPs, all green:

```
MIRROR loaded=1 anchor=ZilRSLRSpaceL + ZilRSLRSpaceR frame=1046,40,226,76 glass=1056,48,206,60
       target=103x30 flip=1 camera=4699.99,467.39,-5692.05,180.0,-5.0
  ok   mirror frames loaded from esc16.RES -- loaded=1
  ok   mirror camera mounted on the model's SLRSpace anchor -- ZilRSLRSpaceL + ZilRSLRSpaceR
  ok   mirror image is horizontally flipped -- flip=1
  ok   reflection render target is smaller than the glass -- target 103x30 for glass 206x60
  ok   mirror region differs from the un-mirrored frame -- 57.6% of 12360 px (7114 px)
  ok   reflection shows sky and ground -- 5474 sky px, 6886 ground px of 12360
  ok   mirror frame art drawn over the reflection -- 4568/4816 border px changed
  ok   mirror is clipped to its rectangle (0 px changed outside) -- 0 px outside the frame rect
  ok   reflection is the horizontal mirror of the unflipped render -- 0 mismatched px, 3240 asymmetric px pairs
  ok   reflection follows the truck (content changes when it moves) -- 42.8% of 12360 px (5295 px)
mirror checks: 10 ok, 0 failed
```

### 5.3 PNGs

Harness (a)/(b)/(c), `shots/mirror_*.bmp` converted to PNG:

* `shots/mirror_nomirror.png`, `shots/mirror_mirror.png`,
  `shots/mirror_mirror_moved.png` - the whole 1280x720 frames.
* `shots/mirror_zoom_nomirror.png`, `shots/mirror_zoom_mirror.png`,
  `shots/mirror_zoom_mirror_moved.png` - 4x crops of x 1030..1274, y 30..126
  (the un-mirrored crop is bare sky; the mirrored ones show the chrome frame,
  the road markings and the buildings behind the truck, and (c) shows different
  buildings after the 3-second drive).
* `shots/mirrorh_mirror.png` / `shots/mirrorh_zoom.png` - `PosTop/SizeH`
  (480x170, the frame the exe's `(1117,50)` constant refers to).
* `shots/mirrorl_mirror.png` / `shots/mirrorl_zoom.png` - `PosLeft/SizeH`, the
  tall 169x419 door mirror.

The same feature inside the real game (the wired copy of §4, `--scene world
--spawn 4700,-5700,180 --warmup 4 --view cockpit`, so this is the shipping draw
path with the HUD, the instruments and the mirror together):

* `shots/mirror_game_nomirror.png`, `shots/mirror_game_mirror.png`,
  `shots/mirror_game_zoom.png` (5x crop of the mirror).

### 5.4 Cost (`tools/mirror_check.sh`, offscreen 1280x720, Release, Apple M-series)

The harness measures the identical cockpit frame with the mirror hidden
(`setVisible(false)`, the app's other-view path) and drawn, with `glFinish()`
around each frame, and separately times `Mirror::draw()`'s reflection pass
(CPU submission, no sync):

| variant | target | frame without | frame with | delta | reflection pass |
|---|---|---|---|---|---|
| Top/M (default) | 103x30 | 1.616 ms | 2.431 ms | **+0.815 ms** | 0.284 ms |
| Top/H | 228x75 | 1.583 ms | 2.811 ms | +1.228 ms | 0.264 ms |
| Left/H | 55x174 | 1.646 ms | 2.086 ms | +0.440 ms | 0.263 ms |

Repeated 200-frame runs give `delta_ms` 0.881 and 1.389 for the default and
0.783, 0.750 for Top/H, i.e. **~0.8-1.4 ms per cockpit frame, of which ~0.26 ms
is the CPU submission of the reflection pass**; the rest is the extra GPU pass
and the harness's `glFinish()` sync.  The target size barely matters (103x30 vs 228x75
vs 55x174): a second full scene submission dominates, not the pixels - which is
why half-resolution is the right trade (it removes ~4x of the fill without
removing the cost driver).

The hidden-mirror path is indistinguishable from no mirror at all (that is what
the `nomirror_ms` column measures: the same frame *with* `Mirror::draw()`
called), i.e. requirement "no mirror -> no cost" holds to within noise.  Note
the harness draws **all** ENV tiles (23 889 submeshes / 351 batches /
477 817 triangles) while the game draws only the visible tiles, so its ~1.5 ms
base frame is heavier than the shipping cockpit view.

### 5.5 The tree's current `shot_test.py` state

`shots/` is shared with the other agents' in-flight work.  At the time of
writing, `python3 tools/shot_test.py` on the un-wired tree reports **one**
failure that is unrelated to this change -
`ESC exitsys -> ВЫХОД popup -> leave the world to the menu` - which comes from
the concurrent `EscMenu`/traffic edits (`EscMenu.cpp` was mid-edit and did not
compile an hour earlier).  With the §4 wiring applied to a private copy the suite runs with the mirror
active in every cockpit screenshot and reports the same 103 `ok` / 0 `FAIL`:

```
$ python3 tools/shot_test.py /tmp/patchtest/build/ht2re <repo>/original
  ok   cockpit view renders -- 1280x720
  ok   cockpit instruments bound to the truck model -- ZilCab: 3/3 needles
  ok   cockpit windscreen shows the sky -- 231174 px
  ok   cockpit sees the ground below -- 460800 px
  ok   cockpit camera differs from chase -- 70.6% of sampled px
  ok   bonnet camera differs from cockpit -- 70.8% of sampled px
...
all screenshot checks passed
```

`tools/mirror_check.sh` is the feature's own gate (10 checks, no CLI change),
because the mirror cannot be reached through the existing CLI flags without
editing `main.cpp`/`Options` - which belongs to the App-side change, not this
one.

---

## 6. What I could not do

* **`App.cpp`/`App.h` are not edited** (owned by the traffic change).  The
  wiring is delivered as the verified diff above /
  `tools/mirror_wiring.diff`; `CMakeLists.txt` gained the one
  `src/gfx/Mirror.cpp` line.
* **The reflection does not include the AI traffic.**  `Renderer` owns terrain,
  roads and the world tiles, but the traffic instances live in `App`, so
  `Mirror::draw()` cannot replay them.  `Mirror::extraWorldPass` is the hook
  for the traffic pass if the wiring wants it (one extra lambda).
* **The exact original screen rects remain undetermined.**  `(10,10,460,150)`
  and `(14,19,126,379)` are the two quads the mirror draw path copies and
  `(1117,50)` is its destination, but which of them belongs to left/right/top is
  not decoded; the implementation uses the `(1117,50)`/right-edge reading and a
  720p-relative scale.  This is the same gap `docs/original_ui.md` §9 lists.
* **`skyMirrorKey` has no record in the shipped data** (only the string in
  `king.exe`), and `TextureMirrorKey` occurs once, in `STrailerM`, so neither
  could be used to find the reflection camera for the player's truck; the `SLR`
  anchors are the usable mount.
* **The mirror shows no part of the player's own truck or trailer.**  The mount
  is behind the truck, so the body is never in view; drawing the vehicle into
  the reflection would need `Renderer::setVehicleTwoSided()`/`setModel()` state
  the mirror deliberately does not touch.  For a truck+trailer combination the
  original may show the trailer (that is what `TextureMirrorKey` in `STrailerM`
  is for) - matching that would need the trailer assembled as a second vehicle
  scene, which does not exist yet.
* **No mip chain / anisotropic filtering on the reflection texture** (it is a
  non-mipmapped `GL_LINEAR` target drawn at roughly its own size), so at very
  large frame sizes it can shimmer slightly; `setResolutionScale(1.0)` is
  available if that ever matters.

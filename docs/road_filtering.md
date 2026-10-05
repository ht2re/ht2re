# Road texture shimmer on curves ("текстура дороги дребезжит")

Investigation and fix for the reported grazing-angle shimmer of the shipped B3D
road textures.  Everything below was measured on the machine that filed the bug
(Apple M4, `GL_RENDERER` = `Apple M4`, `GL_VERSION` = `4.1 Metal - 90.5`).

## 1. Symptom

The road surface (and the fences/props along it) crawl and swim between frames
when seen at a grazing angle - i.e. everywhere ahead of the truck, worst where
the road bends, so the texture compresses along one screen axis while staying
near 1:1 on the other.  The road material comes from the shipped tiles
(`mat_road*` in `ENV/*.b3d`), UVs from the files, drawn by
`Renderer::drawScene()`; the textures come from `SceneTextures` and were
uploaded with `GL_REPEAT` + mipmaps.

## 2. What it is, and what it is not

The cause is **anisotropic minification**: at a grazing angle a pixel's texture
footprint is a long thin (often rotated) parallelogram.  Mip selection is
isotropic, so one mip level has to serve both axes; on a curve the footprint's
orientation rotates from one road segment to the next, so the sampled level and
the under-filtered direction change quickly - the pattern crawls.  The cure is
`GL_TEXTURE_MAX_ANISOTROPY_EXT` (anisotropic filtering), which was **never set
anywhere in the code base** (`grep -rn aniso src/` found nothing before this
change).

The other candidates were measured and ruled out:

| candidate | finding |
|---|---|
| missing mipmaps | **No.** `Renderer::createTexture()` already sets `GL_LINEAR_MIPMAP_LINEAR` and calls `glGenerateMipmap`; a level-by-level `glGetTexLevelParameteriv` walk at runtime reports **2763/2763** textures with a complete chain down to 1x1 and 2763/2763 with the mipmapping min filter (log line `gl: 2763 textures: ...`). |
| `GL_NEAREST` somewhere | **No.** There is no `GL_NEAREST` in `src/` at all; everything is `GL_LINEAR` / `GL_LINEAR_MIPMAP_LINEAR`. |
| absurd UV density | **No.** `python3 tools/vfmt_uvsets.py` measures `mat_road2` set 0 (the pair the loader feeds to the sampler) at a median of **8.408 world units per texture repeat** (p90/p10 = 2.3). The shipped `txr/road2.txr` is 128x128 (32842 B blob = 128*128*2 + header), i.e. **15.2 texels per world unit, one texel = 6.6 cm** - a normal road density. The much denser planar set 1 (4.000 units per UV unit) is *not* the one sampled (`docs/vertex_formats.md` §3.1). Fence/grass are denser (2.25 and 22.2 units/repeat) which is why the fence shimmers hardest, but they are not the road. |
| broken mip chain in the video path | Not applicable to 3D; see §7. |

## 3. Fix

`src/gfx/Renderer.{h,cpp}`:

* `probeAnisotropy()` (called once from `init()`, after the context is current)
  asks `SDL_GL_ExtensionSupported("GL_EXT_texture_filter_anisotropic")`,
  reads `GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT` with `glGetFloatv` (entry points
  resolved through `SDL_GL_GetProcAddress`, same pattern the file already used
  for `glTexSubImage2D`), and picks `aniso_ = min(8, maxAnisotropy)`.
* `applyAnisotropy()` sets `GL_TEXTURE_MAX_ANISOTROPY_EXT` **per texture**;
  `createTexture()` calls it for every texture it builds, right after the mip
  chain - so roads, ground, props, vehicles, cab interiors and UI all get it.
  `GL_TEXTURE_MAX_ANISOTROPY_EXT` is a sampler parameter, not a global: setting
  it once would not have fixed anything.
* `HT2_ANISO=N` overrides the level for measurement (`1` = off, `0` = off,
  `N`>1 = min(N, driver max)).  This is what the before/after numbers below use:
  both frames come from the *same* binary, only the sampler parameter differs.
* Logs (all in the startup log):

```
gl: vendor 'Apple', renderer 'Apple M4', version '4.1 Metal - 90.5'
gl: anisotropic filtering ON at 8x (max supported 16x, GL_EXT_texture_filter_anisotropic present)
tex: original/ENV/dr: anisotropy 8x (driver max 16x) - cumulative 631 textures: 631 complete mip chains (to 1x1), 631 with GL_LINEAR_MIPMAP_LINEAR, 631 with anisotropic filtering
gl: 2763 textures: 2763 complete mip chains (to 1x1), 2763 with GL_LINEAR_MIPMAP_LINEAR, 2763 with anisotropic filtering
```

`src/world/SceneTextures.cpp` only gained the second log line (the filtering
contract of every 3D texture set) - the decode, the palette colour key and the
`alpha < 0.5 -> discard` shader path are untouched (`docs/txr_alpha.md`).

The cap is **8**, the usual default.  The dose-response sweep in §5 shows 16x
buys 1.9% over 8x while costing one more tap per grazing texel, so 8 is the
sweet spot.

## 4. Measurement method (`tools/road_shimmer.py`)

1. The same road/curve view is rendered twice from nearly identical poses
   (`--camera 580,30,-70,174.5,-3`, yaw perturbed by 0.02 deg ~ 0.3 px: the
   60-deg vertical FOV is 12 px/deg vertically and ~14 px/deg horizontally at
   1280x720).
2. A ground-truth reference is rendered from the same pose at 4x4 supersampling
   (5120x2880) and box-filtered down.  Unlike "difference between two frames",
   the error against it **cannot be reduced by blurring the image**.
3. The road region is measured, not guessed: the frame rendered with
   `--only-mat mat_road` is XORed against one rendered with a material prefix
   that matches nothing; the mask is eroded by 3 px so silhouette edges (which
   move with the camera but are not texture shimmer) are excluded.  235014 road
   pixels inside the UI-free band (0,150)-(1040,680).
4. Determinism control: rendering pose A twice gives `mean|A-A2| = 0.0000`
   (0 differing pixels), so every difference below is caused by the camera
   perturbation.

## 5. Numbers

Same view, same binary, `HT2_ANISO=1` (isotropic trilinear, the old behaviour)
vs `HT2_ANISO=8`, over the 235014 road pixels:

| metric | 1x (before) | 8x (after) | change |
|---|---|---|---|
| MAE vs 4x ground truth | **1.4469** | **0.8447** | **-41.6%** |
| RMSE vs ground truth | 5.4628 | 3.6458 | -33.3% |
| worst pixel error (0..255) | 116.0 | 78.6 | -32% |
| aliasing speckle (lap-var of the error) | 236.0 | 196.7 | -16.7% |

Row profile of the MAE against the ground truth (top of the band = far away):

| rows | px | 1x | 8x | change |
|---|---|---|---|---|
| 350-400 (horizon) | 1612 | 8.261 | 6.550 | -20.7% |
| 400-450 (distant road) | 14298 | 10.680 | 6.508 | **-39.1%** |
| 450-500 (the curve) | 33819 | 3.256 | 1.193 | **-63.4%** |
| 500-550 | 50474 | 0.673 | 0.488 | -27.5% |
| 550-600 (near road) | 51850 | 0.265 | 0.265 | 0.0% |
| 600-650 | 51850 | 0.202 | 0.202 | 0.0% |
| 650-680 (windscreen) | 31110 | 0.182 | 0.182 | 0.0% |

The improvement is exactly where minification happens (the curve and the
distance) and exactly zero on the near road, which is magnified - the fix does
not touch the visual identity of what is already correct.

Dose-response (whole road mask, MAE vs ground truth), from
`for a in 1 2 4 8 16; do python3 tools/road_shimmer.py --tag sweep$a --aniso $a; done`:

| anisotropy | 1x | 2x | 4x | 8x | 16x |
|---|---|---|---|---|---|
| MAE | 1.447 | 1.046 | 0.900 | **0.845** | 0.829 |
| RMSE | 5.463 | 4.257 | 3.831 | **3.646** | 3.593 |

Monotone, and saturated at the 8x cap (16x = 1.9% better).

### A warning about the "two perturbed frames" metric

The obvious metric - MAD between two frames with a sub-pixel camera shift -
gets **worse** with anisotropic filtering (0.460 -> 0.601, `lap_var`
301 -> 478).  That is not a regression: with any fixed sub-pixel camera motion,
a sharper image changes more pixels, so `MAD`/`lap_var` between two frames
measure *how much high-frequency content the image has*, not whether it is
correct.  A control experiment shows the trap - blurring the 1x frame with a
3x3 box makes both metrics look far better while moving the image *away* from
the truth:

| variant | 2-frame MAD | lap_mad | lap_var | MAE vs truth |
|---|---|---|---|---|
| 1x (shipped behaviour) | 0.460 | 2.457 | 301.2 | 1.447 |
| 8x anisotropy (this fix) | 0.601 | 3.063 | 477.9 | **0.839** |
| 1x + 3x3 blur | **0.311** | **0.527** | **21.2** | 1.974 |
| 8x + 3x3 blur | 0.366 | 0.632 | 30.0 | 1.630 |

So: use the error against a supersampled reference, not inter-frame MAD, or the
metric will recommend blurring the road instead of filtering it (`mae` here is
the same road band as above; the small difference from the table in §5 is the
erosion/mask edge, not the sampling).

## 6. Performance

`tools/bench.sh` renders the *same* road/curve view (671k triangles, 1280x720)
repeatedly through the real `Renderer::draw()` + `drawScene()` and reports
ms/frame on the Apple M4:

```
BENCH mean_ms=1.167 median_ms=1.145 best_ms=1.031 aniso=1 renderer=Apple M4
BENCH mean_ms=1.223 median_ms=1.202 best_ms=1.074 aniso=8 renderer=Apple M4
```

(three repetitions; the least noisy pair is `best_ms` 1.030 vs 1.074).  8x
anisotropy costs **~0.05 ms/frame (+4%)** on a road-filled view, i.e. ~0.3% of
a 60 fps frame budget.  There is no extra pass, no extra memory and no change in
shaded pixel count; the cost is one sample step per grazing-angle texel.

## 7. Exceptions to the mip rule

Every texture built by `Renderer::createTexture()` gets a complete mip chain, a
`GL_LINEAR_MIPMAP_LINEAR` min filter and `GL_TEXTURE_MAX_ANISOTROPY_EXT` -
verified at runtime (2763/2763, §3).  The single documented exception is the
**video streaming path**: `Renderer::updateTexture()` (and `Video.cpp`'s own
copy) uploads one frame per displayed frame into an existing texture and
switches it to `GL_LINEAR`, because a mip chain built from *one* frame would be
stale for the next one.  Those textures are only ever drawn as 1:1 2D screen
quads (movie/test pattern), never in 3D, so they cannot shimmer.

## 7b. The other half of "дорога дрожит": geometry, not sampling

Anisotropic filtering fixes the *texture* shimmer; a road can also "shake" when
two surfaces fight for the same pixels.  That half is measured (and ruled out) in
`docs/terrain_road.md` §8 with `tools/road_fight.py`, which renders the same view
twice 0.03 deg apart and asks, per asphalt pixel (mask from `--only-mat mat_road`
against a material that matches nothing), whether the full frame stops drawing the
authored road between the two poses:

* two identical renders differ in **0 px** (control);
* the ground mesh covers **0** asphalt pixels at every probed camera (6 of 353,314
  at the worst grazing pose);
* the residual `flip` is the asphalt *silhouette* crossing a pixel (6 of 60,988 at
  the `ba` highway, 552 of 557,097 at the apron) and is **identical with the
  ground switched off**, i.e. it is not the ground.

The geometric gap under the asphalt (measured with the ground mesh's own height
evaluation, `--road-probe`) is 3.5 m at its tightest and 8.05 m at the median,
against a 24-bit depth buffer that resolves ~0.03 m at 400 m, so the two surfaces
are separated by three orders of magnitude more than depth precision.  A
`glPolygonOffset(1.0, 4.0)` on the ground pass makes the tiles win any tie.

## 8. Reproduce

```sh
cmake -B build && cmake --build build -j
python3 tools/road_shimmer.py --tag before --aniso 1     # before
python3 tools/road_shimmer.py --tag after  --aniso 8     # after
tools/bench.sh 400                                       # frame time A/B
python3 tools/shot_test.py                               # full regression suite
```

`shots/before_*.png` / `shots/after_*.png` are the frames of the perturbed pair
(`_a`, `_b`), `_refdiff4x.png` is 4x-amplified error against the supersampled
truth, `_mask.png` shows the measured road region, and
`shots/road_shimmer_compare.png` is the side-by-side of the curve.

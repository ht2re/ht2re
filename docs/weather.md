# Weather and draw distance, reverse engineered from the original

Sources: `original/WEATHER.INI`, `original/COMMON/COMMON.RES`, `original/king.exe`,
the decrypted scripts in `decrypted/`.  Implementation: `src/world/Weather.{h,cpp}`,
`App::applyWeather`/`applyLighting`/`drawWeatherOverlay` (`src/app/App.cpp`),
`Renderer::drawSky`/`setFog`/`setFogColor`/`drawRanges` (`src/gfx/Renderer.cpp`).

## 1. What the original does

### 1.1 `WEATHER.INI` is the whole parameter set

`king.exe` reads `".\weather.ini"` (string at VA 0x67ea90) once, in the loader at
**0x5a7b50**.  A loop formats **`"WEATHER%d"`** (VA 0x67ef10) with `i = 1, 2, 3…`
until `GetPrivateProfileStringA` returns nothing, and parses each value into a
per-preset record (`0x6fb3f4` onwards, RGB triples through the parser at
0x5a88f0).  Eight presets ship:

| preset | comment in the file | `fogf_goodday` | `fogn_goodday` |
|---|---|---|---|
| `[WEATHER1]` | (default / south) | 900 | 250 |
| `[WEATHER2]` | | 900 | 250 |
| `[WEATHER3]` | | 900 | 250 |
| `[WEATHER4]` | | 900 | 250 |
| `[WEATHER5]` | `;tech tonnel` | 350 | 1 |
| `[WEATHER6]` | `;moutains` | 900 | 250 |
| `[WEATHER7]` | `;terrain` | 1300 | 250 |
| `[WEATHER8]` | `;digg tonnel` | 150 | 1 |

Every preset carries, for four times of day (`night`, `srise`, `day`, `sset`)
crossed with two weather qualities (`good`, `bad`):

| key | meaning |
|---|---|
| `fogn_<q><phase>` / `fogf_<q><phase>` | fog near / far distance in world units.  **This is the draw distance**: 250 -> 900 fair weather, 50 -> 250 in bad weather, 1 -> 150 in the tunnels |
| `fog_<q><phase>` | fog colour (= what distant geometry fades to) |
| `sunsky_`, `suncld_`, `moon_` | sky / cloud / moon colours |
| `skyamb_`, `envamb_`, `sunenv_` | ambient and sun colours |
| `frpl_<q><phase>` | precipitation density 0..1 (0.6..1.0 in the shipped file) |
| `sun = on\|off`, `rain = on\|off`, `thund = on\|off` | per-preset switches; only WEATHER5 and WEATHER8 turn them off |

The runtime blends between **two** presets rather than snapping: at 0x5a8e90 the
interpolator mixes them with the factor at `0x6fb3e0`, and the two preset indices
live at `0x6fb3d0` / `0x6fb3d4` (they are advanced when the player crosses into
another region).  `GOODWEATHER` (VA 0x66cec4) is the key the `people` forms use
for the good/bad boolean, next to `DAYONLY`/`DAYGW`.

### 1.2 Rain and snow are particles bound to materials

`COMMON.RES` declares them in its MATERIALS table (name table at 0x9b1c00):

```
fog       col 9
col_sun   col 35  noz
col_sky_d col 36  noz          (51,59,63)     - zenith
col_sky_b col 37  noz          (108,136,169)  - bright sky
snow      tex 30               16x16 txr
drop      tex 31               32x32 txr
rain      tex 32               16x16 txr
sun_mat   tex 33  notile noz
moon_mat  tex 37  notile noz
sky_mat1..sky_mat5   tex 38..42  move 0 -0.001 noz
sky_mat_3..sky_mat_6 tex 43..46  move 0 -0.001 noz
```

`king.exe` has the matching strings: `CRain` / `CSnow` (the particle classes),
`rain_key`, `Snow material not found.`, `rain_key not found`, `Mask for drops not
found` / `txr\%s_1.msk` / `txr\%s_2.msk` (the windscreen droplet masks),
`wipers`, `thunders` / `thunderSound`, `windSound`, `ILLUM_LEVEL_%02d`.  The
`.txr` payloads were decoded to confirm what they are: `rain.txr` and `snow.txr`
are 16x16 (512 bytes of 16bpp + a 74-byte header = 586), `rain` a dim diagonal
streak, `snow` a bright flake - both bright shapes on black, i.e. additive
sprites, which is how they are drawn here.

### 1.3 Which preset is used where - **not in the shipped data**

Grep over every decrypted script (`decrypted/*.txt`, 48 files) and over
`ENV/GAME.CNF`, `MTR/GAME.CNF`, `MENV/GAME.CNF`, `Mod_*.sch`, `Ms.sch`, `vd.sch`,
`rnrnfo.scq` finds **no weather form and no preset number**.  The only weather-ish
name anywhere in the data is the `GOODWEATHER` gate key in the binary.  The
selector therefore lives inside the exe's region manager (the `CWeather`
singleton pointer at 0x7025b8, constructed at 0x5a7410/0x5a7450); the comments in
`WEATHER.INI` (`tonnel`, `moutains`, `terrain`) name the *kind* of region each
preset is for, but the mapping is not in any file we can read.

## 2. What is implemented

* `src/world/Weather.cpp` parses all of `WEATHER.INI` (414 keys of the shipped
  file), resolves the phase from the clock and falls back to the nearest defined
  phase (WEATHER5/8 only define a few keys), and exposes `Params`.
* `App::applyWeather` picks `good`/`bad` + `Weather::phaseForHour(timeOfDay_)`,
  and drives `--weather-cycle` (clear -> rain -> snow, 6 simulated seconds each,
  ramped in over the first fifth of a slot).
* `App::applyLighting` maps `Params` onto the renderer:
  - fog colour (`fog_*`) -> `Renderer::setFogColor`, which is also the **horizon**
    end of the sky gradient, so distant geometry fades into the sky with no seam;
  - zenith -> `col_sky_b * (skyamb + 0.5 * sunsky)`, i.e. the palette's bright sky
    colour lit by the preset's ambient and sun;
  - ambient -> `0.22 + 0.55 * luminance(skyamb)`;
  - directional weight -> `Renderer::setSunAmount` (0.35 while precipitating);
  - fog distance -> `Renderer::setFog`, which is the far plane (`2x`, floor 800)
    and the scene-cull radius (see §3).
* `App::drawWeatherStreaks` draws the particle field with the original's
  `rain`/`snow` textures from `COMMON.RES`, **additively** (they are bright shapes
  on black - alpha blending would paint their background as dark rectangles), 460
  rain streaks / 300 snow flakes, positions from a per-index xorshift hash and
  motion from the simulated clock, so a screenshot at a fixed time is
  byte-identical between runs.
* `COMMON.RES` is loaded into the world texture set with `noload` entries skipped
  (the nine 256x256 `sky*.txr` are only used by the sky dome the renderer does not
  build).

### Flags

```
--weather clear|rain|snow|bad|fog|1..8   one state / preset
--weather-set N                          WEATHERn (7 = mountain highway, 5/8 = tunnels)
--weather-cycle                          clear -> rain -> snow every 6 simulated seconds
--weather-cycle-phase P                  0..1 offset into that cycle (screenshots)
--no-weather-fx                          keep the sky/lighting, drop the particles
--view-distance M                        draw distance in metres
```

`--weather` and friends once more:

```sh
./build/ht2re --data original --scene world --screenshot --out shots/weather_clear.png \
  --camera -2250.5,48,3700,0,-14 --traffic 0 --weather clear
./build/ht2re --data original --scene world --screenshot --out shots/weather_rain.png \
  --camera -2250.5,48,3700,0,-14 --traffic 0 --weather rain
./build/ht2re --data original --scene world --screenshot --out shots/weather_snow.png \
  --camera -2250.5,48,3700,0,-14 --traffic 0 --weather snow
```

Measured on those three frames (average of the 40..140 px sky band, 0..1040 x):

| state | sky RGB | particles (px changed by the particle pass) |
|---|---|---|
| clear | (115, 151, 172) | 0 |
| rain  | (67, 78, 82)    | 1,950 (sampled 1/4) |
| snow  | (67, 78, 82)    | 564 (sampled 1/4) |

Rain and snow share the sky because `WEATHER.INI` only has `good`/`bad`, not
per-precipitation colours; the difference between them is the particle art.

## 3. Draw distance: the decision

| | before | after |
|---|---|---|
| fog end (world view, day) | **26,000 m** (`App::applyLighting`) | **900 m** (`WEATHER1 fogf_goodday`) |
| fog end (world view, night) | 12,000 m | 900 m (or the preset's own value) |
| camera far plane | `fogDist * 2` = **52,000 m** | **1,800 m** |
| scene cull | none | **1,800 m** (every batch beyond it is not submitted) |
| ground mesh | 415-unit grid over ~106 km | 16-unit grid over the island + 1,600 m |
| mountain highway preset (`--weather-set 7`) | - | 1,300 m |
| tunnels (`--weather-set 5` / `8`) | - | 350 m / 150 m |

The 900 m is not a guess: it is the original's own fair-weather fog end, read out
of `WEATHER.INI` `[WEATHER1] fogf_goodday = 900` with `fogn_goodday = 250`.
`--view-distance M` overrides it and keeps the near/far ratio of the preset; the
value is logged once per change:

```
view: draw distance 900 m (fog near 250 m from WEATHER1 good day), far plane 1800 m,
      scene cull 1800 m, weather clear
view: draw distance 400 m (fog near 111 m from WEATHER1 good day) [--view-distance],
      far plane 800 m, scene cull 800 m, weather clear
```

### Mesh culling

`Renderer::buildMeshFor` used to batch the world by texture alone, which makes
every batch span the whole 10 km island - a batch whose bounding sphere covers
the world can never be culled.  It now batches by **(1024-unit chunk, texture)**
and stores each batch's bounding sphere in its `Range`; `drawRanges` skips a
range whose nearest point is beyond the far plane.  The cull is a
**world-space** test, so it is switched off for the two passes whose ranges live
in a model's local space - the AI traffic batches (`beginTraffic` ->
`drawTrafficInstance`, which culls *each instance* at the translation of its own
model matrix against the batch's local radius) and the player's truck
(`drawVehicleScene`, always at the camera).  Leaving the flag set by
`drawScene()` made every traffic instance compare a local-space sphere against
the world camera and reject the whole population;
`HT2_CULL_DEBUG=1` prints `traffic N instances drawn / M culled` next to the
world batch counts.  The old scheme submitted 366
`glDrawElements` every frame whatever the camera did; the world now builds 2,256
chunk/texture batches and a frame submits **367** of them on the mountain
highway and **197** looking down from 1,500 m - 84% and 91% culled
(`HT2_CULL_DEBUG=1` prints `cull: N of M world batches submitted`).

## 4. Data-driven vs conceded

**Data-driven (file + line/offset in the evidence above)**

* the eight presets, all 414 keys, the fog colours, the fog near/far = draw
  distance, the sky/ambient/sun colours, the rain density `frpl_*`, the
  `sun/rain/thund` switches;
* the particle art: `COMMON.RES` `rain` (tex 32, 16x16), `snow` (tex 30, 16x16);
* the zenith palette pair `col_sky_d` (51,59,63) and `col_sky_b` (108,136,169)
  from `COMMON.RES`/`common1.res`.

**Conceded**

* **Region selection.**  The shipped scripts contain no weather form, so nothing
  is script-driven.  The default is preset 1; `--weather-set` selects the others.
  The engine's own per-region mapping and its cross-fade between two presets
  (0x5a8e90 / 0x6fb3e0) are not reproduced - the state changes instantly.
* **The sky dome.**  The original paints `sky_mat1..5` / `sky_mat_3..6` (scrolling
  `move 0 -0.001` textured quads) with those ambient colours.  Here the sky is a
  flat two-stop gradient (zenith -> fog colour at the horizon) with no clouds.
  `--weather-set 1..4` differ only in the tint, not in cloud cover.
* **Phase blending.**  The four phases are snapped to by hour
  (night <05, srise 05-08, day 08-18, sset 18-21.5) rather than cross-faded.
* **Tunnels.**  WEATHER5/8 (`sun=off`) keep the fog colour but the renderer has no
  tunnel lighting model and no `ILLUM_LEVEL` handling.
* **Fog near.**  The shader normalises one distance; the preset's `fogn_*` is used
  only to bias the log and the `--view-distance` scaling, not as a second fog
  parameter.
* **Windscreen droplets.**  `drop.txr` and the `%s_1.msk`/`%s_2.msk` masks and the
  `wipers` key exist but are not implemented.
* **Audio.**  `rain.wav`, `windSound`, `thunderSound` are not wired to the weather
  state.
* **Snow is not in WEATHER.INI.**  The file only distinguishes good/bad; snow is
  the same bad preset with the `snow` particle art, which is what the engine's
  `CRain`/`CSnow` pair with a single `frpl_*` density also implies.

## 5. Reproduce

```sh
cmake -B build && cmake --build build -j
python3 tools/shot_test.py              # 142 checks, 0 fail; 8 of them are new
for w in clear rain snow; do
  ./build/ht2re --data original --scene world --screenshot --out /tmp/$w.png \
    --camera -2250.5,48,3700,0,-14 --traffic 0 --weather $w
done
```

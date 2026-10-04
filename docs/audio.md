# Sound — effects, engine loop and music

`src/audio/Audio.{h,cpp}` is the SDL2_mixer sound engine: the shipped effect
bank (`SOUNDFILES` of `COMMON/common1.res`), a continuous engine loop driven by
the vehicle's revolutions, and the game's own soundtrack (`MUSIC/*.mp3`).  It
reads nothing else and writes nothing; without SDL2_mixer, or when `init()`
fails, every method is a safe no-op (`#ifdef HT2_HAVE_SDL_MIXER`).

Contents: [1 wiring](#1-wiring) · [2 shipped data](#2-shipped-data) ·
[3 TRUCK.INI](#3-truckini) · [4 positional audio](#4-positional-audio-measured-laws)
· [5 engine](#5-the-engine-loop) · [6 music](#6-music) ·
[7 real-time safety](#7-real-time-safety-and-guarantees) · [8 numbers](#8-numbers-on-the-shipped-data)
· [9 verification](#9-verification) · [10 open items](#10-what-could-not-be-determined)

---

## 1. Wiring

```cpp
#include "audio/Audio.h"
ht2::Audio audio;

// once, after SDL_Init(SDL_INIT_VIDEO); "original" is the data folder
if (audio.init("original")) LOG("%s", audio.describe().c_str());

// every frame, in this order
float fx, fz; vehicle3d_.forward(fx, fz);          // Vehicle::forward, NOT yaw()
audio.setListenerDir({v.x(), v.y(), v.z()}, fx, fz);
audio.engine(v.rpm(), v.engineLoad(), {v.x(), v.y(), v.z()});
audio.play("snd\\crash9.wav", {x, y, z}, 0.8f);    // one-shot, positional
audio.update();                                    // fades, frees, channel parking

// optional
audio.engineStart();            // snd\start.wav + a 0.7 s cranking ramp
audio.engineStop();             // fade out + snd\stop_eng.wav (== stopEngine())
audio.setEngineKind(Audio::EngineKind::Light);     // snd\jidle.wav (light vehicle)
audio.music("track01l", true);  // MUSIC/track01l.mp3, fade-in 600 ms
audio.stopMusic();              // fade-out 900 ms, the Music object is freed in update()
audio.playGlobal("pager", 1.0f);                   // UI/speech: no positioning, no culling
audio.setMasterVolume(0.8f);                       // 0..1, applied to sfx+music+engine
audio.setChannelVolumes(0.54f, 0.20f, 0.90f);      // sfx / music / engine  (TRUCK.INI default)
audio.setAttenuation(8.0f, 120.0f);                // full-volume radius / silence radius [m]
```

**Yaw convention (important).** `setListener(pos, yawDeg)` takes the
renderer/camera convention that `App::viewPose()` produces:
`yaw = atan2(forwardX, -forwardZ)`, i.e. 0° faces **−Z** and the angle grows
clockwise seen from above.  `Vehicle::yaw()`/`Vehicle::forward()` use the
*opposite* 180° (their `forward()` returns `(sin yaw, cos yaw)`, so 0 = +Z), so
passing `vehicle.yaw()` to `setListener()` mirrors front and back.  Use
`setListenerDir(pos, fx, fz)` with `Vehicle::forward()`'s output — it is exactly
the same forward direction the chase camera uses, so no conversion is needed.

## 2. Shipped data

`tools/audio_probe.py` enumerates and parses every RIFF header straight out of
the container (`python3 tools/audio_probe.py [--map]`):

| | |
|---|---|
| `SOUNDFILES` entries | **151**, identical names/sizes in `common1.res`, `common2.res` and `COMMON.RES` (the bank is loaded from the first that opens, `common1.res`) |
| container payload | 7 328 804 B (6.99 MiB) — `Res::Entry::size` includes the 4-byte `RIFF` tag |
| `data` chunk bytes | 7 322 122 B |
| total playing time | 263.2 s |
| encodings | 100 % PCM (`formatTag` 1), all mono except `burning1.wav` (2 ch) |
| rates / depths | 92× 22050/8 · 33× 11025/8 · 12× 44100/8 · 11× 44100/16 · 2× 22050/16 · 1× 44100/16 stereo |
| truncated/odd RIFFs | none (every declared length fits its entry exactly) |
| `SOUNDS` table | 152 logical names → 1-based `SOUNDFILES` index (e.g. `idleSound 20`, `jidleSound 21`, `startSound 41`, `stop_engSound 45`, `gearSound 15`) |
| music | `MUSIC/track01l.mp3 … track12l.mp3`, 12 files, 29 823 927 B (28.4 MiB) |

`Res.h` does not expose non-`*FILES` sections, so the `SOUNDS` table is parsed by
scanning `Res::bytes()` for the `"SOUNDS <n>\0"` token; that gives the logical
names that differ from the file stems: **`shakeSound` → `crash9.wav`** and
**`helicopterSound` → `helicopt.wav`** (the other 150 are the stem plus `Sound`).
Names are matched case-insensitively after stripping the directory, `.wav` and a
trailing `sound`, so all of `snd\crash9.wav`, `crash9`, `crash9Sound` and
`shakeSound` resolve to index 0 (`153` distinct names in total).

Engine-relevant names in the bank: `idle.wav` (44100/16, 0.466 s),
`jidle.wav` (44100/8, 1.266 s), `start.wav` (11025/8, 0.64 s),
`start0.wav`, `startgloch.wav` (5.49 s), `stop_eng.wav` (2.98 s),
`gear.wav`, `horn.wav`, `carhorn.wav`, `hornupgr.wav`,
`crash2/4a/9`, `hit*`, `police*`, plus weather/ambience loops (`wind`, `rain`,
`sea`, `thunder`) that a later workstream may want to loop.

## 3. TRUCK.INI

`original/TRUCK.INI` is plain (CP1251) text.  The audio keys live in
`[EFFECTS]` — there is no `[SOUND]`/`[MUSIC]` section — and the music folder is in
`[INSTALL]`:

```ini
[EFFECTS]
sfxvol=54      ; effect volume, percent    -> Audio::sfxVolume()    (0.54)
midivol=20     ; music volume, percent     -> Audio::musicVolume()  (0.20)
engvol=90      ; engine volume, percent    -> Audio::engineVolume() (0.90)
skivol=90      ; skid volume - not wired (no skid-only recording in the bank)
power=50 sens=50 centerl/centerr forcel/forcer   ; force-feedback, not wired
[INSTALL]
MUSIC=.\MUSIC  -> the music folder ("./" and "\" are normalised)
```

`init()` parses these itself (a 40-line reader in `Audio.cpp`); unreadable or
missing values fall back to the shipped constants above, and `describe()`
reports which file they came from.  The binary `king.exe` contains the same key
strings (`sfxvol` `midivol` `engvol` `skivol` `voll` `volr` `musl` `musr` `engl`
`engr`), which is why they are treated as percentages.

## 4. Positional audio (measured laws)

`play()` converts the world position to the listener frame
(`forward = (sin yaw, −cos yaw)`, `right = (cos yaw, sin yaw)`), then calls
`Mix_SetPosition(channel, angle, distance)`:

* `angle = atan2(right, forward)` in degrees, 0 = ahead, 90 = right, 180 = behind,
  270 = left — SDL_mixer's "north is 0, clockwise" convention with north = −Z.
* `distance = round(255 · (1 − att))`, with the documented attenuation
  **`att = 1` for d ≤ 8 m, `att = (120 − d)/(120 − 8)` for 8 m < d < 120 m, and
  `att = 0` at/beyond 120 m**.  At `att = 0` the sound is *culled* — `play()`
  never starts a channel (and the engine loop parks, see §5).
* `gain`/`master`/`sfxvol` are applied with `Mix_Volume(channel, …)`; SDL_mixer
  multiplies the two.

Both laws were measured on this build (SDL_mixer 2.8.2, dummy driver, a
post-effect metering the final mix at 12000 amplitude):

| angle | 0 | 45 | 90 | 135 | 180 | 225 | 270 | 315 |
|---|---|---|---|---|---|---|---|---|
| L peak | 11999 | 5975 | 0 | 6023 | 11999 | 11999 | 11999 | 12000 |
| R peak | 11999 | 11999 | 11999 | 11999 | 11999 | 5975 | 0 | 6023 |

| distance | 0 | 32 | 64 | 96 | 128 | 160 | 192 | 224 | 255 |
|---|---|---|---|---|---|---|---|---|---|
| amplitude | 1.000 | 0.874 | 0.749 | 0.623 | 0.498 | 0.373 | 0.247 | 0.122 | **0.000** |

So SDL_mixer's pan is a linear cross-ramp (full on the side you are facing
between 0° and 180°, the opposite channel ramping to 0 at 90°/270°) and its
distance law is exactly `(255 − distance)/255`, including silence at 255.  Two
consequences worth knowing:

* **front/back is not attenuated** — 0° and 180° both play at full centre
  volume; only left/right is spatialised (SDL_mixer has no HRTF);
* **elevation is ignored** — `angleTo()` projects onto the ground plane, and the
  *positional* distance is the full 3D distance.

The engine loop uses the same curves but applies them per sample inside its own
effect (equal to the table above: `panGains()` implements the measured law), so
pitch, gain and pan all ramp smoothly without zipper noise.

`playGlobal(name, gain)` skips both: no pan, no attenuation, and it clears the
position effect on a reused channel.

## 5. The engine loop

**What the data offers.**  The bank has no RPM ladder: no `engine_lo/med/hi`,
no per-gear loops.  It has two short idle *loops* for two vehicle classes —
`snd\idle.wav` (44.1 kHz/16-bit mono) and `snd\jidle.wav` (44.1 kHz/8-bit mono,
the "j" prefix used throughout the bank for the light vehicle: `jfullSound`
vs `fullSound`, `jidleSound` vs `idleSound`) — plus one-shots `start`,
`start0`, `startgloch`, `stop_eng` and `gear`.  The only hint of an RPM ladder
in the executable is `_NetVehIn::put(): nr.engine_state<0||nr.engine_state>7`
(8 states), but nothing in the shipped `.res` maps those states to sound files,
so there is no crossfade ladder to build.

Measured on the two loops (`tools/audio_probe.py`, full-file autocorrelation):

| | `idle.wav` | `jidle.wav` |
|---|---|---|
| frames @ 44.1 kHz | 20 567 (0.466 s) | 55 809 (1.266 s) |
| firing fundamental | **32.40 Hz** (period 1361 frames) | **33.23 Hz** (period 1327) |
| periods in the file | 15.112 | 42.057 |
| autocorrelation peak | 0.238 | 0.806 |
| seam \|x[0] − x[last]\| | 0.1185 | 0.0156 |
| RMS | 0.186 | 0.087 |

At ~32–33 Hz a 4-stroke fires 2×/rev (4-cyl), 3×/rev (6-cyl) or 4×/rev (V8), so
the recordings sit at **486 rpm (V8) … 648 rpm (6-cyl) … 972 rpm (4-cyl)**.  The
cylinder count cannot be read from the data, so `kIdleRpm = 600` (inside the
V8…six-cylinder bracket) is the tuning constant at which the loop plays as
recorded; `describe()` and the harness print the measured period so the constant
can be revisited.

**Strategy: one loop per vehicle class, resampled continuously by a mixer
effect.**  `engine()` keeps a single `Mix_PlayChannel(channel 0, chunk, −1)` alive
and registers `engineEffectCb` on that channel; the callback *replaces* the
channel's audio with its own phase-accumulator read of a preloaded stereo float
copy of the loop:

```
phase += ratio per output frame;   wrap at loopFrames
sample = lerp(loop[floor(phase)], loop[floor(phase)+1], frac) · per-channel gain
```

`ratio` and the L/R gains are one-pole smoothed *in the callback* (audio rate, so
the behaviour does not depend on the game's frame rate): pitch rises with
`τ = 25 ms` and falls with `τ = 300 ms` (throttle blip vs engine braking), gain
with `τ = 60 ms`.  Nothing is restarted per frame — the harness asserts
**0 restarts over 120 `engine()` calls** — and `update()` parks the channel after
2 s of silence (engine off or beyond 120 m) so an inaudible truck costs nothing,
and if `engine()` stops being called altogether (menu, pause, destroyed vehicle)
it fades the loop out after 250 ms instead of leaving a stuck drone.
`ratio = clamp(rpm / 600, 0.55, 3.20)` covers ≈330…1920 rpm; the mapping is
linear in rpm, which is physically right (firing frequency ∝ revolutions), but
at ~3× the idle recording the timbre is a transposed idle, not a real wide-open
engine — the honest consequence of having a single loop.

**Seam repair at load time.**  `idle.wav` is 15.112 periods long, so wrapping the
end to the start jumps the phase by 12 % of a cycle and the amplitude by 0.1185 —
an audible tick every 0.47 s.  `buildLoop()` autocorrelates the first 4096 frames
over 30…250 Hz, keeps `L = round(frames / period) · period` frames (an integer
number of periods) and crossfades the leftover `T = frames − L` frames over the
head, which makes `out[0] == src[L]` and therefore the wrap continuous by
construction.  Measured: `idle.wav` becomes 20 310 frames (15 × 1354, the short
window measures 32.6 Hz) with the seam step reduced **0.1185 → 0.0035**;
`jidle.wav` becomes 55 734 frames with 0.0156 → 0.0078.  When the file is already
near-integral (`T < 16`) the crossfade is skipped.

**Idle/rev behaviour.**  `engineStart()` plays `snd\start.wav` (0.64 s) and holds
the loop at `ratio 0.60`, 45 % gain for 700 ms (the cranking ramp); after that
`engine()` follows the rpm.  At idle (600 rpm, `load = 0`) the loop runs at
`ratio 1.0` and 35 % engine gain; opening the throttle raises the gain to 100 %
(`0.35 + 0.65·load`) and the rpm (usually through `Vehicle::rpm()`'s 700…2500
band) raises the pitch, with the slow fall giving the "engine braking" tail.
`engineStop()` (alias `stopEngine()`) fades the gain to 0 and plays
`snd\stop_eng.wav`.  `setEngineKind(Light)` swaps to `snd\jidle.wav` for the
light vehicle and rebuilds the loop.

## 6. Music

`music(file, loop)` resolves `file` (bare name, with or without `.mp3`, or a
path) against `<dataDir>/<TRUCK.INI MUSIC>` — `original/MUSIC` — loads it with
`Mix_LoadMUS` and starts it with **`Mix_FadeInMusic(…, loop ? −1 : 0, 600 ms)`**.
Note `loops = 0` (play once); SDL_mixer's `1` would play it twice.  Requesting
the track that is already playing only refreshes the volume/loop state.
`stopMusic()` calls `Mix_FadeOutMusic(900 ms)`; the `Mix_Music*` is freed in
`update()` once `Mix_PlayingMusic()` drops, and a non-looping track that ends by
itself is freed there too, so no `Mix_Music` ever leaks.  Switching tracks halts
and frees the previous one immediately.

MP3 decoding depends on the installed SDL2_mixer build; here
`Mix_Init(MIX_INIT_MP3)` returns `0x8` and all 12 tracks load.  When it is
missing (or a file is absent) `music()` logs once and does nothing else.

## 7. Real-time safety and guarantees

* **`play()` never touches the filesystem.**  The whole container is read once in
  `init()`; the 151 chunks are eagerly decoded into the mixer's device format
  (44.27 MiB, ~27 ms including `Res::load`), so nothing is loaded during play.
* `init()` requests exactly **44100 Hz / `AUDIO_S16SYS` / 2 channels / 1024
  frames**; `Mix_QuerySpec` confirms the device (`describe()` reports it) and the
  engine effect supports both `AUDIO_S16SYS` and `AUDIO_F32SYS`, mono or stereo.
* `Mix_AllocateChannels(32)` and `Mix_ReserveChannels(1)` — channel 0 belongs to
  the engine, so a burst of one-shots cannot steal it.
* The engine effect callback allocates nothing, locks nothing, calls no SDL
  function and only reads its own preallocated float loop; the game thread talks
  to it through three `std::atomic<float>` targets.
* Identical one-shots are rate-limited to one start per **25 ms** per name
  (a 60 fps retrigger loop cannot exhaust the 32 channels).
* Unknown names are logged **once per name** and ignored; a name whose chunk
  failed to decode is treated the same way.  `init()` failing (no device, no
  mixer) leaves every subsequent call a no-op; after `shutdown()` everything is
  released and `init()` can be called again (the harness does exactly that).

## 8. Numbers on the shipped data

| quantity | value |
|---|---|
| effects in the bank | 151 / 151, 0 failed |
| resident bank (44.1 kHz/16-bit/stereo) | 46 420 972 B = **44.27 MiB** |
| source payload | 7 328 804 B = 6.99 MiB (×6.3 after conversion) |
| distinct names (stems + aliases) | 153 (2 aliases) |
| init() cost | ~27 ms (`Res::load` + 151 decodes); first `engine()` adds ~4 ms of autocorrelation |
| engine loop (truck) | 20 310 frames, 15.0 periods, seam 0.0035 |
| engine loop (light) | 55 734 frames, 42.0 periods, seam 0.0078 |
| pitch range | ratio 0.55…3.20 = 330…1920 rpm at `kIdleRpm = 600` |
| positional radius | full ≤ 8 m, silent ≥ 120 m (linear in amplitude between) |
| music | 12 tracks, 28.4 MiB, fade 600 ms in / 900 ms out |

## 9. Verification

```sh
cmake -B build && cmake --build build -j     # the game builds with HT2_HAVE_SDL_MIXER=1
tools/build_audio_check.sh                   # builds + runs the two checks below
python3 tools/audio_probe.py [--map]         # RIFF inventory of the shipped effects
tools/out/audio_test original                # the C++ harness (sets SDL_AUDIODRIVER=dummy)
```

`tools/audio_test.cpp` initialises the module against `original/`, checks the
device, the bank counts/bytes, the `TRUCK.INI` volumes, the name/alias table, the
pan and attenuation laws (a mixer **post-effect meters the final output**, so the
pan/cull/attenuation checks observe real samples), the engine loop (period, seam
repair, monotonic rpm→ratio, gain floor, 0 restarts in 120 frames, silence and
parking out of earshot), the music fade-in/out and re-init after `shutdown()`; it
exits non-zero if any check fails.  `tools/build_audio_check.sh` additionally
asserts that a missing sound is logged exactly once for three calls.

## 10. What could not be determined

* **The original's engine model.**  `king.exe`'s audio code was not disassembled;
  whether the original resampled `idle.wav` by rpm, crossfaded several layers or
  simply played it at a fixed pitch is unknown.  What is known is what ships:
  two idle loops and no rpm-indexed files (§5).
* **The recorded rpm / cylinder count** of the loops — inferred as 486…972 rpm
  from the 32.4/33.2 Hz firing fundamentals; `kIdleRpm = 600` is a tuning
  constant, not a measurement.
* **The original attenuation curve** (8 m/120 m, linear) — chosen here because it
  is audible, cheap and documented; `setAttenuation()` exposes it.  Nothing in
  `TRUCK.INI` or the `.res` container carries distance/rolloff parameters.
* **Which logical sounds belong to which gameplay event.**  The `SOUNDS` table
  gives names and indices only; the `.sch` scripts that trigger them are
  encrypted (SDTE) and there are names in `king.exe` (`bighornSound`,
  `waterSound`) that are **not** in `common1.res`'s `SOUNDS` table, so they would
  resolve to nothing.
* **No loop flags** exist in the container: which effects are "loops"
  (`wind`, `rain`, `sea`, `burning*`, `idle`, `jidle`) had to be judged by name
  and duration.  `play()` is one-shot by design; looping ambience would need a
  new entry point (the engine's phase-accumulator effect is reusable for it).
* **Front/back and elevation** are not representable with SDL2_mixer (measured
  above): behind sounds are as loud as ones in front, and height is ignored.
  A true 3D pan would need OpenAL, which the project does not link.
* **`skivol`, `power`, `sens`, `voll/volr`, `musl/musr`** (skid volume,
  force-feedback, per-speaker trims) are not wired: the bank has no dedicated
  skid recording, and the force feedback API is SDL's haptics, not audio.
* **Frame-rate dependence** of the *caller*: `engine()` is assumed to be called
  once per frame and `update()` after it; the audio-rate smoothing tolerates
  missed frames, but the crank timer and the retrigger guard use `SDL_GetTicks`.

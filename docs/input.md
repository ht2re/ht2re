# Unified input — keyboard + game controller

`src/app/Input.{h,cpp}` is the only place that reads the keyboard or an SDL game
controller.  It supports a handheld pad (Anbernic-class device: two sticks,
D-pad, ABXY, LB/RB, analog LT/RT, Start/Select) and a keyboard at the same time,
and it produces **analog steering and analog throttle/brake**, so the game is
playable on a pad alone.

The module contains no game logic: it never looks at the vehicle, the economy or
the UI, it only returns an `Input::State` snapshot.  Everything game-specific
(what "buy" means, which camera mode comes next) stays in `App`.

Contents: [1 API the parent calls](#1-api-the-parent-calls) · [2 mapping table](#2-mapping-table)
· [3 filters and constants](#3-filters-and-constants) · [4 rumble](#4-rumble)
· [5 hot-plug](#5-hot-plug) · [6 adding a mapping DB for a handheld](#6-adding-a-mapping-db-for-a-handheld)
· [7 verification](#7-verification) · [8 limitations and open items](#8-limitations-and-open-items)

---

## 1. API the parent calls

```cpp
#include "app/Input.h"
ht2::Input input_;

// once, after the renderer/video is up (never fails, keyboard-only is fine):
input_.init(opt_.dataDir);              // dataDir is used to find gamecontrollerdb.txt
// alternative: input_.init()           // no data dir

// every frame, after the SDL_PollEvent loop:
Input::State st = input_.poll(dt);
Vehicle::Input vi;                      // Vehicle.h is not included by Input.h on purpose
vi.throttle  = std::max(-1.0f, std::min(1.0f, st.throttle - st.brake));  // st.brake is 0..1
vi.steer     = st.steer;                // already rate limited and expo shaped
vi.handbrake = st.handbrake;
if (st.viewToggle) viewMode_ = (viewMode_ + 1) % 3;
if (st.buy)     { /* vehicle market   */ }
if (st.job)     { /* accept cargo job */ }
if (st.deliver) { /* complete job     */ }
if (st.menu)    { /* pause / menu     */ }
if (st.map)     { /* map screen, skip */ }
if (st.any)     { /* player is acting - leave attract mode */ }

// crash / off-road feedback (no-op on a keyboard or a pad without rumble):
input_.rumble(0.8f, 0.4f, 220);         // low, high (0..1), duration ms

input_.shutdown();                      // optional: the destructor does it, and it is
                                        // safe before or after Renderer::shutdown()/SDL_Quit()
```

| member | meaning |
| --- | --- |
| `bool init()` / `bool init(const std::string& dataDir)` | opens the controller subsystem, loads the mapping DB, opens the first pad.  Always returns `true`; a missing pad or a missing joystick subsystem only produces a log line. |
| `void shutdown()` | removes the event watch, closes the pad, quits the controller subsystem if `init()` started it.  Guarded with `SDL_WasInit`, so it is safe after `SDL_Quit()`. |
| `State poll(float dt)` | one frame of input.  `dt` is clamped to `[0, 0.1]`.  SDL events must already have been pumped by the caller.  Call it **exactly once per frame** — the one-shot edges are consumed by the call that sees them. |
| `void handleEvent(const SDL_Event&)` | optional: forward pumped events so hot-plug is handled even without the internal watch.  Idempotent. |
| `bool controller()` / `const std::string& controllerName()` | pad present / `SDL_GameControllerName`. |
| `const std::string& mappingSource()` | e.g. `original/../gamecontrollerdb.txt (312 mappings)` or `SDL built-in database`. |
| `int joystickCount()` | `SDL_NumJoysticks()` as of the last poll. |
| `void setDeadZone(float)` / `void setTuning(const Tuning&)` / `const Tuning& tuning()` | filter constants, see §3.  `Tuning` is a plain struct, no config file. |
| `void rumble(float low, float high, uint32_t ms)` / `stopRumble()` / `rumbleSupported()` | see §4. |
| `void onControllerAdded(int)` / `onControllerRemoved(uint32_t)` | hot-plug entry points, normally driven by the event watch. |
| `void injectRawPad(const RawPad&)` / `clearInjectedPad()` | synthetic pad snapshot: `poll()` uses it instead of SDL but runs the identical filters.  Used by `tools/input_test.cpp`; also handy for a demo/replay. |

`State` fields: `throttle` (−1..1, signed: negative means brake/reverse),
`steer` (−1..1), `brake` (0..1), `handbrake`, `any`, and the one-shot edges
`viewToggle`, `buy`, `job`, `deliver`, `menu`, `map` (true on exactly one poll
per press, never repeated while the key/button is held).

`Input::Filter` (`ht2::InputFilter`) holds the pure filter functions — no SDL, no
state: `applyStick`, `applyTrigger`, `shapePedal`, `shapeSteer`, `rateLimit`,
`blend`, `clampf`.  They are public so the harness (and any future
self-calibration screen) can call them directly.

## 2. Mapping table

| action | keyboard | pad (SDL controller API) | `State` field | kind |
| --- | --- | --- | --- | --- |
| throttle | `W` / `↑` | right trigger `RT`, or right stick **up**, or D-pad up | `throttle` (+1) | analog |
| reverse / brake | `S` / `↓` | left trigger `LT`, or right stick **down**, or D-pad down | `throttle` (−1) / `brake` | analog |
| steer | `A` `D` / `←` `→` | left stick **X**, or D-pad left/right | `steer` | analog (stick) |
| handbrake | `Space` | `A` / Cross | `handbrake` | level |
| camera view | `C` | `Y` / Triangle, `RB` (alt) | `viewToggle` | edge |
| buy vehicle | `B` | `X` / Square | `buy` | edge |
| take job | `T` | `B` / Circle | `job` | edge |
| deliver | `Y` | `LB` | `deliver` | edge |
| menu / pause | `Esc` | `Start` | `menu` | edge |
| map / skip | `Tab` | `Back` / Select | `map` | edge (extension) |

Notes:

* `handbrake` is a level (`A` held = on); everything in the "edge" rows fires
  once per press.  Keyboard and pad edges are tracked separately, so holding
  `C` while tapping `Y` still gives one edge per source.
* `RB` is a second view toggle and the D-pad drives throttle/steer as a digital
  fallback (`Tuning::dpadDriving`); both are extensions on top of the required
  mapping and can be switched off in `Tuning`.
* When keyboard and pad are active at the same time, the source with the larger
  magnitude wins (`Filter::blend`), so `W` always means full throttle and a
  half-pressed `RT` never cancels it.
* The keyboard throttle is not shaped or rate limited (it is already binary);
  keyboard steering **is** rate limited, which gives keyboard players an
  analog-feeling ramp instead of an instant full-lock flick.
* `App` currently handles `SDL_KEYDOWN == SDLK_ESCAPE` inside its event loop.
  Once `poll()` is wired in, that handler should be removed (or made to ignore
  the case) or Esc will toggle twice.

## 3. Filters and constants

Defaults (`Input::Tuning`, changeable in code through `setTuning()`):

| constant | default | effect |
| --- | --- | --- |
| `deadZone` | **0.18** | per-axis stick dead zone |
| `triggerDeadZone` | **0.08** | analog trigger dead zone |
| `steerExpo` | **0.35** | steering expo (0 = linear, 1 = pure cubic) |
| `pedalLinear` | **0.65** | pedal curve blend (1 = linear, 0 = pure `x²`) |
| `steerRiseRate` | **3.0 /s** | slew limit toward more lock (0 disables) |
| `steerFallRate` | **6.0 /s** | slew limit back to centre (0 disables) |
| `stickPedals` | `true` | right stick Y as throttle/brake |
| `dpadDriving` | `true` | D-pad as digital steer/throttle |

**Dead zone with re-normalisation** (`Filter::applyStick`).  SDL axes are
normalised to −1..1 first (`SDL_GameControllerGetAxis / 32767`, clamped).  Then

```
|raw| <= dz      -> 0
otherwise        -> sign(raw) * (|raw| - dz) / (1 - dz)
```

so the travel left after the dead zone is stretched back over the full 0..1
range: a stick pushed to its full deflection with a 0.18 dead zone commands 1.0,
instead of the 0.82 a plain threshold would leave.  The rescale is continuous at
the dead-zone edge (no jump), and it is applied per axis, so a diagonal push
with X inside the dead zone does not steer (`steer == 0`, verified).

**Triggers** (`Filter::applyTrigger`).  SDL reports triggers as −32768
(released) .. 32767 (fully pressed), i.e. −1..+1; that is mapped to 0..1 with
the same dead zone + rescale, then shaped.

**Throttle/brake curve** (`Filter::shapePedal`, `l = pedalLinear = 0.65`):

```
shaped = l*x + (1 - l)*x²
```

A blend of linear and square: monotonic, `(0)=0`, `(1)=1`, derivative at 0 is
0.65.  On the curve itself, x = 0.5 gives 0.4125 instead of 0.5; through the
real trigger path (raw 0 = half travel, minus the 0.08 trigger dead zone =
0.4565) it gives **0.3697** — the low end of the trigger travel stays
controllable while a full press still gives everything.  A pure `x²` is too dull
at the top, a pure line is too twitchy just off the stop.

**Steering expo** (`Filter::shapeSteer`, `e = steerExpo = 0.35`):

```
shaped = (1 - e)*x + e*x³
```

Full lock is unchanged (±1 = ±1) and the centre is softer: on the curve,
x = 0.5 gives 0.369 and x = 0.25 gives 0.168.  Through the stick path, a raw
axis of 0.5 becomes 0.390 after the dead-zone rescale and commands 0.274, while
a raw 0.30 (rescaled 0.146) commands 0.096.  This is the usual "dual-rate" feel
and keeps the truck straight while cruising.

**Steering rate limit** (`Filter::rateLimit`, applied to the combined
keyboard + pad steering target every frame):

```
riseRate 3.0/s  while |target| >= |current|   -> 0.050 per 60 Hz frame
fallRate 6.0/s  otherwise                     -> 0.100 per 60 Hz frame
```

Centre to full lock takes 20 frames ≈ **0.33 s**, release back to centre takes
10 frames ≈ **0.17 s**.  A stick flick therefore cannot spin the truck: one
frame of full stick yields `steer = 0.05`, not `1.0`, and a small movement
(e.g. `lx = 0.30` → target 0.096) settles without any overshoot.  `dt` is
clamped to 0.1 s so a frame hitch cannot snap the wheel either.  Setting
`steerRiseRate`/`steerFallRate` to 0 disables the limiter (instant steering,
the old keyboard behaviour).

**Edge detection** (`Input::EdgeState`): one flag per action per source, armed
when the input is up and fired when it goes down; `state = now && !prev`.  A
button held for 61 consecutive polls fires exactly once (verified for all
seven edge bindings plus the keyboard `C`).

## 4. Rumble

```cpp
input_.rumble(0.85f, 0.35f, 260);   // crash: strong low-frequency, short
input_.rumble(0.30f, 0.10f, 60);    // off-road: light, short, re-issued while off-road
input_.stopRumble();
```

* `low`/`high` are `0..1` and are scaled to SDL's `0..65535`; out-of-range
  values are clamped.  `ms` is the duration, `0` means "stop".
* Backed by `SDL_GameControllerRumble()` (SDL ≥ 2.0.9).  Capability is checked
  at open time with `SDL_GameControllerHasRumble()` (SDL ≥ 2.0.18); if the pad
  reports no rumble, or there is no pad at all, the call is a **no-op** and logs
  one line.  If the rumble call itself fails, rumble is disabled for that pad.
* `rumbleSupported()` reports the current capability, so a caller can skip the
  call entirely.  Re-issuing `rumble()` within the duration refreshes it.
* The module never rumble-ticks by itself: crash/off-road logic belongs to the
  caller.

## 5. Hot-plug

A handheld can boot with the pad already attached, connect it later (or have it
appear only after the USB/Bluetooth stack is up), so hot-plug is handled in
three independent ways:

1. `init()` calls `SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER)`,
   `SDL_GameControllerEventState(SDL_ENABLE)` and installs an
   `SDL_AddEventWatch` callback.  The watch reacts to
   `SDL_CONTROLLERDEVICEADDED`, `SDL_CONTROLLERDEVICEREMOVED` and
   `SDL_CONTROLLERDEVICEREMAPPED`.  It only sets atomic flags — it never calls
   the joystick API — so it is safe to run inside `SDL_PumpEvents()`.
2. If the caller prefers to own the event loop, it can forward events to
   `handleEvent()`; having both paths active is harmless because the flags are
   idempotent.
3. Independently of events, every `poll()` rescans: `SDL_NumJoysticks()`,
   `SDL_IsGameController()` and `SDL_GameControllerGetAttached()`.  This covers
   a missing event watch, a device that appears without an event, and a pad that
   was yanked out (the state degrades to keyboard-only, `controller()` becomes
   false).  A failed `SDL_GameControllerOpen()` is retried at most every 2 s so
   an unsupported device cannot spam the log.

On `ADDED` the module tries to open the pad immediately; on `REMOVED` it closes
the pad if the instance id matches (and reopens the first remaining pad); on
`REMAPPED` — e.g. after `gamecontrollerdb.txt` was (re)loaded — the open pad is
closed and reopened so the new bindings take effect.  Only the **first**
controller reported by SDL is used; extra pads are ignored (a second player or a
spurious virtual device from Steam/DS4Windows cannot steal the input).

Typical log lines (all on stdout, `[ht2re]` prefix):

```
[ht2re] input: loaded 312 controller mappings from original/../gamecontrollerdb.txt
[ht2re] input: controller 0: "Microsoft X-Box 360 pad" (instance 0, mapping: ... (312 mappings), rumble: yes)
[ht2re] input: ready (controller: Microsoft X-Box 360 pad, mapping: ...)
[ht2re] input: hot-plug: controller added (device 1)
[ht2re] input: controller closed: "Microsoft X-Box 360 pad"
```

## 6. Adding a mapping DB for a handheld

SDL ships a built-in mapping database, which already covers XInput pads, most
8BitDo/Anbernic devices and the common generic gamepads.  A device that is not
recognised shows up as a joystick but **not** as a game controller
(`SDL_IsGameController()` false) — the module then stays in keyboard-only mode.
The fix is a `gamecontrollerdb.txt`:

1. Get the community database (or just the relevant line) from
   <https://github.com/mdqinc/SDL_GameControllerDB> (`gamecontrollerdb.txt`).
   On the device you can also find the GUID with `sdl2-jstest --list` or
   `evtest`, and write a mapping line for it:
   `03000000...,My Pad,a:b0,b:b1,x:b3,y:b2,leftx:a0,lefty:a1,righttrigger:a5,lefttrigger:a4,start:b7,back:b6,leftshoulder:b4,rightshoulder:b5,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,platform:Linux,`
2. Put the file where the game looks for it, in this order (first hit wins):
   1. `$HT2_GAMECONTROLLERDB` (explicit path, useful on a handheld front-end);
   2. `<dataDir>/gamecontrollerdb.txt` (e.g. `original/gamecontrollerdb.txt`);
   3. `<dataDir>/../gamecontrollerdb.txt` (next to the data dir — the usual place);
   4. `./gamecontrollerdb.txt` (working directory / next to the binary);
   5. `<SDL_GetBasePath()>/gamecontrollerdb.txt` (next to the executable);
   6. `/usr/share/gamecontrollerdb/gamecontrollerdb.txt` and
      `/usr/share/sdl2-gamecontrollerdb/gamecontrollerdb.txt`.
3. Start the game and look for `input: loaded N controller mappings from <path>`
   followed by `input: controller 0: "<name>"`.  If the file is loaded but no
   controller is logged, the GUID line is missing or malformed
   (`SDL_GameControllerAddMappingsFromFile` skips bad lines silently; a
   `mapping: <path> (0 mappings)` line means the file had no usable entries).
4. No restart is needed for the mappings themselves, but a pad that is already
   open keeps its old bindings — with the module's `REMAPPED` handling the pad
   is reopened automatically.  If you replace the DB while the game runs, an
   unplug/replug always applies it.

On a handheld this typically means dropping `gamecontrollerdb.txt` next to
`ht2re` or into the data directory and starting the game with
`--data original/gamecontrollerdb.txt`-adjacent layout (see the search list
above); nothing needs to be rebuilt.

## 7. Verification

`tools/input_test.cpp` runs headless (no pad needed — it injects synthetic SDL
axis/button values through the same public filter path) and prints the numbers
below.  Build and run:

```sh
c++ -std=c++17 -O2 -I src tools/input_test.cpp src/app/Input.cpp \
    -o tools/out/input_test $(pkg-config --cflags --libs sdl2)
tools/build_input_test.sh                 # or: builds + runs it
SDL_VIDEODRIVER=dummy ./tools/out/input_test original
```

Result on this machine (macOS, SDL 2.32.72, **no controller attached**),
`cmake --build build -j` clean, 48 checks / 0 failures:

```
SDL_Init(SDL_INIT_VIDEO) only, like the app
controller subsystem before Input::init(): down (init() must bring it up)
poll() before init(): ok (throttle 0.00, controller no)

controller detected : no - keyboard only
mapping source      : SDL built-in database
joysticks reported  : 0        rumble supported: no (no pad)
init() brought up SDL_INIT_GAMECONTROLLER itself: ok (hot-plug events will be generated)

left stick X (SDL int16 -> filter)          right trigger (SDL int16 -> filter)
     raw      norm     steer note             raw      norm  throttle note
  -32768   -1.0000   -1.0000               -32768   -1.0000    0.0000
  -16384   -0.5000   -0.2745               -16000   -0.4883    0.1370
   -6000   -0.1831   -0.0025                -6000   -0.1831    0.2767
   -2000   -0.0610    0.0000 dead zone          0    0.0000    0.3697
       0    0.0000    0.0000 dead zone       8000    0.2441    0.5045
    2000    0.0610    0.0000 dead zone      16384    0.5000    0.6590
    6000    0.1831    0.0025                24576    0.7500    0.8231
   16384    0.5000    0.2745                32767    1.0000    1.0000 full

full stick flick -> steer 0.0500 after one frame, full lock after 20 frames (0.33 s)
release          -> centre in 10 frames (0.17 s)
edge detection   -> every button held 61 frames fired exactly 1 time
                    (Y, X, B, LB, Start, Back, RB and keyboard C)
600-poll script  -> throttle max 1.000, brake max 0.553, steer -0.808 .. 0.808,
                    edges view/buy/job/deliver/menu/map = 1/1/1/1/1/1, any in 600/600 frames
hot-plug         -> simulated ADDED/REMOVED/handleEvent: no crash, 0 joysticks
rumble           -> no-op without a pad (logged once)
shutdown         -> SDL_Quit() then shutdown() twice: no crash (idempotent)
RESULT: PASS (48 checks, 0 failures)
```

The harness compiles with `-Wall -Wextra` and produces no warnings for
`src/app/Input.cpp` or `tools/input_test.cpp`.

Not covered by the harness because no pad is attached on this machine: a real
`SDL_GameControllerOpen`/`GetAxis`/`GetButton` round trip, real rumble, and real
hardware hot-plug.  The event paths are exercised with simulated
`SDL_CONTROLLERDEVICEADDED`/`REMOVED` events and the untested hardware calls are
all guarded (`SDL_WasInit`, `NULL` checks) and log-and-continue.

## 8. Limitations and open items

* **One pad only** — the first game controller SDL reports.  No multi-player,
  no per-pad selection.
* **Triggers must be real SDL axes.**  Pads whose L2/R2 are exposed as buttons
  (some cheap generic controllers) have no analog brake/throttle; the right
  stick (pedals) and the D-pad/keyboard still drive the truck.  Adding such a
  pad to `gamecontrollerdb.txt` cannot create an axis that the hardware does
  not report.
* **Esc is both an edge here and a `SDL_KEYDOWN` handler in `App`** — the parent
  must drop one of them (§2, last note).
* Keyboard steering is intentionally rate limited (~0.33 s to full lock).  Set
  `Tuning::steerRiseRate` / `steerFallRate` to 0 for the old instant steering.
* `State::any` is "any mapped action or any key/button anywhere", including keys
  used by menus, so it is not a reliable "the player wants to start the game"
  signal on its own.
* No mouse/touch input, no rebinding UI, no config file — the mapping and the
  constants live in code (`Tuning`, §3) by design for now.
* The keyboard part of the harness writes into SDL's internal key-state array
  (a test-only trick, marked in the source); nothing in `src/app/Input.cpp`
  depends on it.
* `SDL_GameControllerRumble` is used unmodified; trigger rumble
  (`SDL_GameControllerRumbleTriggers`) is not exposed yet.

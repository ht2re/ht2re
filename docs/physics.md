# Player-vehicle physics (`src/game/Vehicle.{h,cpp}`)

Everything below is recovered from the shipped data and the shipped
`original/king.exe` (PE32, image base `0x400000`; **VA = file offset + 0x400000**).
Reproduce with:

```bash
# the A/B bench: the pre-recovery model (git HEAD) vs the recovered one
tools/physics_ab.sh                 # RESULT lines for both models
tools/physics_ab.sh ZIL             # pass a vehicle.tech section through

# the same measurements in the real game
cmake --build build -j
./build/ht2re --scene world --drive 30 --no-collision
```

Contents: [1 the tech table](#1-vehicletech--the-shipped-drivetrain-table) ·
[2 the recovered model](#2-the-recovered-model) ·
[3 the constants the exe could not answer](#3-constants-the-exe-could-not-answer) ·
[4 measured before/after](#4-measured-beforeafter) ·
[5 the old model](#5-the-model-that-was-replaced)

---

## 1. `vehicle.tech` — the shipped drivetrain table

`TRUCK.INI [CAR] tch_file=truck2.tch` / `tech_number=2003` are **not** read by
this build of `king.exe` (no such strings), so the port reads the binary tech
table directly.

* `fcn.00508000` (`0x508000`) opens `.\vehicle.tech` read-only (`"rb"`,
  `0x508010`), reads a `u32 record size` (`0x50802e`) and **requires it to be
  2416** (`0x508104`: `cmp eax, 0x970`; the record slots are `fread` at
  `0x508196`).  The global record array starts at `0x697868`.
* The header is `[u32 nameLen][name][u32 ver][u32 recSize][u32 count][7 u32]`:
  `nameLen = 13`, `name = ".\truck2.tch\0"`, `recSize = 0x970`, `count = 43`.
  **Record *i* therefore lives at file offset `0x39 + i*2416`.**
* A record is a numeric block (`+0x000..+0x867`) followed by NUL-terminated
  strings: `+0x868` b3d booth node, `+0x8a4` source `.tch`, `+0x8c2` section,
  `+0x8e0` section_tech, `+0x8fe` display name, `+0x912` short name,
  `+0x926` node.
* `fcn.004ebd00` copies the whole record verbatim into the runtime vehicle:
  `0x4ec78d shl esi,4` / `0x4ec790 add esi,0x697868` / `rep movsd 0x25c`
  (604 dwords = 2416 bytes) with the destination `object+0x1f48`.  So
  **`record offset X` == `runtime object offset 0x1f48 + X`.**
* 92 slots exist in the file, **43 carry a record** (`[+0x8a4] == '.'`), which
  is exactly the count the loader stores (`0x6ce32c = 43`).

### 1.1 Field offsets

Offsets marked **[reader]** are the destination of the `fcn.00603d30` key read
(`GetPrivateProfileStringA` + `atof`, `0x603d30`) for the legacy text `.tch`
format, anchored on the mass destination at `0x50879d`
(`add edx, 0xffffffa0` with `edx = record+0xc0` → `record+0x60`) and validated
by the 43 shipped records.

| offset | key | shipped values (43 records) | evidence |
|--------|-----|------------------------------|----------|
| `+0x060` | `mass` | 715..12000 kg, scales with class | `0x50879d` |
| `+0x064` | `rwheel` | 0.292..0.711 m | `0x5087fa` |
| `+0x068` | `rwheel1` | 0.170..0.711 m | `0x50881c` |
| `+0x06c` | `horse_power` | 30..600 hp, scales with class | value semantics; key read at `0x509105` |
| `+0x0ac` | `air_press_coeff` | 0.3 everywhere (loader default 0.3) | `0x50888a` |
| `+0x0b0` | `air_resist_coeff` | 0.3 everywhere (default 0.3) | `0x5088ac` |
| `+0x0b4` | `air_side_resist_coeff` | 0.8 everywhere (default 0.8) | `0x5088d5` |
| `+0x0b8` | `front_wheel_thickness` | 0.15..0.4 | `0x50883e` |
| `+0x0bc` | `rear_wheel_thickness` | 0.2..0.8 | `0x508863` |
| `+0x0c0` | `maxrotmomentum` | 65..2400, ∝ power | `0x5087bd` |
| `+0x0c8` | price-like (cars 16000..30000, lorries 36000, tractors 40000) | `0x509121` reads `price` into a *different* template | value semantics only |
| `+0x0cc` | `rear_axle_coeff` | 3.08..15.72 (default 5.0) | `0x50891a` |
| `+0x0d0` | `reverse_trans_coeff` | −3.06..−14 (default −4.0) | `0x50893e` |
| `+0x0d8 + 4k` | `gear_trans_coeff(k+1)`, k = 0..12 | see 1.2 | `0x50897d` |
| `+0x10c` | `tach_with_max_power`, **rev/s** | 20..54.17 (loader default 1200 **rpm** = 20 rev/s) | `0x5088f3` |
| `+0x860` | `numtech` | 0 for the player truck | `0x4ec861` (`[edx+0x6980c8]`, `0x6980c8-0x697868 = 0x860`) |

### 1.2 The gearbox auto-fill (this is what proves the gear offset)

`0x50897d` reads `gear_trans_coeff%d` for d = 1..13 with the default `-100.0`
(`0xc059000000000000` at `0x508990`).  When the value read is below `0x64daf0`,
`0x5089b7` replaces it with

```
cur = prev * qword [0x64dae8]        ; 0x64dae8 = 0x3fe6db6db6db6db7 = 5/7
```

i.e. **an unauthored gear is 5/7 of the previous one**, and the number of real
gears is the index where that starts (`0x5089c8` caps it at 11).  In the shipped
records the 5/7 signature appears from gear 6 on for *every* vehicle, so every
vehicle's gearbox has 5 authored ratios (trailers have none).  This both locates
`+0x0d8` and gives the authored count.

### 1.3 The player's truck (`ZilR` → section `ZIL`, `.\lorries.tch`, record 21)

| | |
|---|---|
| mass | 5100 kg |
| wheel radius | 0.533 m (rear 0.325 m) |
| engine | 255 hp = 187.5 kW |
| gears | 7.44, 4.10, 2.29, 1.47, 1.00 (5-speed) |
| final drive | 5.29 |
| reverse | −4.0 |
| `maxrotmomentum` | 950 |
| max power at | 26.667 rev/s = 1600 rpm |
| aero | air_press 0.3, air_resist 0.3, air_side 0.8 |

`scripts`/`evehicles.sch` confirms the mapping: `ZIL_LORRY*` carry
`model=ZilR`, class `lorry`.

---

## 2. The recovered model

The runtime is a standard longitudinal model whose **inputs are the shipped
record**, so nothing is hand-picked except the three constants listed in §3.

```
omegaMaxPower = 2*pi * tach_with_max_power                    ; rad/s
omegaRedline  = omegaMaxPower * 1.25                          ; §3.1
omegaIdle     = max(omegaMaxPower * 0.30, 60)                 ; 573 rpm floor
I_engine      = maxrotmomentum / omegaMaxPower                ; kg m^2
```

* **Flywheel inertia.** `maxrotmomentum` is the engine's angular momentum at the
  max-power speed, so dividing by `omegaMaxPower` gives a flywheel inertia.
  Across the 43 shipped vehicles that is 0.207 kg m² (FIAT, 30 hp) …
  17.9 kg m² (RENAULT lorry, 360 hp) — the physical range for a car..lorry
  engine, which is what makes the reading credible.
* **Torque.** `T = min(P/omega, 1.3 * P/omegaMaxPower)` with `P = hp * 735.5 W`,
  scaled by throttle, by `(1 - 0.85*damage)` and to zero at an empty tank.  The
  `1.3x` low-end ceiling is the only shape assumption (the record is not read
  for a torque curve at a pinned offset).
* **Gearbox.** Automatic: up-shift above `0.96 * omegaRedline`, down-shift below
  `0.45 * omegaMaxPower`, kick-down under full throttle, 0.30 s/0.25 s shift
  timers, `R` selected by holding the brake at a standstill for 0.35 s.
* **Traction.** `F_drive <= 0.90 * m * g * 0.55` (tyre friction x the weight on
  the driven axle) and the drivetrain's reflected inertia
  `I_engine * (gear*finalDrive)^2 / r^2` is added to the mass.  That is what
  makes 1st gear (39.4:1) so short on a lorry: the effective mass in 1st is
  ~36 t against 5.1 t in top.
* **Resistances.** rolling `c_rr * m * g` with `c_rr = 0.006` on a road
  (`GroundMap::onRoad`) and `0.035` off it; aero `0.5 * rho * C_d * A * v^2`
  with `rho = 1.2`, `C_d = air_resist_coeff` and the frontal-area proxy
  `A = 0.6 * m/1000 + 2.0 m^2`.
* **Brakes.** `0.50 g` on the pedal (converter unlocked, so the engine inertia
  does not fight the brakes) and `0.75 g` on the handbrake.  See §3.2.
* **Steering.** Bicycle model `yawRate = v * tan(lock) / WHEELBASE` with the
  lock wound off as `1/(1 + v^2 * 0.0022)` and the yaw rate capped by
  `a_lat <= 6.0 m/s^2`, so the steering rate rises with speed and then falls as
  `a_lat/v` — a measured 16.6 deg/s at 10 km/h, 32.4 at 30 km/h, 16.0 at
  80 km/h.
* **Body.** `WHEELBASE = 7.9`, `TRACK = 1.7`, `RIDE_HEIGHT = 1.0` stay as
  measured from the assembled mesh; the climb/drop/cliff/sea/edge rules are
  unchanged (`shot_test.py` pins them).

### 2.1 Top speed is a closed form

`v_max = omegaRedline * r / (topGear * finalDrive)`.  For the ZIL:

```
v_max = (2*pi*26.667*1.25) * 0.533 / (1.00 * 5.29) = 21.10 m/s = 75.98 km/h
```

and the simulated top speed is **75.968 km/h** — the model reproduces the
record's own arithmetic, it is not fitted.

---

## 3. Constants the exe could not answer

These three are the only model inputs that are *not* recovered.  Each is stated
with what it was calibrated to.

### 3.1 Redline ratio 1.25 (from the port's own tachometer)
`src/app/App.cpp` draws the tach as `rpm()/2500` with the redline marked at
`2000` (`drawGauge(..., vehicle3d_.rpm()/2500.f, 10, 2500.f, "RPM", 2000.f)`), a
value the port took from the original dashboard.  `tach_with_max_power` for the
ZIL is 26.667 rev/s = 1600 rpm, so `redline / maxPowerSpeed = 2000/1600 = 1.25`.
The result is coherent with the gear thresholds the port had already eyeballed
from the dashboard: the recovered 5-speed shifts at 10.2 / 18.5 / 33.2 / 51.7 /
76.0 km/h against the old table's 12 / 24 / 38 / 54 / 72 km/h.

### 3.2 Braking 0.5 g
`king.exe` reads a `breakpower` key (`0x67505c`, read at `0x5087e9`, default
`0x40b7700000000000 = 5872.0`), but no offset in the binary record could be
matched to it: a scan of all 43 records for a positive field in a brake-force
range that correlates with mass (`r > 0.9`) returns only `mass` itself, and the
`0x509xxx` reader writes it into a template that is *not* the 2416-byte record.
`0.5 g` is the standard laden heavy-vehicle figure.

### 3.3 Frontal-area proxy
The `.tech` format has a `body_square` key, but its offset could not be pinned
either.  `A = 0.6 * m/1000 + 2.0` gives 2.9 m² for the 1500 kg GAZELLE and
9.2 m² for the 12 t STORM.  Aero drag is a small term at these speeds (≈570 N
for the ZIL at 25 m/s against 187 kW available), so the top speed stays
gear-limited, as the closed form in §2.1 says it must.

---

## 4. Measured before/after

`tools/physics_ab.sh` (flat asphalt, no ground map, 60 Hz, full throttle).
The "before" column is `git HEAD:src/game/Vehicle.cpp` — the model the port
shipped before this pass.

| metric | before | after | original's number |
|--------|-------:|------:|-------------------|
| top speed | 45.98 km/h | **75.97 km/h** | 75.98 km/h (closed form, §2.1) |
| 0–50 km/h | never (top 46) | **8.98 s** | — |
| 0–60 km/h | never | **10.52 s** | — |
| 0–100 km/h | never | never (v_max 76) | — |
| shift points 1→2 … 4→5 | 12.2 / 24.2 / 38.0 | 9.9 / 18.0 / 32.0 / 49.8 | — |
| gear at top speed | 4 | **5** | 5 authored gears |
| tach at top speed | 1598 rpm | **2000 rpm** | redline 2000 (`App.cpp` gauge) |
| braking 60→2 km/h | 4.21 m | **27.8 m** | 0.5 g ⇒ 27.9 m |
| braking from top speed | 4.21 m | **44.3 m** | 0.5 g ⇒ 44.5 m |
| coast 30 s from top | 46→0 km/h (17.8 m) | **76→63.7 km/h (580 m)** | — |
| yaw rate at 10/30/80 km/h | 9.7 / 15.4 / 17.9 deg/s | 16.6 / 32.4 / 16.0 deg/s | — |

In the game (`--drive 30 --no-collision` from the GAME.CNF start) the truck now
reaches 76.0 km/h and covers 474 m, against 46 km/h before; the
`shot_test.py` drive checks (`> 300 m`, `> 35 km/h`, stays on the ground) hold.

---

## 5. The model that was replaced

The old `Vehicle::update` was a toy:

```cpp
float maxSpeed = 30.0f;      // m/s, ~108 km/h - never reached
float accel = 7.0f;          // m/s^2
float brakeDecel = 14.0f;    // m/s^2 = 1.43 g
float drag = 0.45;           // dv/dt = -0.45*v
...
speed_ -= (speed_ > 0 ? 1 : -1) * 1.2f * dt;   // constant rolling loss
```

Its terminal speed solved `7.0 = 0.45*v + 1.2` ⇒ **12.9 m/s = 46 km/h** — the
`maxSpeed = 30` cap was unreachable, which is exactly what the bench measured
(45.98 km/h).  Braking at 1.43 g stopped the truck in 4.2 m from 60 km/h, the
gearbox was a km/h ladder with no effect on the physics, and the vehicle's own
mass, power, gearing and aero did not exist.

---

## 6. What the exe still could not answer

* `breakpower` and `body_square` record offsets (§3.2, §3.3).
* `fuelConsumption` / `fuelTimeEndurance` / `tank` / `FuelScale` offsets: the
  keys are read at `0x508ad3`, `0x508a2f`, `0x508a08` and the defaults are
  known, but the destinations are relative to a template that is not the record,
  so the burn rate stays the historic port formula.
* `driving_wheels` / `steering_wheels` / `touching_road_wheels`
  (`0x508fad`, `0x50901c`, `0x508fcb`) would give the driven-axle share exactly;
  the offset could not be pinned, so `0.55` is used.
* `suspension` (`0x508ea2`) and `body_height` (`0x5097ee`): not needed by the
  current model.
* `TRUCK.INI [CAR]` (`friction_coeff=1.4`, `steering_sence=0.5`,
  `idle_steering=0.5`, `RoadWidthReducement=1.5`, `collision_coeff=0.05`) is
  **not read by this build** — none of those strings exist in `king.exe`.  The
  file is a leftover from another version or the port's own author; the numbers
  in this document do not come from it.

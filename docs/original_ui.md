# Original UI: HUD, cockpit, loading screen and menus

Reverse-engineering of the on-screen interface of **Hard Truck 2: King of the Road**
(`original/king.exe`, PE32 x86, MSVC 6.0, built 2004‑01‑14).

How to read the addresses in this document (same convention as the rest of `docs/`):

* A **file offset** is what you pass to radare2 in raw mode:
  `r2 -q -e scr.color=0 -n -e asm.arch=x86 -e asm.bits=32 -c 'pd 60 @ 0x169bf9' original/king.exe`
* A **VA** is `file_offset + 0x400000` (ImageBase). String literals live at
  VAs `0x66xxxx…0x68xxxx`, so a code address `0x169bf9` that contains
  `mov edx,[esi+0x4e8]` is a *file offset*, while `push 0x67b830` pushes the *VA*
  of the string `m16\tablo.txr`.
* `tools/ui_dump.py` (added by this investigation) lists/decodes any `.res` entry:
  `python3 tools/ui_dump.py original/COMMON/esc16.RES --list --match tablo`
  `python3 tools/ui_dump.py original/COMMON/esc16.RES --dir tools/out/ui --match tablo`
* **VERIFIED** = read out of the binary / decoded from the shipped art.
  **INFERRED** = my reading of intent, not proven by a data path.

---

## 0. Executive summary

| Question | Answer |
|---|---|
| Is there a driving HUD? | Yes. One **1600×50 chrome/LED strip**, `m16\tablo.txr`, blitted at **y = 0 (top of the screen)** with a resolution‑dependent **x = 0 / −479 / −399**. All readouts are **text** drawn into the strip's LED windows with two bitmap fonts (`matrix.fon` 7×12 for labels/symbols, `icon.fon` 25×30 for the big numbers). |
| Cockpit view: 2D dashboard or 3D interior? | **3D interior.** The camera sits on the `<Vehicle>BoothSpace` B3D node; speedo/tacho/fuel needles are 3D meshes bound by name (`%sSpeedSpace`, `%sTachoSpace`, `%sFuelArrowSpace`), the gear lever and odometer wheels are 3D nodes (`GearKey`, `KMeter%dKey`), and the dashboard is textured from `COMMON/CABINES*.RES` (`pribor.txr`, `<brand>_rul.txr`, `<brand>_cNN.txr`). The cockpit class `_Viewer` *does* ask for 2D masks (`msk\m_panel.msk`, `msk\fase_2/4.msk`, `msk\lfase_2/4.msk`, `m16\m_rul_%02d.msk`) but **none of those exist in the shipped data**. |
| Loading screen | `m16\loadpix.txr` (**1600×1200**, a dark mountain panorama with the *Дальнобойщики 2* logo and the baked‑in caption **«ЗАГРУЗКА ИГРОВОГО МИРА…»**) blitted **centred**; plus `m16\loadrunner.txr` (**1600×97** bright panorama) drawn at `x = 0`, `y = (screenH−1200)/2 + 552`, **clipped to width = screenW · progress / target** — i.e. the panorama is *revealed left→right* as a progress bar. Progress is a counter that advances **20 units per loaded file** (`0x12936f: mov esi,0x14` + 20× `call 0x110690`), saturating at **900** or **3550/4000**. |

---

## 1. Two different coordinate spaces (VERIFIED)

The game mixes two UI authoring resolutions, and this is the single most confusing
thing about its interface:

| UI family | Art resolution | Evidence |
|---|---|---|
| Front‑end menus (`LOAD.RES`) | **800×600** | `m\back_main.txr` = 800×600 blitted at (0,0) (`0x128be0-0x128bef`); buttons `m\*_{a,ap,p}.msk` = 125×125 at X ≤ 648, Y ≤ 452; `TRUCK.INI [ENV] xres=800 yres=600`. |
| In‑game UI + loading (`COMMON/esc16.RES`, `ind16.RES`) | **1600×1200** | `m16\loadpix.txr` 1600×1200, `m_16\BackLoose1.txr` 1600×1200, `m16\tablo.txr` 1600×50, `m_16\BackOrder.txr` 225×1200, `m_16\BackF1.txr` 215×1200 … |

The in‑game code **adapts the 1600‑wide art to the real screen** with explicit
per‑resolution branches (see §2.1), so an 800×600 screen simply shows a *crop* of the
1600‑wide strip. The front‑end (800×600) art is drawn 1:1. I did **not** determine how
the 1200‑px‑tall in‑game panels (`BackF1`, `BackOrder`, …) are fitted on a 600‑px‑tall
screen — see §8.

---

## 2. The in‑game HUD

### 2.1 The status strip object

| Item | Value |
|---|---|
| Class ctor | file offset **0x169760** (`%s…`; allocates **0x510** bytes) |
| Singleton | **0x6f3438** (created at `0x16ec71…0x16ecb3`) |
| vtable | **0x64fdd4** (`+0x00` dtor `0x169910`, `+0x14` = `0x16aa70`) |
| Per‑frame draw | **0x169ab0**, called from `0x16ce8e` (the in‑game UI aggregator `0x16ce40`) |
| Fonts | `matrix.fon` → `this+0x508`, `icon.fon` → `this+0x50c` (`0x16982e`, `0x169839`, loader `0x1e8c60`) |
| Art | `m16\tablo.txr` → `this+0x4e8`; `m16\pager_a.msk` → `this+0x4ec`; `m16\pager_p.msk` → `this+0x4f0` (`0x1698a8/0x169893/0x169889`; the 15‑bit twins `m15\*` are chosen when `0x168fd0` returns 2) |
| Message ring | 4 lines × 0x80 chars at `this+0x2ac`, current line at `this+0x22c`, index `this+0x4bc` (mod 4), "new message" flag `this+0x4c0`, timestamp `this+0x20` |

**The strip blit (VERIFIED)** — `0x169bf9`:

```
0x169bf9  mov  edx,[esi+0x4e8]     ; m16\tablo.txr (1600x50)
0x169bff  mov  eax,[edi+0xc]       ; destination surface
0x169c02  push edx                 ; src
0x169c03  push 0                   ; y = 0      <-- TOP of the screen
0x169c05  push ebx                 ; x (see table below)
0x169c06  push eax
0x169c07  call 0x1b86c0            ; helper(dst, x, y, src)  (whole surface, no scaling)
```

`0x1b86c0(dst, x, y, src)` builds the RECT `{0,0,src.w,src.h}` and tail‑calls
`0x1b81c0(dst, src, &rect, x, y)` — the engine's "blit a whole surface at (x,y)"
primitive (the same one the loading screen uses).

**Per‑resolution layout (VERIFIED)** — `0x169ad7…0x169b4d`, chosen from the screen
width obtained via `[[0x69684c]]->vtable[0x14]()`:

| screen width | strip x (`ebx`) | text base x (`ebp`) | msg X (`[esp+0x50]`) | msg Y (`[esp+0x38]`) | which branch |
|---|---|---|---|---|---|
| > 1204 (0x4b4) | **0** | 469 (0x1d5) | 1161 (0x489) | 11 (0xb) | `0x169af9` |
| 801…1204 | **−479** (−0x1df) | −10 | 682 (0x2aa) | 11 (0xb) | `0x169b19` |
| 641…800 | **−399** (−0x18f) | 70 (0x46) | 250 (0xfa) | 50 (0x32) | `0x169b37` |
| ≤ 640 | **−479** | −10 | 170 (0xaa) | 50 (0x32) | defaults `0x169ae1…0x169af2` |

Consequence on the default 800×600: the strip is drawn at x = **−399**, so strip
pixels `[399…1199]` are visible; the LED windows (strip x 490…692, 701…1064,
1073…1106 — measured from the decoded art) land at screen **x 91…707**, i.e. nicely
centred, and the text column starts at screen x = 70 + offset.

### 2.2 What the strip contains — the HUD element table (VERIFIED positions)

All coordinates are **(x, y)**. Text x is `text_base + k` where
`text_base = 70` on a 640…800‑wide screen; y is the constant pushed into `0x1ea170`.

Text is drawn with `0x1ea170(dst, x, y, font, string, 0, 0)`, which measures the
string with `0x1ea0e0(font, str)` and blits it with `0x1ea1b0`.

| # | element | x (rel. base) | abs x @800 | y | font | content / format | evidence |
|---|---|---|---|---|---|---|---|
| 1 | label/icon | +0x1a (26) | 96 | 23 | matrix.fon | string‑table idx 0x59 (`0x1d2e50(0x59)`) | `0x169de1…0x169dfe` |
| 2 | number (1st) | +0x1a | 96 | 43 | **icon.fon** | `sprintf("%d",[0x67b72c])`, every char **+0x10** before drawing | `0x169e03…0x169e4f` |
| 3 | centred label | +0x32 − w/2 | 120−w/2 | 38 | matrix.fon | string‑table idx 0x5f, centred | `0x169e54…0x169e9c` |
| 4 | number (2nd) | +0x3f (63) | 133 | 43 | icon.fon | `sprintf("%d",[0x6f3444])` (+0x10) | `0x169ea9…0x169eef` |
| 5 | label/icon | +0x55 (85) | 155 | 23 | matrix.fon | idx 0x5a | `0x169ef7…0x169f14` |
| 6 | **clock** | +0x55 | 155 | 43 | icon.fon | `sprintf("%02d:%02d", ftol([0x6f3460]))` (double seconds → hh:mm) | `0x169f19…0x169f8b` |
| 7 | label/icon | +0x9a (154) | 224 | 23 | matrix.fon | idx 0x65 | `0x169fa0…0x169fc0` |
| 8 | number | +0xd2 (210) | 280 | 23 | matrix.fon | `sprintf("%d",[0x6f3448])` | `0x169fd9…0x169ffb` |
| 9 | clock (2nd) | +0x9e (158) | 228 | 43 | icon.fon | `%02d:%02d` of `[0x6f3460]` (the same timer, other field) | `0x16a03c…0x16a075` |
| 10 | centred number | +0x11f − w/2 | 343−w/2 | 38 | icon.fon | `sprintf("%d",[0x6f3448])` | `0x16a08d…0x16a0c6` |
| 11 | **speed unit** | +0x123 (291) | 361 | 40 | matrix.fon | literal **`MpH`** (`0x67b8f4`) when `[esp+0x44] != 0`, otherwise string‑table idx 0x5d (in the Russian release «км/ч»); `TRUCK.INI kmph=on` | `0x16a0cb…0x16a100` |
| 12 | unit mark | +0x13b (315) | 385 | 40 | icon.fon | literal `[\` (`0x67b8f0`) | `0x16a128…0x16a145` |
| 13 | **gear** | +0x169 (361) + (0 or 15) | 431/446 | 23 | matrix.fon | format from string‑table idx 0x5b/0x5c, argument = `'N'`/`'R'` or `'0'+(gear−1)` (`0x16a157: lea ebx,[eax+0x2f]`) | `0x16a14d…0x16a1b5` |
| 14 | number | +0x182 (386) | 456 | 40 | matrix.fon | idx 0x5e | `0x16a225…0x16a245` |
| 15 | centred number | +0x182 − w/2 | 456−w/2 | 43 | icon.fon | `sprintf(...)` of a 0..9 level (clamped `ebx≤9`) | `0x16a1ef…0x16a220` |
| 16 | level digit | +0x200 (512) | 582 | 40 | icon.fon | format `?%c`, char = `0x50+level` | `0x16a30b…0x16a328` |
| 17 | level digit | +0x229 (553) | 623 | 40 | icon.fon | format `%c%c` or `O%c`, char = `0x50+level` | `0x16a3e5…0x16a402` |
| 18 | **nav arrow** | `ebp+0` | 70 | 40 | icon.fon | one of 8 two‑cell icons `jk hi fg lm no \`a bc de` | `0x16a6f3…0x16a836` |
| 19 | (2nd column) *(INFERRED)* | base moved by `add ebp,0x260` (608) | 686… | — | — | after `0x16a414` the field set is repeated at `+0x260` for a second object (an `[obj+0x509c]`-keyed entity, probably the trailer / second truck of a convoy) | `0x16a40a…0x16a414` |

The numbers that are **not** format strings (`[0x67b72c]`, `[0x6f3444]`, `[0x6f3448]`,
`[0x6f344c]`, `[0x6f3450]`, `[0x6f3454]`, `[0x6f3460]`) are the live game values
(clock as a double, money/reward counters, gear, destination distance). Field 11/12
(speed + unit) and 13 (gear) are the ones visible in the LED window in a normal
driving frame.

> **Colour note (partly undetermined):** the fonts are *ink atlases*, not coloured
> text. `icon.fon` contains exactly two non‑zero 5:6:5 values — `0xF800` (pure red)
> and `0x0002` (near black) with identical pixel counts (8840 each); `matrix.fon`
> contains `0x0002`/`0x0001`. Every HUD call site passes `0` for the last two
> arguments of `0x1ea170`, so the on‑screen colour is chosen inside the glyph blitter
> (`0x1ea1b0`) and comes from those atlas levels. **I did not determine the exact
> RGB mapping** (likely a shadow + body pair) — flagged in §8.

### 2.3 The direction / navigation indicator (the only "compass")

VERIFIED at `0x16a43e…0x16a6d1`: the code

1. takes the target object `[[esp+0x14]+0x509c]` and multiplies its position with the
   camera basis (`call 0x1ac2e0`, then a 2×2 float product at `0x16a699…0x16a6b4`);
2. compares the resulting two components against the **doubles 0.0, 0.85, ±0.35,
   −0.85** (`0x64b6f8`, `0x64fe08`, `0x64fe00`, `0x64fdf8`), picking one of **8
   sectors**;
3. draws the sector's icon string at `(ebp, 40)` with `icon.fon`: the strings are
   `jk hi fg lm no \`a bc de` (VAs `0x67b8b4…0x67b8d0`) — each is **two adjacent
   glyph cells** that form one 2‑cell‑wide arrow (verified by measuring `icon.fon`:
   cell `0x6a` has ink out to x=24 and cell `0x6b` to x=4, i.e. one 30‑px image
   spanning both cells; `0x6e`/`0x6f` are empty).

So the original has **no rotating compass needle and no minimap in the driving HUD** —
just this 8‑sector "arrow to the destination" icon.

### 2.4 The message / pager window inside the strip

VERIFIED at `0x16a884…0x16a9e5` (drawn only when `this+0x4ac` = "message showing"):

| item | art | position |
|---|---|---|
| message window | `m16\pager_p.msk` **300×30** | `(x,y)` = `([esp+0x50],[esp+0x38])` = (1161,11) / (682,11) / (250,50) / (170,50) per §2.1 |
| "new message" blink icon | `m16\pager_a.msk` **21×19** | window + (6,5), blinking while `1 < state < 0x14` and `(state/2)&1` (`0x16a90b…0x16a933`) |
| message text | `matrix.fon` | window + (34,23) i.e. `x+0x22, y+0x17`; **max width 0xf7 = 247 px** (`push 0xf7`, `0x16a8cb`); longer text **scrolls horizontally** (the code computes `(width−247)/font.advance + 1` and shifts the string, `0x16a94a…0x16a9d9`) |
| fade | — | when `[0x6f343c] != 0` the strip is faded with `0x1fd430(surface, 0.6f, 0.6f, 0)` (`0x16a83e…0x16a859`) |

Below 801 px screen width the window is placed **below** the strip (y = 50 = strip
height), on wider screens **inside** the strip's right‑hand chrome area
(strip x = 1161, verified identical in both wide branches once the strip shift is undone).

### 2.5 The big centred message popup (`mess*`)

A second in‑game UI object (global **0x6f3430**, drawn by **0x16b0a0** when called from
`0x16cea7`) draws the big letter/pager popup. It blits its window **centred**:

```
0x16b16a  eax = [[0x69684c]]->vtable[0x14]()   ; screen W
0x16b173  cx  = word[[ebp+0x2c]+0x14]          ; window W
0x16b177  eax = (screenW - windowW)/2          ; -> x
0x16b186  ax  = word[[ebp+0x2c]+0x16]          ; window H
0x16b197  ebx = screenH - windowH ; /2         ; -> y   (0x16b190 calls vtable[0x18])
0x16b1a6  call 0x1b86c0                        ; blit at (x,y)
```

Windows: `m16\mess1.msk`, `mess2.msk`, `mess4.msk`, `mess10…15.msk`, `mess20.msk`,
`mess21.msk` = **280×405**, `mess16.msk` = **220×230** (all in `COMMON/esc16.RES`),
with `m_16\MessTop.txr` (**1600×50**) as the top bar of the same window. The text
lines are drawn below the bar (`0x16b1bd: add ebx,0x14`).

### 2.6 Other in‑game HUD art that exists but whose draw site I did not chase

Loaded by the in‑game UI constructors (verified load sites, positions **not**
determined):

| art (in `COMMON/esc16.RES` unless stated) | size | loaded at | my reading |
|---|---|---|---|
| `m16\pause.msk` | 1600×50 | `0x10cf6c/0x10cf73` | the strip's "ПАУЗА" variant (the word is painted into the art: `tools/out/ui/m16_pause.msk.png`). Loaded into a 0x190‑byte screen object created at `0x10cf19…0x10cf51` (ctor `0x117530`, global `0x6cec74`), i.e. the pause overlay. |
| `m16\vtrpanel.msk`, `msk\vtrpanel.msk`, `m15\vtrpanel.msk` | 320×70 | `0x10fb66/0x10fb77/0x10fb7e` | the **recorder** panel — see §2.7 |
| `m16\big0…big9.msk`, `m16\colon.msk` | 8×12 each | `0x10fdbb…0x10ff05` | big digit masks for a time‑code/counter display, in the same code block as the recorder panel |
| `m_16\up_engine_{0,1}`, `up_ebrake*`, `up_vbrake*`, `up_hlights*`, `up_tire*`, `up_abs_1`, `up_oil_1`, `up_fire_1`, `up_scan_1`, `up_susp_1`, `up_ftank*`, `up_glass*`, `up_transmiss*` | 24×24 / small | not referenced by name in the exe (!) | **indicator lamp icons** (engine, parking/vehicle brake, high beam, tyre, ABS, oil, fire, radar, suspension, fuel tank, glass, gearbox) with `_0` = off / `_1` = on, `*c_*` = a second (centred/colour) variant. Because the exe never builds these names, they are almost certainly loaded through the dynamic `%s…` builders — see §8 |
| `m_16\I_city0…3`, `I_fuel0…3`, `I_box`, `I_debris`, `I_gazhold`, `I_park`, `I_race`, `I_repair`, `I_tank`, `I_trailer`, `I_tr_my##_#`, `I_tr_opp`, `icon1…8` | 13×13 / 32×32 | `0x16e972…0x16eaed` (globals `0x6f2de4…0x6f3380`) | **map/info icons** — city class, fuel level, cargo type, repair, race, parking, tank, trailer, own/opponent trailer + 8 generic icons (globals `0x6f2de4`, `0x6f2de8`, `0x6f2dec`, `0x6f2df0`, `0x6f2df4`, `0x6f3330`, `0x6f3258…0x6f3264`, `0x6f3380`) |
| `m16\mirrLS/LM/LL/LH`, `mirrRS/RM/RL/RH`, `mirrTS/TM/TL/TH` | 169×419 / 81×202 / 480×170 (H) | `0x197933…0x197977` (stored at `this+0x50/0x54/0x58`) | **mirror frames**, 3 positions (Left/Right/Top) × 4 sizes (S/M/L/H). The same init sets a block of screen‑rect constants (`0x6f693c`=0x17b/0x1cc, `0x6f6940`=0x96, `0x6f6944`=0xb6/0x72, `0x6f694c`, `0x6f6950`=0x7e, `0x6f6954`=0x3c/0x26, `0x6f6a10`=0x13, `0x6f6a14`/`0x6f6a18`=10/9, `0x6f6a1c`/`0x6f6a20`=10/14, `0x6f6a3c`=0x45d, `0x6f6a40`=0x32). The mask is loaded through the **16‑bit (0x40) mirror mode** branch, so the mirror frame is *2D art over the 3D scene*, while the mirror *surface* itself is a 3D node (`%sSLRSpaceR/L`, `%sBLRSpaceR/L`, `%sHLRSpaceR/L`, `TextureMirrorKey`, `skyMirrorKey`). |
| `m16\scrb{0,1}`, `scrl{0,1}`, `scrc{0,1}`, `scrr{0,1}`, `m16\input.msk` | 250×17 / 17×17 / 73×13 | **never referenced** | orphans in `esc16.RES` (searched the whole exe for the literal names — zero hits) |

### 2.7 The recorder (VTR) overlay — the only in‑game *panel* overlay

VERIFIED: the constructor immediately before the HUD's (`0x10fae0…0x10ff40`) builds a
window named `MENU` (class vtable `0x64dc50`), loads
`…\vtrpanel.msk` (see above), adds the LED `msk\vtrled1.msk` (`0x10fd8e`) and the big
digits, and creates **9 buttons** with `0x10f650(flags, Y, X, id, name)` — the same
argument order as the menu button constructor `0x10e500`:

| button | name string | X | Y | id |
|---|---|---|---|---|
| stop | `res` (`0x6765fc`) | 36 | 13 | 215 |
| off | `off` (`0x6729a8`) | 6 | 13 | 216 |
| jump‑step right | `jsr` | 294 | 3 | 208 |
| jump‑step left | `jsl` | 271 | 3 | 209 |
| fast forward | `ffr` | 231 | 8 | 213 |
| forward | `frw` | 187 | 8 | 210 |
| step | `stp` | 162 | 8 | 212 |
| back | `bck` | 118 | 8 | 211 |
| fast back | `fbc` | 80 | 8 | 214 |

(each button pulls `m16\vtr<name>_{a,p,ap}.txr`, e.g. `vtrbck_a.txr` 2998 B).

**Panel placement (VERIFIED)** at `0x10fd40…0x10fd7b`:

```
ax = word[[[esi+0xc]]]        ; screen W
eax -= 0x190 (400)  ; clamp ≥0
[ebp+0x2c] = eax              ; container left  = screenW-400
dx = word[[[esi+0xc]]+2]      ; screen H
edx -= 0x46 (70)
[ebp+0x30] = edx              ; container top   = screenH-70
```

→ the recorder panel is a **320×70 box anchored to the bottom‑right corner**
(at 800×600: `(400, 530)…(720, 600)`), with the 9 buttons above/inside it at the
panel‑relative coordinates in the table. `m16\vtrpanel.msk` itself is the "KING OF THE
ROAD RECORDER" face with 7 recessed buttons (see `tools/out/ui/m16_vtrpanel.msk.png`).

### 2.8 "How to reproduce" — HUD

1. Load `COMMON/esc16.RES` (or `esc15.RES` for 15‑bit) plus the loose fonts
   `matrix.fon` and `icon.fon`.
2. Blit `m16\tablo.txr` at `(0, 0)` when screenW > 1204, at `(−479, 0)` when
   screenW ≤ 1204, at `(−399, 0)` when 641 ≤ screenW ≤ 800 (clip to the screen).
3. Draw the readouts as text into the strip's LED window, whose left edge is strip
   x = 490 → screen x = 490 + strip_x; the text column starts at
   `screenX = text_base + k` with `text_base = −10 / 70 / 469` as in §2.1 and
   `k ∈ {26, 63, 85, 154, 158, 210, 291, 315, 361, 386, 458, 512, 553}`, y ∈ {23, 38, 40, 43}.
4. Labels/units/symbols in `matrix.fon` (7×12 cells, line height 13); numbers in
   `icon.fon` (25×30 cells, line height 31) using glyph codes `0x40+digit`.
5. Message line: `m16\pager_p.msk` at the §2.1 position, text at +(34,23), 247 px max,
   horizontally scrolling; `m16\pager_a.msk` blinking at +(6,5).
6. Big message: centre `m16\mess<N>.msk` (280×405) and draw `m_16\MessTop.txr` on top.

---

## 3. The cockpit view

### 3.1 View modes (VERIFIED from the in‑game control‑help screen)

The "controls" screen string block at `0x276f28…0x277268` pairs key placeholders with
descriptions (16 entries each, in order):

| key placeholder | description |
|---|---|
| `<Home><End><Del><PgDn>` | `Chase Camera Control` |
| `>` | `Right View` |
| `<` | `Left View` |
| `<V>` | `Cockpit Toggle` |
| `<W>` | `Wipers` |
| `<H>` | `Horn` |
| `<0>` | `Recover and Continue` |
| `<U>` | `TV Director` |
| `<N>` | `Rear Camera` |
| `<C>` | `Choose Camera` |
| `<M>` | `Switch Camera On/Off` |
| `<Z>` | `Change Gear Down` |
| `<A>` | `Change Gear Up` |
| `<Left>,<Right>` | `Turn` |
| `<Down>` | `Brake` |
| `<Up>` | `Accelerate` |

(INFERRED: the two lists are matched positionally; the placeholders are filled from
the key bindings at runtime, so the letters themselves are not proof of the binding.
Note this contradicts the assumption in `NOTES.md`/`docs/input.md` that `C` toggles the
cockpit — per this table `V` is "Cockpit Toggle" and `C` is "Choose Camera".)

Also present: `tvCameraPosition%d`, `tvCameraRightConer`, `tvcameramonitor` — the
"TV Director" replay cameras are B3D nodes on the world models.

### 3.2 The camera is a B3D node: `<Vehicle>BoothSpace`

VERIFIED in the vehicle node‑binding function (`0x1409a0…0x140a13`):

```
0x1409b0  edx = [esp+0x30]                 ; vehicle name prefix
0x1409bc  push 0x679a2c                    ; "%sBoothSpace"
0x1409c1  call 0x21cca9                    ; sprintf(buf, "%sBoothSpace", prefix)
0x1409d3  call 0xe4750                     ; find B3D node by name -> eax
0x1409db  mov  [0x6d2114 + idx*1488], eax  ; keep the node
0x1409ed  call 0x1e19f0                    ; get its absolute 3x4 matrix ->
0x1409e7  lea  edx,[ebp+0x6d2380]          ; ... stored at 0x6d2380 + idx*1488 (48 bytes)
0x140a0c  rep movsd from 0x7025f0          ; default matrix if the node is missing
```

`vehicle.tech` contains exactly these names, one per vehicle — 42 of them
(e.g. `GazelleBoothSpace`, `BmwM5BoothSpace`, `BusBoothSpace`) — so **the cockpit
camera pose is authored inside each truck model**, not hard‑coded. The matrix is later
consumed when the vehicle's matrices are computed (`0x1473bb: add ecx,0x6d2380;
call 0x1abfd0`).

### 3.3 The instruments are 3D, bound by node name

VERIFIED in `0x140f60…0x1413a0` (same binding function):

| template string | VA | ref | meaning |
|---|---|---|---|
| `%sSpeedSpace` | `0x6798c4` | `0x14109d` | speedometer **needle** pivot |
| `%sTachoSpace` | `0x6798d4` | `0x141072` | tachometer needle pivot |
| `%sFuelArrowSpace` | `0x6798b0` | `0x1410c8` | fuel gauge needle pivot |
| `In b3d there is no Cab node "%s"` | `0x6798e4` | `0x140f7b` | error text for the cab node |
| `%scgBlueSpace%d`, `%scgRedSpace%d` | `0x679884/0x679894` | `0x14136a/0x1412bb` | gauge warning‑lamp nodes (blue/red, indexed) |
| `%swheel%d`, `DamageWheel%dKey` | `0x67995c/0x679948` | `0x140d3f` | wheel nodes |
| `%sFFaraL/BFaraR/BFaraL`, `BackFaraKeyR`, `StopFaraKeyR`, `ReflFara*` | `0x679a20…` | | headlight / brake / reflector nodes |
| `GlowFRKey`, `GlowFLKey`, `TestKey`, `BGRKey/BGLKey/FGRKey/FGLKey` | `0x679908…` | | lamp nodes |

Additionally the generic vehicle binder references `SupportKey`, `IconKey`,
`SkyDomeKey`/`SkyDomeSpace`, `WiperSpace`/`Wiper1Space`/`Wiper2Space`,
`FlameSpace`, `Faza8Key`, `TrafficLightKey0/1`, `RadarHeadOn`, `FonarHeadOn`.

### 3.4 The `_Viewer` class = the cockpit view manager

VERIFIED: class name string `_Viewer` (`0x67899c`) used at `0x1312b2`; vtable
`0x64f2f0` (`0x13196d`); error string `_Viewer::attach_vehicle(): veh==NULL`
(`0x6789e4`, `0x131cdf`); five 0x20‑byte sub‑objects at `this+0x118…+0x198`
(`0x131980…0x1319cb`) — the odometer digit wheels.

Node keys bound by `_Viewer` (VERIFIED addresses of the string pushes):

| key | VA | push site |
|---|---|---|
| `RD_Key` | `0x6788f4` | `0x1318c5` |
| `RD_LampKey` | `0x6788e8` | `0x1318d6` |
| `GearKey` | `0x672e74` | `0x131af6` |
| `AvtoKey` | `0x672e6c` | `0x131b01` |
| `ParkKey` | `0x672e64` | `0x131b12` |
| `FuelLampKey` | `0x672e58` | `0x131b23` |
| `TrailerKey` | `0x6789a4` | `0x131b34` |
| `KMeter%dKey` | `0x672e4c` | `0x131b7d` |

and it loads these masks (`0x131695…0x131702`):

| field | art name |
|---|---|
| `this+0x274` | `msk\m_panel.msk` (`0x67894c`) |
| `this+0x278` | `msk\lfase_2.msk` (`0x67898c`) |
| `this+0x27c` | `msk\lfase_4.msk` (`0x67897c`) |
| `this+0x280` | `msk\lfase_4.msk` |
| `this+0x284` | `msk\fase_2.msk` (`0x67896c`) |
| `this+0x288` | `msk\fase_4.msk` (`0x67895c`) |
| `this+0x28c` | `msk\fase_4.msk` |

(the 16‑bit twins `m16\m_panel.msk`, `m16\lfase_2.msk`, `m16\lfase_4.msk`,
`m16\fase_2.msk`, `m16\fase_4.msk` are pushed in the `cmp eax,2` branch at
`0x13170d…0x13177c`), plus a **numbered mask series** loaded at `0x1324e9…0x132640`:
`m16\m_rul_%02d.msk` / `msk\m_rul_%02d.msk` and `%s\big%d.msk` /
`%s\small%d.msk` / `%s\smallr%d.msk`, where the prefix is `m16` or `msk`.

> **IMPORTANT / VERIFIED NEGATIVE:** none of `m_panel.msk`, `fase_2.msk`, `fase_4.msk`,
> `lfase_2.msk`, `lfase_4.msk`, `m_rul_*.msk` exists in the shipped data. I searched
> the entry lists of **all 141 `.res`/`.RES` files** *and* the raw bytes of every file
> under `original/` — the only hits are the string literals inside `king.exe`. The
> `msk\` prefix is the game's **8‑bit art set** (the same code has `msk\`, `m15\`,
> `m16\` branches chosen by `0x168fd0`: 2 → 15‑bit, 3 → 16‑bit), and the 15/16‑bit
> `m_rul_*`/`big*`/`small*` names are also absent. The retail release simply does not
> ship them.

So: **the cockpit is a modelled 3D interior** (dashboard, needles, wheel, odometer
wheels, levers, lamps as named nodes) whose panel textures live in
`COMMON/CABINES.RES` / `CABINES1.RES` / `CABINES2.RES`:

```
txr\pribor.txr       128x128  4:4:4:4   "прибор" = instrument cluster
txr\glass.txr        256x256  4:4:4:4   windscreen
txr\glass_env.txr    128x128  4:4:4:4   windscreen reflection
txr\fueloff.txr        8x8    fuel warning lamp off   (txr\fuelon.txr = on)
txr\<brand>_rul.txr  128x128  4:4:4:4   dial scale ("ruler"): gaz_/kam_/ren_/...
txr\<brand>_cNN.txr  128x128  4:4:4:4   gauge / lamp faces (c00..c38, plus
                                         paired ids 0425, 0524, 0621, 1022, 1120,
                                         1213, 1223, 1626, 2223, 2227, 2425, ...)
```

(`gaz`=Gazelle, `kam`=Kamaz, `ren`=Renault, …). The paired `cNNNN` ids are the
evidence that the cluster is composed of separate 128×128 gauge quads placed in 3D.

**Answer to "2D overlay or 3D interior":** *3D interior* — with the caveat that the
retail data's `_Viewer` 2D overlay masks are missing, and that the top status strip
(§2.1) plus the mirrors' 2D frames (§2.6) *are* drawn over the 3D scene.

**How to reproduce:** place the camera on the vehicle's `<Name>BoothSpace` node
(import from `vehicle.tech` / the B3D node table), keep the truck interior visible
(back‑face culling must not remove the inside of the cab), and rotate needle meshes
found at `<Name>SpeedSpace` / `<Name>TachoSpace` / `<Name>FuelArrowSpace`; texture the
cluster from `CABINES*.RES` `txr\pribor.txr` + `<brand>_rul.txr` + `<brand>_cNN.txr`.

---

## 4. The loading screen

### 4.1 Art

| art | res entry | size | decoded |
|---|---|---|---|
| background | `ind16.RES` → `m16\loadpix.txr` (15‑bit: `ind15.RES` → `m15\loadpix.txr`) | **1600×1200**, 3 840 074 B | dark, blurred mountain scene; the **Дальнобойщики 2 / Hard Truck 2 logo** sits in the centre and the caption **«ЗАГРУЗКА ИГРОВОГО МИРА…»** ("loading the game world…") is *baked into the art* at y≈660. `tools/out/ui/m16_loadpix.txr.png` |
| progress strip | `m16\loadrunner.txr` | **1600×97**, 310 474 B | a bright photo panorama; `tools/out/ui/m16_loadrunner.txr.png` |

Both are loaded at `0x110624/0x110634` (m15) and `0x110644/0x110653` (m16) into
globals **0x6ced5c** (background) and **0x6ced60** (runner); the choice is
`0x168fd0() == 2` → 15‑bit.

### 4.2 Init and progress target (VERIFIED)

`0x1105b0…0x11061a` (the loader's "enter" function, called from `0x110dad`):

```
0x1105b5  [0x6ced50] = 0                        ; progress counter
0x1105bf  cmp [[0x6cec6c]+0x514], 1
0x1105c8  [0x675468] = 0x384 (900)              ; target, case A  (jle -> 0x1105d4)
0x1105d4  ecx = [[0x6cec6c]+0x524]
0x1105dc  if (ecx == 0) -> [0x675468] = 0xfa0 (4000) or 0xde2 (3550)  ; byte 0x6f2d18
0x1105f8  else            -> [0x675468] = 0x384 (900) or 0x3ca (970)  ; [[0x6cec98]+0x180] >= 2
```

### 4.3 The per‑frame render (VERIFIED, `0x110690…0x1107bf`)

```
ebp = [0x6ced50]; [0x6ced50] = ebp+1; clamp 0 <= ebp <= [0x675468]
if (ebp % 10) return                       ; redraw only every 10 units
edi = (screenW - 1600)/2                   ; both from [[0x69684c]]->vtable[0x14/0x18]
ebx = (screenH - 1200)/2
0x1b86c0(dst, edi, ebx, loadpix)           ; background, CENTRED, 1:1, no scaling
eax = screenW * ebp / [0x675468]           ; revealed width
rect = {0, 0, eax, 97}
0x1b81c0(dst, loadrunner, &rect, 0, ebx + 0x228)   ; y offset 552
```

So on 800×600: background at (−400,−300) (you see the central 800×600 of the
1600×1200 picture — the logo), runner at **(0, 252)** with width
`800·progress/target`, i.e. the panorama **wipes in from the left over the horizon
band of the background art**, which is exactly where the background has its painted
"horizon" strip.

### 4.4 Where the time actually goes (VERIFIED)

The render is called from the world‑load routine: at **0x12936f** `mov esi,0x14`
(20) followed by the loop `0x129374 call 0x110690 / dec esi / jne` — i.e. **20 loading
screen draws per loaded item**, so the counter advances 20 per item and the bar is a
*real* (item‑proportional) progress indicator, not a timer: target 900 ≈ 45 items,
3550/4000 ≈ 178/200 items. The same 20× loop exists at `0x129538` and `0x129831`.

The items loaded on that path (`0x112920…0x1129aa`, helper `0x1107c0` = "open a
`.res`/`.b3d` inside a directory", `0x110a10` = its sibling):

* `COMMON\common` (common1.res/common2.res)
* `COMMON\esc` → **`esc16.RES`** (the in‑game UI!) via `0x110a10`
* `COMMON\cabines` → **`CABINES.RES`** (the cockpit interiors)
* `trucks` → the truck models
* the map set `<map>`, `<map>1.res`, `<map>2.res`, `<map>3.res` and `<map>.b3d`
  (string builders at `0x110883/0x11090a/0x11099e/0x1109a5/0x110adc`)

**How to reproduce:** show `loadpix` centred 1:1 (or its central crop), draw
`loadrunner` at `y = (screenH−1200)/2 + 552` clipped to `width = screenW·loaded/total`,
and use "number of world/truck resources still to open" as `loaded`; the original
starts the bar after the menu and finishes it when COMMON+esc+cabines+trucks+map are
parsed. Note the original does **not** print a percentage — the only progress display
is the widening panorama.

---

## 5. Menu / interface screens

### 5.1 The front‑end (800×600, `LOAD.RES`)

VERIFIED button constructor: **`0x10e500(flags, Y, X, id, name)`** — the caller pushes
`flags, Y, X, id, name` and then `add esp,0x10`. Name → art mapping is
`<name>_a.msk` / `_ap` / `_p` (`%s_a.msk` at `0x10e52f` for the `.msk` variant,
`%s_a.txr` at `0x10e22f` for the `.txr` variant); the `m_16\` / `m_15\` prefix is
prepended for the in‑game screens (`0x10d917/0x10d91e`).

**Main menu** — all 8 buttons, decoded from the 8 call sites
`0x128311/0x128333/0x128355/0x128373/0x128395/0x1283b4/0x1283d3/0x1283ef`
(this also validates the argument order against `NOTES.md`):

| name | id | X | Y | art (LOAD.RES) |
|---|---|---|---|---|
| `m\graph` ГРАФИКА | 107 | 79 | 82 | `m\graph_{a,ap,p}.msk` 125×125 |
| `m\contr` УПРАВЛЕНИЕ | 103 | 27 | 202 | `m\contr_*` |
| `m\demo` ДЕМО | 106 | 27 | 332 | `m\demo_*` |
| `m\recor` ВИДЕОПЛЕЕР | 101 | 79 | 452 | `m\recor_*` |
| `m\singl` ИГРА | 102 | 596 | 82 | `m\singl_*` |
| `m\multi` СЕТЕВАЯ ИГРА | 104 | 648 | 202 | `m\multi_*` |
| `m\chang` СМЕНИТЬ ВОДИТЕЛЯ | 105 | 648 | 332 | `m\chang_*` |
| `m\exit` ВЫХОД ИЗ ИГРЫ | 108 | 596 | 452 | `m\exit_*` |

Backdrop `m\back_main.txr` (**800×600**) blitted at (0,0) (`0x128be0…0x128bef`),
overlay `common.msk` (800×600, chrome frame with a transparent central circle through
which the `MENV\dr.b3d` scene is drawn) + `runner_{a,ap,p}.msk` (12×22 selection
runner) + `cursor.msk` (14×16); fonts `menu.sft`/`menu.fon`, `large.fon`, `menublk.fon`.

### 5.2 In‑game / ESC screens (1600×1200 art, `esc16.RES`)

The screen inventory below is **VERIFIED** by the sibling string references of each
backdrop load (each row = the backdrop art entry and the button/screen names that are
loaded in the same function). Sizes are from the decoded `.txr` headers. The
*interpretation* of a row (which human screen it is) is INFERRED from those names;
the multiplications of buttons into the `m_16\<name>_{a,p,ap,f,pp,n,ok}.txr` family
were checked against the entry list where a name is given.

| screen | backdrop(s) | size | buttons/other names in the same code |
|---|---|---|---|
| in‑game ESC main | `m_16\BackEscMain.txr` | 516 074 B | `exitsys`, `main`, `sound`, `control`, `911`, `restart`, `loadg`, `savg` |
| "continue?" popup | `m_16\BackEscMain.txr` | 516 074 B | `excanc`, `exnsave`, `exsave` |
| graphics options | `g\backGraf.txr`, `g\backGrafOpt.txr` | — | `g\canc`, `g\huge`, `g\large`, `g\med`, `g\small`, `dtl`, `grf`, `video`, `maxdrv`, `numdev`, `aright`, `aleft` |
| controls (УПРАВЛЕНИЕ) | `m_16\BackEscCtrl.txr`, `c\backCtrl.txr`, `c\backCtrlOpt.txr` | 516 074 | `agear`, `forcer/forcel`, `centerr/centerl`, `sensr/sensl`, `autgb`, `mhor`, `skivol`, `engvol`, `midivol`, `c\canc`, `c\def`, `ok`, `canc`, `on/off`, `demo` |
| sound | `m_16\BackEscSnd.txr` | 516 074 | `sfxvol`, `midivol`, `volr/voll`, `musr/musl`, `engr/engl`, `skidr/skidl`, `ok`, `canc` |
| save game | `m_16\BackEscSave.txr` | 516 074 | `save`, `nsave`, `delgame`, `mup`, `mdown` |
| load game | `m_16\BackEscLoad.txr` | 516 074 | `load`, `nsave`, `delgame`, `mup`, `mdown` |
| exit confirm | `e\backExit.txr`, `e\backExitDoit.txr` | — | `e\ok`, `e\canc`, `cont`, `%s.mvp`, `%02d-%02d %02d:%02d` (the key‑help list from §3.1 is drawn here) |
| records / replay | `r\backRcrd.txr`, `r\backRcrdReplay.txr`, `r\backRcrdRec.txr` | — | `r\rec`, `r\repl`, `r\delrec`, `r\canc`, `*************` |
| multiplayer connect | `n\backMult.txr`, `n\backMultConn.txr` | — | `n\color`, `n\join`, `n\host`, `aleft`, `aright`, `truck`, `%s%d` |
| host setup | `h\backHost.txr`, `h\backHostNext.txr` | — | `h\arcad`, `h\simul`, `h\tball`, `h\trial`, `h\circ`, `trafm`, `daym`, `weatm` |
| driver select | `d\backDriv.txr` (a real `.txr`), `d\backDrivCurr.msk` (82 711 B), `d\backDrivRcnt.msk` (47 519 B) | msk = the driver cards | `d\canc`, `d\deldriv`, `aleft`, `aright`, `lastp`, `logo`, `photo`; brand logos `m16\BmwM51.msk` … `m16\ZilR4.msk` (160×160, ~130 files) |
| single player: new / load | `s\backSing.txr`, `s\backSingLoad.txr`, `s\backSingNew.txr` | — | `s\new`, `s\arcad`, `s\simul`, `s\metruck`, `s\mecar`, `listofgames`, `trafs`, `days`, `weats`, `cont`, `s\canc`, `s\load`, `aleft`, `aright` |
| fuel station dialog | `m_16\BackGas.txr` | 540 074 | `gasfull`, `gaspay`, `gascanc` |
| service station dialog | `m_16\BackService.txr` | 1 536 074 (640×1200) | `check`, `servcar/upgradec/upgradet` (140×115), `refuelSound` |
| job market / order / sale / info panels | `m_16\BackOrder.txr`, `BackSale.txr`, `BackJob.txr` (225×1200), `BackInfo.txr` (385×1200), `BackMy.txr` (415×1200) | full‑height side panels | `Info`, `chan`, `fire`, `mup`, `mdown`, `TrafficLightKey0/1` |
| in‑game info screens F1/F2/F3 | `m_16\BackF1.txr`, `BackF2.txr`, `BackF3.txr`, `BackF2Tball.txr` (all 215×1200) | full‑height panels | `Info`, `TrafficLightKey*`, `%s%d` |
| win / lose screens | `m_16\BackLoose1…5.txr`, `BackWin.txr`, `BackWin1/2.txr` | **1600×1200** each | `o_main`, `o_rest`, `ow_main`, `ow_cont`, `ow_rest` |
| F5 — CB radio / trucker talk | `m_16\BackF5pol.msk`, `BackF5maf.msk`, `BackF5opp.msk`, `BackF5nei.msk`, `BackF5tru.msk`, `BackF5cbradio.msk` | 216 363 B (~280×405‑ish masks) | `F5{p,m,o,n,tr}_{ask1…4,ok,canc}`, `F5search`, `F5talk`, `F5tr_{l,r,col}` + answer art `f5*_ans_*.txr` |
| popups | `p\MenuPopupA0/1/2/3.msk`, `p\MenuPopupB.msk` (`0x67733c…0x677398`), `m16\MenuPopupA5.msk` (494×160, in `ind16.RES`) | | `p\0canc`, `p\1canc/1ok`, `p\2ok`, `p\3canc/3wait`, `p\4ok`, `n\%s%d.msk` |
| photo album | `m16\escphoto01…54.txr` (20 074 B each), `escmafia01…08`, `escpolice01…15` | ~100×100 thumbs | |
| recorder overlay | see §2.7 | | |
| CD/install screen | `screen2.txr` (`0x6733f0`, used at `0xe1cea` next to `SOURCE`/`INSTALL`/`\disk1`), `LOAD.RES not found` | | |

**Screen flow (VERIFIED entry points, INFERRED ordering):**

```
main menu (LOAD.RES 800x600)
 ├─ m\graph  -> g\backGrafOpt.txr  -> g\canc -> back
 ├─ m\contr  -> c\backCtrlOpt.txr  -> c\canc / c\def
 ├─ m\demo   -> runs the demo
 ├─ m\recor  -> r\backRcrd.txr -> {r\backRcrdRec.txr | r\backRcrdReplay.txr}
 ├─ m\multi  -> n\backMult.txr -> n\backMultConn.txr -> h\backHostNext.txr
 ├─ m\chang  -> d\backDriv.txr (+ d\backDrivCurr/Rcnt.msk cards, m16\<Brand>N.msk logos)
 ├─ m\singl  -> s\backSing.txr -> {s\backSingNew.txr | s\backSingLoad.txr}
 └─ m\exit   -> e\backExitDoit.txr ; while a game is running ESC -> m_16\BackEscMain.txr
                                                    -> {save, load, sound, control, 911, restart}
   load world: COMMON\common, COMMON\esc, COMMON\cabines, trucks, <map>{,1,2,3}.res/.b3d/.way
```

**How to reproduce (menus):** front‑end = 800×600 canvas, backdrop at (0,0) 1:1,
125×125 buttons at the table above with `_a`/`_ap`/`_p` states, `common.msk` frame on
top, fonts `menu.sft` → `menu.fon` → `large.fon`; in‑game screens = the same button
constructor with the `m_16\` prefix and `_a/_p/_ap/_f/_pp/_n/_ok` state suffixes
(`%s\%s_a.txr`, `%s\%s_p.txr`, `%s\%s_ap.txr`, `%s\%s_pp.txr`, `%s\%s_n.txr`,
`%s\%s_ok.txr`, `%s\%s_f.txr` at `0x10d365…0x10d7f8`, `0x10e196…0x10eb5b`), with
`m_15\` for 15‑bit and `msk\` for 8‑bit.

---

## 6. Text and fonts

| font file | container | atlas | grid | glyph | line height | baseline | used by |
|---|---|---|---|---|---|---|---|
| `matrix.fon` | LOFF/TXR, 16 bpp | 112×208 | 16×16 cells of 7×13 | 7×12 | 13 | row 11 | HUD labels, units, symbols, in‑strip message text |
| `matrix1.fon`, `matrix2.fon` | LOFF/TXR 16 bpp | 112×208 | same | 7×12 | 13 | 11 | alternates (same metrics) |
| `icon.fon` | LOFF/TXR 16 bpp | 400×496 | 16×16 cells of 25×31 | 25×30 | 31 | 28 | HUD **big numbers** (glyph codes `0x40..0x49` = digits, reached by adding `0x10` to ASCII) and the 2‑cell nav arrows (`0x60…0x6f`) |
| `menublk.fon` | LOFF/TXR 16 bpp | 96×144 | 16×16 cells of 6×9 | 6×8 | 9 | 7 | small/block menu text |
| `MENU.FON` | plain `.FON` 8 bpp | 96×144 | 6×9 cells | 6×8 | 9 | 7 | menu text (preferred after `MENU.SFT`) |
| `MENU.SFT` | plain `.SFT` 1 bpp | — | cell 8×16, codes 0x21…0x7E | 8×16 | 16 | | menu text (tried **first**: `0x1ddd39` → `menu.sft`, then `menu.fon`) |
| `D1.SFT`, `LARGE.FON` | | | 7×13 cells (LARGE) | | 13 | | |

Loading: `.fon` via `0x1e8c60` (HUD: `0x16982e` `matrix.fon`, `0x169839` `icon.fon`);
menu fonts via the same loader at `0x10c901` (`large.fon`) / `0x10c90b` (`menu.fon`).
Glyph metrics live in the font object: `+0x14` = cell width, `+0x18` = per‑glyph
advance (used by the message marquee at `0x16a9b2`).

**Text rendering (VERIFIED plumbing):**

* `0x1ea170(dst, x, y, font, str, ctx, flags)` — draws one line;
* `0x1ea0e0(font, str)` — measures (returns the width in pixels);
* `0x1ea1b0` — the glyph blitter (uses `font+0x14`, `font+0x18`, and `0x1d3b10`).
* `0x1d2e50(index)` — **localisation string table** lookup
  (`return table[index]`, table base **0x71e858**, bounds‑checked against 1000/0).
  The fallback strings baked into the exe are the contiguous block
  `0x67b8b4…0x67b8fc`: `jk hi fg lm no \`a bc de ] O%c %c%c ?%c MN KL [\ MpH %02d:%02d`.
  So the HUD's labels/units and the gear‑display format are localised, and the
  English/russified values live in an external language file (not present) with these
  as defaults.
* Numbers typed into the HUD as ASCII are shifted **+0x10 per character** before the
  draw to select the big‑digit cells of `icon.fon` (`0x169e2b`, `0x169ec8`, …).

**Shadow / outline:** not proven. `icon.fon` has exactly two non‑zero pixel values
(`0xF800` red, `0x0002` near‑black, equal counts) and `matrix.fon` has two
(`0x0002`, `0x0001`), which is the signature of a two‑level (body + shadow/outline)
ink atlas, but the blitter's mapping from those levels to on‑screen RGB was not
extracted (see §8). No separate "draw text twice with an offset" call site exists in
the HUD path.

---

## 7. Verified vs inferred — quick index

| Claim | Status |
|---|---|
| `tablo.txr` 1600×50 drawn at y = 0 with x = 0/−479/−399 | VERIFIED (`0x169ad7…0x169bf9`) |
| Field list, x/y offsets, fonts, formats of the strip | VERIFIED (call sites in §2.2) |
| Numbers in `icon.fon` at `0x40+digit` (ASCII+0x10) | VERIFIED (code + measured atlas) |
| Nav arrow = 8 two‑cell icons, thresholds 0.85/±0.35 | VERIFIED (`0x16a43e…0x16a836`, `0x64b6f8…`) |
| `pager_p` 300×30 / `pager_a` 21×19 positions, 247 px text, marquee | VERIFIED (`0x16a884…0x16a9d9`) |
| `mess*` popup centred | VERIFIED (`0x16b16a…0x16b1a6`) |
| Cockpit camera = `<Vehicle>BoothSpace` node matrix | VERIFIED (`0x1409b0…0x140a13`, `vehicle.tech`) |
| Needles/levers/odometer = 3D nodes | VERIFIED (node‑name templates + `_Viewer`) |
| Dashboard textures in `CABINES*.RES` | VERIFIED (entry list) |
| `_Viewer`'s 2D masks (`m_panel`, `fase_*`, `m_rul_*`) absent from the release | VERIFIED (exhaustive search) |
| Loading screen art + geometry + 20 draw calls/item + target 900/3550/4000 | VERIFIED (`0x1105b0…0x1107bf`, `0x12936f`) |
| Menu button constructor signature `(flags, Y, X, id, name)` | VERIFIED against all 8 main‑menu sites |
| Screen inventory (backdrop ↔ button‑name families) | VERIFIED by sibling string references |
| Exact positions on every ESC screen | NOT DONE (only the recorder panel, §2.7) |
| Which HUD field is which game value | INFERRED (`[0x67b72c]`, `[0x6f3444/48/4c/50/54]`, `[0x6f3460]`) |
| Text colour / shadow | NOT DETERMINED |
| Fit of the 1200‑px‑tall in‑game panels onto a 600‑px screen | NOT DETERMINED |
| Semantics of `fase_2/4`, `lfase_2/4`, `m_rul_%02d` | INFERRED (headlight/telltale 2 or 4 lamps + dial scale); art missing |

---

## 8. What I could NOT determine

1. **The exact RGB used for HUD text** and whether a shadow/outline is drawn. The
   fonts are two‑level ink atlases; every HUD draw passes `0` for the two spare
   arguments of `0x1ea170`, so the colour is decided inside `0x1ea1b0`
   (`0x1ea1b0…0x1ea5c0`), which I did not fully decode. Everything needed to *reproduce
   the look* (which font, which cell, which position) is known.
2. **The identity of each numeric readout** in the strip: the code reads globals
   `0x67b72c`, `0x6f3444`, `0x6f3448`, `0x6f344c`, `0x6f3450`, `0x6f3454` and the
   double `0x6f3460`. `0x6f3460` is clearly a **clock/timer** (formatted `%02d:%02d`
   via `ftol`), `0x67b72c`/`0x6f3444` are written from `[[game]+0x509c]+0x210` /
   `+0x20c` (city indices + 1), and `[0x6f344c]` is the **gear** (`'0'+(v-1)` or
   `'N'`/`'R'`), but the money/cargo/fuel mapping is inferred from the surrounding
   formats (`$%d`, `%d min`, `%d%%`, `%d:%d`, `%.2f`) rather than traced to data.
3. **Which on‑screen element owns `m16\big0…9` + `colon`** (loaded in the recorder
   block) and what `m16\scrb*/scrl*/scrc*/scrr*` and `m16\input.msk` were for — the
   exe never references those last names at all, so they appear to be **unused
   leftovers** in `esc16.RES`.
4. **How the in‑game (1600×1200) panels are scaled/cropped to the real screen.** The
   HUD strip's per‑resolution branch table is fully decoded, and the loading screen
   uses pure centring (i.e. cropping), but I did not decode the container/frame class
   (`0x64dc50`) that positions the ESC‑screen panels, so I cannot say whether
   `BackF1.txr` (215×1200) is drawn 1:1, scaled to the screen height, or stretched.
5. **Full per‑button layouts for the ESC/options/driver‑record screens.** Only the
   main menu (8 buttons) and the recorder panel (9 buttons) were extracted with
   coordinates; the other screens' button families are known (§5.2) but their
   positions were not decoded.
6. **The `%sSLRSpaceR/L`, `%sBLRSpaceR/L`, `%sHLRSpaceR/L`, `TextureMirrorKey`,
   `skyMirrorKey`, `Faza8Key` node semantics** and the exact mirror rectangles
   (the constants are listed in §2.6 but their assignment to left/right/top and
   S/M/L/H was not proven).
7. `rnrnfo.scq` and the external **language file** that fills the string table
   `0x71e858` are not present in this release.

---

## 9. Reproduction checklist for the re‑implementation

1. Load `COMMON/esc16.RES` (masks + textures) and `ind16.RES`; keep the 15‑bit
   variants (`esc15`/`ind15`) selectable to mirror the original's colour‑depth switch
   (`0x168fd0`: 2 = 15‑bit, 3 = 16‑bit).
2. HUD: blit `m16\tablo.txr` at the §2.1 (x,0) and draw the fields of §2.2 in
   `matrix.fon`/`icon.fon`. The number cells of `icon.fon` start at glyph `0x40`.
3. Message line: `pager_p` + `pager_a` + 247 px `matrix.fon` text with marquee.
4. Big message: centred `mess<N>.msk` (280×405) + `m_16\MessTop.txr`.
5. Cockpit: camera on `<Vehicle>BoothSpace`, dash textured from `CABINES*.RES`
   (`pribor.txr`, `<brand>_rul.txr`, `<brand>_cNN.txr`), needles at
   `<Vehicle>{Speed,Tacho,FuelArrow}Space`, gear lever `GearKey`, odometer wheels
   `KMeter%dKey`. The 2D `m_panel`/`fase_*` overlay cannot be reproduced from this
   release because the art is not shipped.
6. Loading screen: `loadpix` centred 1:1 + `loadrunner` at `(0, (screenH−1200)/2+552)`
   with width `screenW·loaded/total`, total ≈ number of world resources.
7. Menus: 800×600 front‑end with the §5.1 table; in‑game screens with the §5.2 art and
   `m_16\` prefixed button triples `_a/_p/_ap`.

Art dumps produced during this investigation (decode with `tools/ui_dump.py`, view as
PNG): `tools/out/ui/m16_tablo.txr.png`, `m16_pause.msk.png`, `m_16_MessTop.txr.png`,
`m16_vtrpanel.msk.png`, `m16_pager_a.msk.png`, `m16_pager_p.msk.png`,
`m16_loadpix.txr.png`, `m16_loadrunner.txr.png`, `m16_mirr{L,R,T}{S,M,L,H}.msk.png`,
`m16_input.msk.png`.

---

## 9. Implementation notes (this port)

The strip and the two new event windows are implemented in `src/ui/Hud.cpp`; the
left-hand road map is `src/ui/Map.{h,cpp}` (it used to be a corner minimap inside
`App::drawMinimap`, which is still used as the 2D fallback).

* **Strip layout (VERIFIED against the retail frames).** The art is blitted
  **1:1** with the per-resolution x of §2.1 (`>1204 -> 0`, `801..1204 -> -479`,
  `641..800 -> -399`); at 800 the first LED window (art x 490) lands at screen
  x 91 and the second (art x 701) at 300, which is what
  `shots/ref/ref_hud.png`/`ref_map_and_fine.png` show.  The readouts keep their
  authored pixel size at every resolution (the original scales nothing).
* **Element table of the shipped RU build** (800-px reference coordinates,
  measured off the frames; the doc's §2.2 offsets are the same layout shifted by
  the strip x): ПОЗИЦИЯ (78,6) `2 км 3` (90,20) · ОСТ.ВРЕМЯ (143,6) `25:06`
  (140,20) · speed (307,22) + `км/ч` (335,26) · АККИ (396,6) + step digit
  (425,6) + `20x100 об/м` (396,24) · clock (624,20) · gear/level at the end.
* **The map panel** keeps the reference rectangle `268x434` (left 6, under the
  strip) scaled uniformly to the play area.  World -> panel is `0.133 px/unit`
  at the 800-px reference (measured: the «АЗС 22…32» shields of the reference are
  ~1.5 km apart and ~200 px apart), the truck sits at 45 % / 60 % of the panel
  and the roads are the `.way` RSEG polylines.
* **Map icons** come from `COMMON/esc16.RES`: `m_16\I_city0`, `I_fuel0`,
  `I_park`, `I_repair`, `I_tank`, `I_debris`, `I_box`, `I_gazhold`, `I_race`.
  The markers are the `map_town_*` / `mapBenzo_*` / `mapSto_*` / `mapPark_*` /
  `mapNameStore_*` / `map_gasholder_*` / `debris_*` `.way` nodes of
  §5.6 (`docs/script_gameplay.md`), and the settlement names + «АЗС NN» numbers
  come from the `town` forms (field 3 = the number, field 8 = the node names).
* **The road shields** («АЗС 22») are placed every ~4 km along the roads named in
  the `.way` data (`W001`, `W005`, …); the shipped data carries no road numbers,
  so the number in the tag is derived from that road name.  **[INFERRED]**
* **The pager window** is centred inside the 50-px strip band (the original's two
  branches put it at y=11 above 801 px and y=50 below; the retail frame shows it
  there), with the `m16\pager_p.msk` plate, the blinking `m16\pager_a.msk` icon
  at +(6,5) and the text at +(34,8) scrolling past 247 px.
* **`--msg-shot TEXT` / `--fine-shot SPEED`** are one-frame world captures: they
  force `State::World` before the uploads (after `loadData()`, which otherwise
  re-picks MENU from the default `--scene`), so the strip, the map and the
  vehicle are all loaded.  `HT2_MSG_GLYPHS=1` prints the exact cp1251 glyph-code
  string `Hud::drawMessage` hands to `Font::draw`, which the test asserts against
  `text.encode("cp1251")` (this is what locks the script-message conversion).
* **The fine panel** is composed in code from the strip chrome, `matrix.fon` and
  the real `txr\flash.txr` sprite: the shipped data has no violation-panel mask
  (all 141 `.res` containers and the exe literals were searched).  The original's
  `snd\photo.wav` and `snd\antiradar.wav` are shipped and are played with it.
* **The contract clock** (`ОСТ.ВРЕМЯ`): the scripts carry no deadline, so
  `Economy` derives one as `25:00 + distance / 16 m/s` (a fresh job therefore
  reads ~25:00 like the frames) and pays half the reward when it runs out.
  **[INFERRED]**

---

## 9. Front-end / in-game menu **flow** (verified against `king.exe`)

Section 5 already lists the recovered button-entry points.  This section records
what the *flow* is, what the port implements, and the deviations fixed in
`src/ui/Menu.{h,cpp}`.  Drive the whole graph with:

```bash
python3 tools/menu_flow_test.py          # 17 checks, 0 failures
python3 tools/menu_flow_test.py --list   # the flow table only
```

Screenshots: `shots/menu_flow_main.bmp` (front-end) and
`shots/menu_flow_esc_{main,exit,sound,control,save,load}.bmp`.

### 9.1 The front-end is **one** screen, not a stack

All 8 buttons come from one constructor run at `0x1282f8..0x1283f4`
(`0x10e500(flags, Y, X, id, name)`), the backdrop `m\back_main.txr` is blitted
once at `(0,0)` (`0x128be0`), and the only other art is `common.msk`,
`runner_*.msk` and `cursor.msk`.  So the start menu has **no sub-screens and no
"back"**: every button leaves the screen to another module.  The port's
`src/ui/Menu.cpp` already matches this (one screen, 8 sockets, `_a/_ap/_p`
states), and `tools/menu_flow_test.py` verifies that all 8 sockets carry art.

| button | id | opens (recovered entry point) | port |
|--------|----|------------------------------|------|
| `m\graph` | 107 | `g\backGrafOpt.txr` | not implemented (`Menu::screenFor("graph") == "graph"`) |
| `m\contr` | 103 | `c\backCtrlOpt.txr` = the ESC **control** screen | not implemented, but `EscMenu` *has* that screen |
| `m\demo` | 106 | runs the demo | not implemented |
| `m\recor` | 101 | `r\backRcrd.txr` → rec \| replay | plays the intro movie |
| `m\singl` | 102 | `s\backSing.txr` → new \| load | enters the world directly |
| `m\multi` | 104 | `n\backMult.txr` → conn → host | not implemented |
| `m\chang` | 105 | `d\backDriv.txr` | not implemented |
| `m\exit` | 108 | `e\backExitDoit.txr` — **a confirmation screen** | fixed, see 9.3 |

### 9.2 The in-game ESC family is the real flow graph

`EscMenu` implements the 6 `Family::Esc` screens (`main`, `exit`, `sound`,
`control`, `save`, `load`) plus the side panels.  The recovered entry points and
the port's graph agree, and the walks in `tools/menu_flow_test.py` confirm every
edge:

```
ESC main ──sound────▶ sound  ──ok/canc──▶ ESC main
         ──control──▶ control──ok/canc──▶ ESC main
         ──savg─────▶ save   ──nsave────▶ ESC main
         ──loadg────▶ load   ──nsave────▶ ESC main
         ──exitsys──▶ exit (the confirm) ──excanc──▶ ESC main
                                        ──exsave──▶ exitsave
         ──main─────▶ front-end menu (action "menu")
```

**"Back" semantics** (`EscMenu::back`, `src/ui/EscMenu.cpp`): with a side panel
open Esc resumes driving (`"resume"`); on any ESC sub-screen Esc returns to the
ESC main (`"back"`); on the ESC main Esc closes it (`"resume"`).  That matches
the original, where the panels are dialogs and the ESC pages nest one level.
The front-end itself has no "back" (9.1), so Esc at the start menu quitting the
game is a port choice, not an original rule.

**Keyboard / gamepad / mouse parity.**  The ESC family has all three: `App` maps
`SDL_CONTROLLERBUTTONDOWN` (dpad → `selectNext/adjustSelected`, A/START →
`activateSelected`, B/BACK → back) plus `SDL_MOUSEMOTION`/`BUTTONDOWN`.  The
front-end `Menu` only had keyboard + mouse — the pad branch in `App::run()` is
guarded by `esc_.active()`.  `Menu::handlePad(PadButton)` now exists (dirs →
`handleKey(UP/DOWN/LEFT/RIGHT)`, A/START → RETURN, B → ESC), so the App only has
to forward the pad event; the hunk is in `tools/app_vehicle_wiring.diff`.

### 9.3 Deviations fixed

| deviation | original | port before | port now |
|-----------|----------|-------------|----------|
| **exit confirmation** | `m\exit` → `e\backExitDoit.txr`, and the in-game popup has `exsave`/`exnsave`/`excanc` | `ВЫХОД ИЗ ИГРЫ` quit on the first RETURN | `Menu::activate()` opens a modal confirm (`confirmText_`, yes/no, the safe answer preselected); the `exit` id is reported only after the confirm. B/Esc/click-right cancels |
| **pad navigation** | same ids for every input source | pad did nothing on the front-end | `Menu::handlePad()` + `tools/app_vehicle_wiring.diff` |
| **mouse routing** | one activation path | `handleMouse` set `activated_` directly, bypassing the confirm | `handleMouse` → `activate()` |
| **hot keys** | button ids 101..108 | matched `key == it.socketId` | unchanged, but routed through `activate()` so id-hotkey `exit` also confirms |

### 9.4 What is still not implemented (needs the App, not `Menu`)

`graph`, `contr`, `demo`, `multi`, `chang` have no screen in the port.  The
closest win is `contr`: `EscMenu` already builds the recovered `control` screen
(`c\backCtrlOpt.txr`, sliders `forcel/centerl/sensl`, `autgb`, `c\canc`, `c\def`),
so `m\contr` should open it instead of printing "НЕ РЕАЛИЗОВАНО"; that is an
`App::run()` hunk, not a `Menu` one.  `m\singl` should open
`s\backSing.txr` (new | load) before entering the world — the port has the
save/load screens but jumps straight in.

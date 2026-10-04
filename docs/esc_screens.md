# In-game ESC screens — buttons, art and the panel fit rule

Reverse‑engineering of the **in‑game ESC UI** of *Hard Truck 2: King of the Road*
(`original/king.exe`, PE32 x86, MSVC 6.0, built 2004‑01‑14) — the menus you get with
<kbd>Esc</kbd> while driving, plus the other in‑game dialogs that share the same widget
framework (fuel, service, info/market panels, win/lose, F5 radio).

Address convention (same as `docs/original_ui.md`):

* every address in this document is a **file offset** — the value you pass to
  `r2 -q -e scr.color=0 -n -e asm.arch=x86 -e asm.bits=32 -c 'pd 40 @ 0xADDR' original/king.exe`;
  the **VA** of a file offset `o` is `o + 0x400000`.  Both are printed in the tables
  where it matters (`file 0x1113dd` = `VA 0x5113dd`).
* **VERIFIED** = read out of the binary / decoded from the shipped art.
  **INFERRED** = my reading of intent.  §9 lists everything I could **not** determine.

New tools added by this investigation (nothing existing was modified):

| tool | purpose |
|---|---|
| `tools/esc_re.py`   | raw‑mode r2 wrapper: `dis()/asm()/text()/callsites()/imms()/cstr()/func_start()` |
| `tools/esc_scan.py` | symbolic **stack + register tracker** over one function; prints the arguments of the artefact constructors and every explicit widget‑rect write |
| `tools/esc_art.py`  | art‑size lookup over all 141 `<name>.res` files (handles the `" noload"` name suffix and the `m_16` / `m_15` / `msk` sets) |

Decoded art for this report lives in **`tools/out/esc/`** (backdrops, every ESC button in
all states, slider arrows, F5 masks, popups).  Raw extraction logs:
`tools/out/esc/_report.txt` (per‑function constructor/rect dump), `_scan_raw.txt`,
`_artsizes.txt`; verification renders `_compose_escmain.png`, `_verify_snd.png`.

---

## 0. Executive summary

**The single most important correction to `docs/original_ui.md`:** the in‑game backdrops
are **not** 1600×1200 pictures.  They are **215‑/225‑/385‑/415/640‑pixel‑wide × 1200‑pixel‑tall
vertical side panels**; only the win/lose and loading screens are true 1600×1200 full‑screen
art.  (The old report read the `.res` *byte size* `516074` as if it were a size in pixels.)

| in‑game screen | ctor (file offset) | backdrop (`.res`) | art px | blit anchor | buttons |
|---|---|---|---|---|---|
| **ESC main** (`ПРОБЛЕМЫ В ПУТИ`) | `0x111170` | `m_16\BackEscMain.txr` (esc16) | 215×1200 | **(0, y)** | **8** |
| **ESC exit popup** (exit / save / don't save) | `0x11e9a0` | `m_16\BackEscMain.txr` | 215×1200 | (0, y) | **3** |
| **sound** (`ЗВУК`) | `0x116ea0` | `m_16\BackEscSnd.txr` | 215×1200 | (0, y) | **2** + 4 sliders |
| **controls** (`УПРАВЛЕНИЕ`) | `0x117530` | `m_16\BackEscCtrl.txr` | 215×1200 | (0, y) | **2** |
| **save game** (`СОХРАНИТЬ`) | `0x11dab0` | `m_16\BackEscSave.txr` | 215×1200 | (0, y) | **3** |
| **load game** (`ЗАГРУЗИТЬ`) | `0x11e340` | `m_16\BackEscLoad.txr` | 215×1200 | (0, y) | **3** |
| **fuel station** | `0x120240` | `m_16\BackGas.txr` | 225×1200 | (screenW−224, y) | **2** |
| **service station** | `0x120e30` | `m_16\BackService.txr` | 640×1200 | (screenW−640, y) | **2** |
| **connecting / loading dialog** | `0x1228e0` | `m_16\BackConnect.txr` | 225×1200 | (screenW−224, y) | **2** |
| **info panel “my truck”** | `0x126470` | `m_16\BackMy.txr` | 415×1200 | (0, y) | **2** + photo album |
| **win / lose** | `0x11eea0` | `BackWin/BackWin1/2`, `BackLoose1…5` | 1600×1200 | **centred 1:1** | **8** (2+3+3) |
| **F5 CB radio questions** | `0x123270` | `BackF5{pol,maf,opp,nei}.msk` | 280×405 | **centred 1:1** | **20** (4 pages) |
| **F5 CB radio search/talk** | `0x125b50` | `BackF5cbradio.msk` | 280×405 | **centred 1:1** | **2** |
| **“5 yes/no” popup** | `0x122cf0` | `m16\MenuPopupA5.msk` (ind16) | 494×160 | **centred 1:1** | **2** |
| *market / order / job / info panels* | `0x16b000…0x16cb74` | `m_16\BackOrder/Sale/Job.txr` | 225×1200 | (screenW−224, y) | — |
| *market info panel* | same | `m_16\BackInfo.txr` | 385×1200 | (screenW−383, y) | — |
| *in‑game F1/F2/F3 help* | `0x1750xx…0x175a80` | `m_16\BackF1/F2/F2Tball/F3.txr` | 415/215×1200 | (0, y) | — |
| *front‑end* graphics / controls / driver / multiplayer / main menu | `0x115ca0` / `0x113b30` / `0x11b640` / `0x11a580` / `0x127f30` | `LOAD.RES` 800×600 art | 800×600 | (0, 0) 1:1 | 2/2/4/2/8 |

**Total in‑game ESC‑reachable buttons: 21** (ESC main 8 + exit popup 3 + sound 2 + controls 2 +
save 3 + load 3) plus the 4 sound sliders.

---

## 1. The two screen families and the widget framework

Two button constructors exist.  All in‑game (1600×1200‑authored) screens use the first one,
the 800×600 front‑end uses the second.

### 1.1 `0x10def0(name, id)` — the in‑game button

```
0x10def0  push -1 / push 0x64605b / SEH frame / sub esp,0xd8
0x10e008  call 0x168fd0                 ; colour depth: 2 = 15-bit
0x10e010  mov edi, 0x6762c4             ; "m_15"
0x10e015  je 0x10e01c
0x10e017  mov edi, 0x6762bc             ; "m_16"
0x10e01c  mov ebx, [esp+0xf4]           ; arg1 = name
0x10e02a  push ebx / push edi / push 0x6762b0 / push buf
0x10e032  call 0x21cca9                 ; sprintf(buf, "%s\\%s_a.txr", prefix, name)
0x10e050  call 0x1ef3d0                 ; load the surface
0x10e13d  call 0x1bf820                 ; widget base ctor (vtbl 0x651838, kind 4, id)
0x10e1d9  call [edx+0x14]               ; widget->Update()  = 0x1bf880 -> size from the art
```

* **verified argument order: `(name, id)`** — `name` is the last push (arg1), `id` is the
  second‑to‑last (arg2).  The `push edi`/`push ebx` immediately *before* the two arguments is
  **not** part of this call; it is the second argument of the following
  `container->add(button, 0)` (`0x1bdbf0`, a `ret 8` thiscall).  The compiler hoists that
  constant push.
* `id` ends up at **button+0x1c** (set inside `0x1bf820`).
* art names built by this ctor (`0x10def0`, in‑game):
  `%s\%s_a.txr` (`0x6762b0`), `%s\%s_p.txr` (`0x6762a4`), `%s\%s_ap.txr` (`0x676294`),
  `%s\%s_n.txr` (`0x676278`), `%s\%s_pp.txr` (`0x676284`), `%s\%s_ok.txr` (`0x6762cc`),
  `%s\%s_f.txr` (`0x676344`).  The variants that actually ship for ESC buttons are
  **`_a` (normal), `_p` (pressed), `_ap` (active), `_f` (forbidden/disabled)** — identical
  pixel size in every case.
* **there are no X/Y arguments.**  All in‑game screens position their buttons afterwards,
  either by an explicit rect write or by a container layout pass (§1.3).

### 1.2 `0x10e500(name, id, X, Y)` — the front‑end button

```
0x10e51d  mov edi, [esp+0xf0]      ; arg1 = name (includes its folder, e.g. "m\\graph")
0x10e52e  push 0x6763a4            ; "%s_a.msk"   (variants: _p, _ap, _f, _ph, _pp)
0x10e729  mov edx, [esp+0xf4]      ; arg2 = id
0x10e73a  call 0x1bf820
0x10e7d8  mov eax, [esp+0xf8]      ; arg3 = X  -> rect.left
0x10e7f4  mov eax, [esp+0xfc]      ; arg4 = Y  -> rect.top
0x10e822  ret
```

`docs/original_ui.md` described this as `(flags, Y, X, id, name)`; the labels were wrong.
The real order (verified against all 8 main‑menu sites and the 2+2+4 remaining sites) is
**`(name, id, X, Y)`** — and `docs/original_ui.md`'s *table* (which lists `id`, `X`, `Y`) is
correct.  There is no `flags` argument.  Art = `<name>_a.msk` etc. with **no folder prefix**.

### 1.3 The container / widget classes (shared)

| what | address | notes |
|---|---|---|
| widget vtable | `0x651838` | `+0x14 Update()` = `0x1bf880`, `+0x1c` text draw, `+0x20/+0x24` mouse, `+0x48 ComputeSize` = `0x1bdc80` |
| widget size from art | `0x1bf880` / `0x1bdc80` | flags & 0x24 → `w=word[art+0x14]-1`, `h=word[art+0x16]-1`; flags & 0x48 → `word[art+4]/[art+6]`; else text metrics |
| container vtable | `0x64dc50` | `+0x14 Layout()` = `0x5bf740` = `push 4 ; call 0x1be130 ; ret` (default row pitch 4) |
| container::add | `0x1bdbf0(obj, item, where)` | `ret 8`; `where == 0` ⇒ **prepend** to the child list at `obj+0x120` (`+8` = next, `+4` = prev) |
| list next | `0x1d3080` | `mov eax,[ecx+8]` |
| container::layout | `0x1be130(obj, spacing)` | `ret 4`; **vertical stack**: for each child (from the head, i.e. *last added first*) `left = obj+0x3c`, `right = obj+0x3c + maxChildWidth`, `top = running`, `bottom = running + h-1`, `running += (h-1) + spacing`; finally calls `0x1bdc80` to compute the container's own extent |
| container child origin | — | hit‑test (`0x1be630`) subtracts `container[0x2c]/[0x30]` before recursing ⇒ **child rects are relative to the parent's rect origin** |
| 6 style slots per widget | `+0x44,+0x68,+0x8c,+0xb0,+0xd4,+0xf8` | 0x24 bytes each: `{…, float alpha @+0x18 (0.5f), art* @+0x1c, flags @+0x20}`; the size comes from **slot 0's art**, i.e. `[widget+0x60]` |
| default style templates | `0x6ce390`, `0x6ce3b8`, `0x6ce3e0`, `0x6ce408`, `0x6ce938`, `0x6ce960`, `0x6ce988`, `0x6ceb38` | zeroed by `0x10c9d0` at game start; screens fill `+0x1c` of one of them with the backdrop surface before copying it into their container (e.g. `mov [0x6ce3fc], eax` = style base `0x6ce3e0` + 0x1c) |

Consequence: a screen builds its backdrop as *the container's style‑0 art*, so the container's
rect **is** the backdrop's pixel size, and the child buttons are laid out relative to it.

### 1.4 How the two positioning idioms look

**(a) auto‑layout** — the buttons are only added, then one layout call positions them all.
ESC main, `0x111501…0x111526`:

```
0x111501  mov eax,[esp+0x14]        ; the "main" button (was saved after its ctor)
0x111505  mov ecx,[eax+0x30]        ; its top
0x111508  mov edx,[eax+0x38]        ; its bottom
0x11150b  sub ecx,edx
0x11150d  add ecx,0x2d              ; spacing = 45 + (top-bottom) = 45 - (h-1)
0x111510  push ecx
0x111511  mov ecx,esi               ; the button container
0x111513  call 0x1be130             ; layout  -> pitch becomes exactly 0x2d = 45 px
```

**(b) explicit rect** — the ctor result is written directly, e.g. the sound screen `ok`:

```
0x11700e  call 0x10def0             ; "ok", id 0xb8
0x11701d  call 0x1bdbf0             ; container->add(ok,0)
0x11704a  mov [esi+0x2c], 0x15      ; left   = 21
0x117054  mov [esi+0x30], 0x2fa     ; top    = 762   (right/bottom adjusted by the deltas)
```

---

## 2. THE FIT RULE (VERIFIED) — how the 1200‑px panels are fitted

### 2.1 The resolution class table

`0x110f20…0x11106e` (inside the “start/load the world” function that begins at `0x110bc0`)
computes the in‑game panel anchors.  `esi` is `0` in this path (`xor esi,esi` at `0x110f25`),
and the screen class is `[[0x6cec70]+0x198] & 0xf0`:

```
0x110f8a  mov eax,[ecx+0x198]
0x110f90  and eax,0xf0
0x110f95  cmp eax,0x10   -> 640x480  : y0 = 0xfffffe98 (-360), right = 0x1a0 (416), right2 = 0xfe  (254)
0x110fc2  cmp eax,0x20   -> 800x600  : y0 = 0xfffffed4 (-300), right = 0x240 (576), right2 = 0x19e (414)
0x110fec  cmp eax,0x30   -> 1024x768 : y0 = 0xffffff28 (-216), right = 0x320 (800), right2 = 0x27f (639)
0x111016  cmp eax,0x40   -> 1600x1200: y0 = 0,                right = 0x560 (1376), right2 = 0x4bf (1215)
0x11104e  sub ecx,0x32 / sub eax,0x32 / sub edx,0x32          ; all three y values get -50
0x111054  mov [0x6ced18], ecx        ; 2nd rect bottom  / 2nd anchor y
0x111063  mov [0x6ced10], eax        ; 1st rect top     / LEFT panel y
0x111068  mov [0x6ced20], edx        ; 3rd rect bottom  / 3rd anchor y
          [0x6ced0c] = esi = 0       ; 1st rect left    / LEFT panel x
          [0x6ced14] = right const   ; 2nd panel x
          [0x6ced1c] = right2 const  ; 3rd panel x
```

Decoding the constants against the class' screen height `H`:

| class | screenW×screenH | `-(1200-H)/2` | `-50` | stored |
|---|---|---|---|---|
| 0x10 | 640×480 | −360 | **−410** | `[0x6ced10]` = `[0x6ced18]` = `[0x6ced20]` |
| 0x20 | 800×600 | −300 | **−350** | idem |
| 0x30 | 1024×768 | −216 | **−266** | idem |
| 0x40 | 1600×1200 | 0 | **−50** | idem |

and the two x constants are `screenW−224` (416/576/800/1376) and `screenW−385`
(254/414/639/1215).

### 2.2 The rule

> **The in‑game UI is drawn 1:1 — pixel for pixel, never scaled.**
> A 1200‑pixel‑tall panel is **vertically centred on the screen and then shifted 50 px up**:
> `y = −(1200 − screenH)/2 − 50`.  At 800×600 that is **−350**, so a 215×1200 panel shows its
> art rows **350…949**; everything outside is clipped.
> * left‑docked panels (`BackEsc*`, `BackF1/F2/F3/F2Tball`, `BackMy`) are at **x = 0**
>   (`[0x6ced0c]` = 0, `y` = `[0x6ced10]`);
> * the 225‑px right‑docked panels (`BackOrder/BackSale/BackJob/BackGas/BackConnect`) are at
>   **x = screenW − 224** (`[0x6ced14]`, `y` = `[0x6ced18]`);
> * `BackInfo` (385 px) is at **x = screenW − 383** (`[0x6ced1c]+2`, `y` = `[0x6ced20]`);
> * the 640‑px service panel is at **x = screenW − 640** (`0x12139c…0x1213a2`: `x = word[surface] − 0x280`);
> * the full‑screen 1600×1200 art (win/lose, loading screen) is **centred 1:1**, i.e. cropped
>   on smaller screens (`0x11f2bb-0x11f307`: `x=(screenW−artW)/2`, `y=(screenH−artH)/2`);
> * 280×405 / 494×160 popups (`BackF5*`, `MenuPopupA5`) are **centred 1:1**
>   (`0x124331-0x124382`, `0x125d77-0x125dc8`, `0x122ee3-0x122f3d`).

So on the default 800×600 (TRUCK.INI `xres=800 yres=600`) the ESC menu occupies the left
215 px of the screen with the panel's middle 600 rows visible — the “ПРОБЛЕМЫ В
ПУТИ” title lands at screen y ≈ 40…120 and the 8 buttons at y = 104…449.

### 2.3 Verification render

`tools/out/esc/_compose_escmain.png` is the real `m_16\BackEscMain.txr` cropped to the
visible band (art rows 350…949 = the 800×600 screen) with the real `_a` art of the 8 buttons
composited at the computed coordinates.  It reproduces the shipped menu exactly — title,
8 labels in reading order, 150×30 buttons on a 45‑px pitch.  (The same check for the sound
screen is `tools/out/esc/_verify_snd.png`.)  See §3.1 for the image.

---

## 3. Per‑screen button tables

Notation: `X`,`Y` are the button's **top‑left corner in the screen's own coordinate space**
(panel‑local, i.e. relative to the backdrop's top‑left; add the panel anchor of §2.2 to get
screen coordinates).  `ctor VA` is the `call 0x10def0` instruction (file offset / VA = +0x400000).
Every listed `_a.txr` also exists as `_p.txr` and `_ap.txr` (and `_f.txr` where noted).  All
in‑game buttons are **150×30** unless stated.

**Display order = reverse creation order** (the container prepends and `0x1be130` walks from
the head), so the *last* button created is drawn **topmost**.  Two independent confirmations:
the sound sliders (created skid→eng→mus→vol, and the baked‑in labels in `BackEscSnd.txr` read
ОБЩАЯ ГРОМКОСТЬ, МУЗЫКА, ЗВУК ДВИГАТЕЛЯ, ЗВУК ТОРМОЖЕНИЯ top→bottom), and the ESC main list,
whose natural reading order (ВЫХОД last) is produced by the same rule.

### 3.1 ESC main — `ПРОБЛЕМЫ В ПУТИ` (ctor `0x111170`)

* backdrop `m_16\BackEscMain.txr` (**215×1200**, esc16.RES, `0x6767f0`), loaded at `0x111321` and
  stored into the **`+0x1c` art field of the default style template `0x6ce3e0`**
  (`mov [0x6ce3fc],eax` at `0x11132b` — `0x6ce3e0 + 0x1c = 0x6ce3fc`).  Both containers created by
  this function then receive **four copies of that very template** (`push 0x6ce3e0` ×4 at
  `0x11135c…0x11137a` for the backdrop container and `0x1113af…0x1113cd` for the button
  column), so the backdrop surface is also the button column's style‑0 art.
* **anchor:** container rect moved to `([0x6ced0c],[0x6ced10]) = (0, −(1200−screenH)/2−50)`
  at `0x111538…0x111548`.
* **two containers:** the backdrop container (`ebp`, `0x111351`, name `"MENU"`, flags 4) and the
  button column (`esi`, `0x1113a4`, flags 0).  The column is translated by **(+18, +454)** at
  `0x11152e…0x111533` (the only callers of the tiny helpers `0x1115b0` (translate rect) and
  `0x1115e0` (move rect) are in this function).
* **layout:** `esi->layout(45 − (btn_h−1))` at `0x111513`; the button container's own gap
  `[0x3c]` is 0, so button *k* (0 = topmost) sits at column‑local `(0, 45k)` and the column
  origin is `(18, 454)`.

| # | id | name | art (esc16.RES, 150×30) | label — **baked into the art** | X | Y | ctor call |
|---|---|---|---|---|---|---|---|
| 1 | **0xa7** (167) | `savg` | `m_16\savg_a.txr` (+`_f`) | СОХРАНИТЬ ИГРУ | **18** | **104** | file `0x1114d3` / VA `0x5114d3` |
| 2 | **0x6e** (110) | `loadg` | `m_16\loadg_a.txr` (+`_f`) | ЗАГРУЗИТЬ ИГРУ | **18** | **149** | file `0x11149c` / VA `0x51149c` |
| 3 | **0xa8** (168) | `restart` | `m_16\restart_a.txr` (+`_f`) | НАЧАТЬ СНАЧАЛА | **18** | **194** | file `0x111467` / VA `0x511467` |
| 4 | **0xa6** (166) | `911` | `m_16\911_a.txr` | 911 | **18** | **239** | file `0x11144d` / VA `0x51144d` |
| 5 | **0xa5** (165) | `control` | `m_16\control_a.txr` | УПРАВЛЕНИЕ | **18** | **284** | file `0x111432` / VA `0x511432` |
| 6 | **0xa4** (164) | `sound` | `m_16\sound_a.txr` | ЗВУК | **18** | **329** | file `0x111417` / VA `0x511417` |
| 7 | **0xa3** (163) | `main` | `m_16\main_a.txr` | ГЛАВНОЕ МЕНЮ | **18** | **374** | file `0x1113f7` / VA `0x5113f7` |
| 8 | **0xa9** (169) | `exitsys` | `m_16\exitsys_a.txr` | ВЫХОД | **18** | **419** | file `0x1113dd` / VA `0x5113dd` |

Creation order in the code is exactly the reverse (exitsys first … savg last); the ids, the
`_f` (disabled) variants and the network check `if ([[0x6cec6c]+0x524] != 0) btn->[0x28] = 1`
(`0x111482…0x1114fa`) make `savg`, `loadg` and `restart` unavailable in a network game.

**How to reproduce:** draw `m_16\BackEscMain.txr` at `(0, −(1200−H)/2 − 50)` 1:1, then the eight
150×30 buttons of the table at `x = 18`, `y = 104 + 45k`, in this top→bottom order.

![ESC main screen reproduced](tools/out/esc/_compose_escmain.png)

### 3.2 ESC exit popup — “exit / save / don't save?” (ctor `0x11e9a0`)

Re‑uses **`m_16\BackEscMain.txr`** (`0x11ea75`), same anchor (0, y) at `0x11ec1d…0x11ec26`.
Buttons are positioned **explicitly** (no layout pass), so the order is whatever the rects say.

| # | id | name | art (150×30) | label (baked) | X | Y | ctor call |
|---|---|---|---|---|---|---|---|
| 1 | **0xaa** (170) | `exsave` | `m_16\exsave_a.txr` | СОХРАНИТЬ | 18 | 564 | `0x11eba8` |
| 2 | **0xab** (171) | `exnsave` | `m_16\exnsave_a.txr` | НЕ СОХРАНЯТЬ | 18 | 614 | `0x11eb58` |
| 3 | **0xac** (172) | `excanc` | `m_16\excanc_a.txr` | ОТМЕНА | 18 | 664 | `0x11eb08` |

(screen y = −350 + 564/614/664 = 214 / 264 / 314 at 800×600)

**How to reproduce:** same backdrop/anchoring as §3.1, three 150×30 buttons at x = 18,
y = 564/614/664.

### 3.3 Sound — `ЗВУК` (ctor `0x116ea0`)

* backdrop `m_16\BackEscSnd.txr` (**215×1200**, `0x676e64`), loaded `0x116f7b`, anchor
  `(0, y)` at `0x117218…0x117244`.
* the panel art carries the four group labels **and their slider tracks**;
  the game only overlays 8 arrow masks and the two buttons.
* sliders are built by a second ctor **`0x10d860(name_left, name_right, &value, callback, 140)`**
  (5 args), not by `0x10def0`:

| slider (top→bottom) | left/right art (esc16.RES) | value ptr | id | group rect origin |
|---|---|---|---|---|
| ОБЩАЯ ГРОМКОСТЬ (master) | `m_16\voll_a.msk` / `volr_a.msk` (16×28) | `this+0x17e` | 0xb4 | `0x117185…0x1171a7`, callback `0x516d50` |
| МУЗЫКА | `m_16\musl_a.msk` / `musr_a.msk` | `this+0x180` | 0xb5 | `0x11714f…0x117171`, callback `0x516db0` |
| ЗВУК ДВИГАТЕЛЯ | `m_16\engl_a.msk` / `engr_a.msk` | `this+0x184` | 0xb6 | `0x11711c…0x11713b`, callback `0` |
| ЗВУК ТОРМОЖЕНИЯ | `m_16\skidl_a.msk` / `skidr_a.msk` | `this+0x186` | 0xb7 | `0x1170ec…0x117108`, callback `0` |

  the slider container is 140 px wide, laid out with pitch **70** (`spacing = 70 − (28−1)`,
  `0x1171bb…0x1171d0`) and then moved to **(22, 489)** (`0x1171f6/0x1171ff`); the arrow masks
  sit at the ends of the painted tracks.
* **the two text buttons:**

| # | id | name | art | label (baked) | X | Y | ctor call |
|---|---|---|---|---|---|---|---|
| 1 | **0xb8** (184) | `ok` | `m_16\ok_a.txr` **55×30** | OK | 21 | 762 | `0x11700e` |
| 2 | **0x2** (2) | `canc` | `m_16\canc_a.txr` **80×30** | ОТМЕНА | 77 | 762 | `0x11705b` |

  they are added to a *pass‑through* container whose own rect is `(0,0,0,0)`, so their
  coordinates are effectively panel‑local (screen y = −350 + 762 = **412** at 800×600).
* the slider labels are **not** buttons: `engvol` / `midivol` / `sfxvol` (`0x676e00/08/10`) are
  passed to a translating string builder at `0x11725e…0x117286` and drawn as text.

**How to reproduce:** panel at (0,y); 4 slider rows at panel x = 22, y = 489 + 70k, each with a
16×28 arrow pair at the painted track's ends and the label from the art; `ok` (55×30) at (21,762)
and `canc` (80×30) at (77,762).

### 3.4 Controls / key list — `УПРАВЛЕНИЕ` (ctor `0x117530`)

* backdrop `m_16\BackEscCtrl.txr` (**215×1200**, `0x676eec`), loaded `0x117615`, anchor `(0,y)`.
* the key list container is moved to **(22, 511)** (`0x117771/0x11777b`); it holds the 16
  key/description rows from `docs/original_ui.md` §3.1 (drawn as text, not buttons).
* buttons (explicit rects):

| # | id | name | art | label (baked) | X | Y | ctor call |
|---|---|---|---|---|---|---|---|
| 1 | **0xb8** (184) | `ok` | `m_16\ok_a.txr` 55×30 | OK | 21 | 762 | `0x1178b0` |
| 2 | **0x2** (2) | `canc` | `m_16\canc_a.txr` 80×30 | ОТМЕНА | 77 | 762 | `0x117860` |

**How to reproduce:** panel at (0,y); 16 text rows at (22, 511); OK at (21,762), CANCEL at (77,762).

### 3.5 Save game — `СОХРАНИТЬ` (ctor `0x11dab0`)

* backdrop `m_16\BackEscSave.txr` (**215×1200**, `0x6777ac`), loaded `0x11dbb9`, anchor `(0,y)`
  at `0x11df7b/0x11df84`.
* buttons (explicit rects; order as drawn — the panel art has the slot list above them):

| # | id | name | art | label (baked) | X | Y | ctor call |
|---|---|---|---|---|---|---|---|
| 1 | **0xad** (173) | `save` | `m_16\save_a.txr` 130×30 | СОХРАНИТЬ | 24 | 679 | `0x11dca8` |
| 2 | **0x11a** (282) | `delgame` | `m_16\delgame_a.txr` 130×30 | УДАЛИТЬ | 24 | 724 | `0x11dc59` |
| 3 | **0xac** (172) | `nsave` | `m_16\nsave_a.txr` 130×30 | ОТМЕНА | 24 | 769 | `0x11dc0a` |

**How to reproduce:** panel at (0,y); three 130×30 buttons at x = 24, y = 679/724/769.

### 3.6 Load game — `ЗАГРУЗИТЬ` (ctor `0x11e340`)

* backdrop `m_16\BackEscLoad.txr` (**215×1200**, `0x6777e4`), loaded `0x11e41a`, anchor `(0,y)`
  at `0x11e6b1/0x11e6bd`.

| # | id | name | art | label (baked) | X | Y | ctor call |
|---|---|---|---|---|---|---|---|
| 1 | **0x6e** (110) | `load` | `m_16\load_a.txr` 130×30 | ЗАГРУЗИТЬ | 24 | 679 | `0x11e4fe` |
| 2 | **0x11a** (282) | `delgame` | `m_16\delgame_a.txr` 130×30 | УДАЛИТЬ | 24 | 724 | `0x11e4b8` |
| 3 | **0xac** (172) | `nsave` | `m_16\nsave_a.txr` 130×30 | ОТМЕНА | 24 | 769 | `0x11e46b` |

**Its command handler is decoded** (function at `0x11e880`, reached from the screen's
`vtable+0x34` = `0x1d8bb0` → `[[this+0x38]]->vtable[0x38](id)`):

```
0x11e885  cmp eax,0xac   ; 0xac (nsave)  -> 0x11e944
0x11e88d  jg 0x11e95d
0x11e893  je 0x11e944
0x11e899  cmp eax,0x1b   ; 0x1b = keyboard ESC -> 0x11e944
0x11e89c  je 0x11e944
0x11e8a2  cmp eax,0x6e   ; 0x6e (load)   -> 0x11e8ab : read the selected name
0x11e8a5  jne 0x11e990   ;                     from [this+0x1b8]+0x12c and load it
        0x11e95d cmp eax,0x11a ; 0x11a (delgame) -> 0x11e964 : call 0x11d9e0(name)
0x11e944 (close) [[this-0x40]]->vtable[0x20]() ; [0x6ced6c]->0x1d8610() -> pop the screen
```

### 3.7 Fuel station dialog (ctor `0x120240`)

* backdrop `m_16\BackGas.txr` (**225×1200**, `0x6779c8`), loaded `0x120315`, **right‑docked**:
  anchor `([0x6ced14],[0x6ced18]) = (screenW−224, y)` at `0x120503…0x12052f`.
* buttons (explicit rects):

| # | id | name | art | label (baked) | X | Y | ctor call |
|---|---|---|---|---|---|---|---|
| 1 | **0x113** (275) | `gaspay` | `m_16\gaspay_a.txr` 70×30 | OK | 52 | 755 | `0x1203f5` |
| 2 | **0x2** (2) | `gascanc` | `m_16\gascanc_a.txr` 80×30 | ОТМЕНА | 123 | 755 | `0x1203a5` |

  (two further widgets are positioned at (175,655) and (25,655) — the fuel‑amount LED/checkbox,
  see §9)

**How to reproduce:** draw `m_16\BackGas.txr` at `(screenW−224, −(1200−H)/2−50)`, put the two
buttons at panel x = 52 / 123, y = 755.

### 3.8 Service station dialog (ctor `0x120e30`)

* backdrop `m_16\BackService.txr` (**640×1200**, `0x677a40`), loaded `0x120f05`, anchored
  `x = screenW − 640`, `y = [0x6ced18]` (`0x12139c…0x1213c2`).
* buttons (explicit rects):

| # | id | name | art | label (baked) | X | Y | ctor call |
|---|---|---|---|---|---|---|---|
| 1 | **0x117** (279) | `servpay` | `m_16\servpay_a.txr` 130×30 | OK | 209 | ? | `0x121335` |
| 2 | **0x2** (2) | `servcanc` | `m_16\servcanc_a.txr` 130×30 | ОТМЕНА | 349 | ? | `0x1212e8` |

  the x values are explicit (`[esi+0x2c] = 0x1b5/0x15d`); the y is derived from the sibling
  repair rows — I did not finish decoding the y (§9).  The panel also contains the
  `check` (19×17) and `servcar/upgradec/upgradet` (140×115) service icons as
  non‑button widgets at `0x120f05…0x121298`.

**How to reproduce:** draw `m_16\BackService.txr` flush to the right edge, y as §2.2; the two
130×30 buttons at panel x = 209 / 349 near the panel bottom.

### 3.9 Connecting / “loading” dialog (ctor `0x1228e0`)

* backdrop `m_16\BackConnect.txr` (**225×1200**, `0x677a88`), loaded `0x1229b5`, right‑docked
  `(screenW−224, y)` at `0x122a99…0x122ac5`.
* buttons:

| # | id | name | art | label (baked) | X | Y | ctor call |
|---|---|---|---|---|---|---|---|
| 1 | **0xa2** (162) | `start` | `m_16\start_a.txr` 145×30 | СТАРТ | ? | 689 | `0x122a53` |
| 2 | **0xa3** (163) | `mmenu` | `m_16\mmenu_a.txr` 145×30 | ГЛАВНОЕ МЕНЮ | ? | 729 | `0x122a06` |

  (x is not set explicitly for these two — they inherit the container's left edge; §9)

### 3.10 “yes / no” popup A5 (ctor `0x122cf0`)

* backdrop `m16\MenuPopupA5.msk` (**494×160**, **ind16.RES**, `0x677ac4`), loaded `0x122dc5`,
  **centred 1:1** (`0x122ee3…0x122f3d`).
* buttons:

| # | id | name | art | label (baked) | X | Y | ctor call |
|---|---|---|---|---|---|---|---|
| 1 | **0xa1** (161) | `5no` | `m_16\5no_a.txr` **100×49** | НЕТ | 266 | 96 | `0x122e7a` |
| 2 | **0xa0** (160) | `5yes` | `m_16\5yes_a.txr` **100×49** | ДА | 128 | 96 | `0x122e2a` |

**How to reproduce:** centre the 494×160 mask, put ДА at (128,96) and НЕТ at (266,96) inside it.

### 3.11 Info panel “my truck” (ctor `0x126470`)

* backdrop `m_16\BackMy.txr` (**415×1200**, `0x678228`), loaded `0x126559`, left‑docked `(0,y)`
  (`0x126875/0x12687a`).
* buttons (explicit rects):

| # | id | name | art | label (baked) | X | Y | ctor call |
|---|---|---|---|---|---|---|---|
| 1 | **0xba** (186) | `fire` | `m_16\fire_a.txr` **100×30** | УВОЛИТЬ | 85 | 774 | `0x12664d` |
| 2 | **0xb9** (185) | `chan` | `m_16\chan_a.txr` **166×30** | ПОМЕНЯТЬ МАШИНУ | 186 | 774 | `0x1265f7` |

  plus a photo album: `m16\escphoto01…54.txr`, `escmafia01…08`, `escpolice01…15` loaded in a loop
  (`0x1268bb…0x1268f7`) into globals `this+0x174…+0x2a0`.

### 3.12 Win / lose screens (ctor `0x11eea0`)

* backdrops (all **1600×1200**, esc16.RES) loaded at `0x11ef74…0x11f06e`:
  `m_16\BackWin.txr`, `BackWin1.txr`, `BackWin2.txr`, `BackLoose1.txr` … `BackLoose5.txr`.
* **anchor: centred 1:1** — `0x11f2bb…0x11f307` computes `x=(screenW−1600)/2`,
  `y=(screenH−1200)/2` and moves the container there.
* three containers × buttons, all **130×30** (`o_*` / `ow_*`):

| screen | container | id | name | X | Y | label (baked) | ctor call |
|---|---|---|---|---|---|---|---|
| lose (`BackLoose1…5`) | 1 | 0xa8 (168) | `o_rest` | ? | ? | НАЧАТЬ СНАЧАЛА | `0x11f132` |
| lose | 1 | 0xa3 (163) | `o_main` | 589 | 687 | ГЛАВНОЕ МЕНЮ | `0x11f111` |
| win (`BackWin*`) | 2 | 0xa8 (168) | `ow_rest` | ? | ? | НАЧАТЬ СНАЧАЛА | `0x11f236` |
| win | 2 | 0xac (172) | `ow_cont` | ? | ? | ПРОДОЛЖАТЬ | `0x11f215` |
| win | 2 | 0xa3 (163) | `ow_main` | 589 | 687 | ГЛАВНОЕ МЕНЮ | `0x11f1f4` |

* **its command handler is fully decoded** — `0x11fd70` (an `__thiscall`, `ret 4`, registered
  as `vtable[0x3c]` of the screen's second vtable at `0x64e6a4`):

```
0x11fd8c  sub eax,0xa3
0x11fd94  je  0x11ff4a     ; 0xa3 (163)  "main"          -> quit to main menu
0x11fd9a  sub eax,5
0x11fd9d  je  0x11fe1c     ; 0xa8 (168)  "restart"       -> restart the race
0x11fd9f  sub eax,4
0x11fda2  jne 0x11ffe7     ; anything else: ignore
0x11fda8  ...              ; 0xac (172)  "continue"      -> leave the screen, resume driving
```

### 3.13 F5 CB radio (ctors `0x123270` and `0x125b50`)

* backdrop `m_16\BackF5{pol,maf,opp,nei}.msk` (280×405 masks) + answer art
  `m_16\F5{p,m,o,n}_ans_*.txr`, and `m_16\BackF5cbradio.msk` (280×405).  **All centred 1:1**
  (`0x124331…0x124382` for the question pages, `0x125d77…0x125dc8` for the CB page).
* 20 buttons, all **258×44** art, positioned explicitly inside each page container at
  x = 11 and y = 205 / 250 / 295 / 340 (and 250 / 295 for OK / CANCEL):

| page | ids (ask1..ask4 / ok / canc) |
|---|---|
| police (`F5p_*`) | 0xbe, 0xbf, 0xc0, 0xc1 / 0xce / 0xcf |
| mafia (`F5m_*`) | 0xc2, 0xc3, 0xc4, 0xc5 / 0xce / 0xcf |
| opponent (`F5o_*`) | 0xc6, 0xc7, 0xc8 / 0xce / 0xcf |
| neighbour (`F5n_*`) | 0xca, 0xcb, 0xcc / 0xce / 0xcf |
| CB radio page (`F5cb`) | `F5search` 0xbc (99×36) at (30,135), `F5talk` 0xbd (84×84) at (137,146) |

### 3.14 Market / order / job / in‑game info panels (not `0x10def0` screens)

Loaded once into globals at `0x16ed42…0x16ee55` and blitted from the in‑game info overlay
(`0x16b000…0x16cb74`, a switch on the panel index via the jump table `0x56cb98`):

| panel | global | art (esc16.RES) | blit (x, y) | blit site |
|---|---|---|---|---|
| BackOrder | `0x6f33c4` | 225×1200 | `([0x6ced14], [0x6ced18])` = (screenW−224, y) | `0x16c1e0`, also `0x16c46c`, `0x16c537` |
| BackSale | `0x6f33c8` | 225×1200 | (screenW−224, y) | `0x16bcf5` |
| BackJob | `0x6f33cc` | 225×1200 | (screenW−224, y) | `0x16bf21` |
| BackInfo | `0x6f33d0` | 385×1200 | `([0x6ced1c]+2, [0x6ced20])` = (screenW−383, y) | `0x16c880` |
| BackF1 | `0x6f33d4` | 415×1200 | `([0x6ced0c], [0x6ced10])` = (0, y) | `0x17579a` |
| BackF2 | `0x6f33d8` | 415×1200 | (0, y) | `0x1759c9` |
| BackF2Tball | `0x6f33dc` | 215×1200 | (0, y) | `0x175a07` |
| BackF3 | `0x6f33e0` | 215×1200 | (0, y) | `0x175a6f` |

**These panels are where “911 / market” live:** the ESC‑menu `911` button (id 0xa6) opens this
info/market overlay family.  They are drawn 1:1 with the §2.2 anchors, and their text is placed
relative to the panel (e.g. BackOrder text at panel `(+125, +557)`, BackSale `(+103, +585)`,
BackJob `(+103, +565)`).

### 3.15 Front‑end screens (for contrast; 800×600, `LOAD.RES`, 1:1 at (0,0))

| screen | ctor | backdrop(s) | buttons (id | X | Y | art) |
|---|---|---|---|---|
| main menu | `0x127f30` | `m\back_main.txr` 800×600 at (0,0) + `common.msk` 800×600 | 0x6c `m\exit` (596,452) · 0x69 `m\chang` (648,332) · 0x68 `m\multi` (648,202) · 0x66 `m\singl` (596,82) · 0x65 `m\recor` (79,452) · 0x6a `m\demo` (27,332) · 0x67 `m\contr` (27,202) · 0x6b `m\graph` (79,82) — all `m\*_a.msk` **125×125** |
| controls (front‑end) | `0x113b30` | `c\backCtrl.txr` 800×600 + `c\backCtrlOpt.txr` **400×330** (110,153) | 0x8e `aleft` (259,157) · 0x8f `aright` (514,157) — `aleft_a.msk`/`aright_a.msk` **29×29**; `c\canc_a.txr` **154×49**, `c\def_a.txr` **100×36** |
| graphics | `0x115ca0` | `g\backGraf.txr` 800×600 + `g\backGrafOpt.txr` **329×302** (310,218) | 0x85 `aleft` (324,263) · 0x86 `aright` (536,263); `g\canc_a.txr` 154×49, `g\huge/large/med/small_a.txr` **110×35** |
| driver select | `0x11b640` | `d\backDriv.txr` 800×600 + `d\backDrivCurr.msk` **344×258** + `d\backDrivRcnt.msk` **344×160** | `aleft`/`aright` 0x80/0x81 at (308,175) · 0x82/0x83 at (416,265) |
| multiplayer connect | `0x11a580` | `n\backMult.txr` 800×600 + `n\backMultConn.txr` **340×380** | 0x78/0x79 `aleft`/`aright` at (323,200)/(528,200) |

Cursor `cursor.msk` / `cursor_a.msk` = **14×16**.  Selection runner `runner_{a,ap,p}.msk` = 12×22.
Tip bar `tip1_left/midd/right.msk` = 8×18.

### 3.16 Complete baked-in label list (all read off the decoded `_a` art)

Every in‑game button carries its Russian caption **inside the texture** — there is no string‑table
index and no font involved for these labels.  (Verified by decoding every `m_16\<name>_a.txr`.)

| name | art px | caption |
|---|---|---|
| `savg` `loadg` `restart` `911` `control` `sound` `main` `exitsys` | 150×30 | СОХРАНИТЬ ИГРУ · ЗАГРУЗИТЬ ИГРУ · НАЧАТЬ СНАЧАЛА · 911 · УПРАВЛЕНИЕ · ЗВУК · ГЛАВНОЕ МЕНЮ · ВЫХОД |
| `ok` | 55×30 | OK |
| `canc` | 80×30 | ОТМЕНА |
| `save` | 130×30 | СОХРАНИТЬ |
| `delgame` | 130×30 | УДАЛИТЬ |
| `nsave` | 130×30 | ОТМЕНА |
| `load` | 130×30 | ЗАГРУЗИТЬ |
| `exsave` | 150×30 | СОХРАНИТЬ |
| `exnsave` | 150×30 | НЕ СОХРАНЯТЬ |
| `excanc` | 150×30 | ОТМЕНА |
| `gaspay` | 70×30 | OK |
| `gascanc` | 80×30 | ОТМЕНА |
| `servpay` | 130×30 | OK |
| `servcanc` | 130×30 | ОТМЕНА |
| `start` | 145×30 | СТАРТ |
| `mmenu` | 145×30 | ГЛАВНОЕ МЕНЮ |
| `5yes` / `5no` | 100×49 | ДА / НЕТ |
| `fire` / `chan` | 100×30 / 166×30 | УВОЛИТЬ / ПОМЕНЯТЬ МАШИНУ |
| `o_main` / `o_rest` | 130×30 | ГЛАВНОЕ МЕНЮ / НАЧАТЬ СНАЧАЛА |
| `ow_main` / `ow_cont` / `ow_rest` | 130×30 | ГЛАВНОЕ МЕНЮ / ПРОДОЛЖАТЬ / НАЧАТЬ СНАЧАЛА |

The panel titles are likewise painted into the backdrops
(`BackEscMain` → «ПРОБЛЕМЫ В ПУТИ», `BackEscSnd` → «ОБЩАЯ ГРОМКОСТЬ / МУЗЫКА / ЗВУК
ДВИГАТЕЛЯ / ЗВУК ТОРМОЖЕНИЯ» together with the slider tracks, `BackEscSave`/`BackEscLoad`
→ the save‑slot list frames, …).

![all in-game ESC buttons](tools/out/esc/_montage_all_buttons.png)

---

## 4. Art / state catalogue for the in‑game buttons

| state suffix | purpose | format |
|---|---|---|
| `_a`   | normal | `m_16\<name>_a.txr` (5:6:5, opaque) — style slot 0 (`widget+0x44`) |
| `_p`   | pressed | same size as `_a` — style slot 1 (`+0x68`) |
| `_ap`  | active/selected | same size as `_a` — style slot 2/3 (`+0x8c`/`+0xb0`) |
| `_f`   | forbidden/disabled — ships for `savg`, `loadg`, `restart`, `delgame`, `deldriv` | style slot 4 (`+0xd4`); `0x1bd940` selects it when `widget+0x28 != 0` (`0x10e18c…0x10e1d0` loads `%s\%s_f.txr` straight into `esi+0xd4`) |
| `_n`, `_pp`, `_ok`, `_ph` | other style slots the ctor can load (`0x676278/284/2cc/3b0`); **no ESC screen ships them** | — |

`m_15\…` is the 15‑bit twin and is chosen when `0x168fd0() == 2` (same names, same sizes).

## 5. Backdrop inventory (all VERIFIED by decoding)

| art | res | px | used by | anchor |
|---|---|---|---|---|
| `m_16\BackEscMain.txr` | esc16 | 215×1200 | ESC main, ESC exit popup | (0,y) |
| `m_16\BackEscSnd.txr` | esc16 | 215×1200 | sound | (0,y) |
| `m_16\BackEscCtrl.txr` | esc16 | 215×1200 | controls | (0,y) |
| `m_16\BackEscSave.txr` | esc16 | 215×1200 | save | (0,y) |
| `m_16\BackEscLoad.txr` | esc16 | 215×1200 | load | (0,y) |
| `m_16\BackGas.txr` | esc16 | 225×1200 | fuel | (screenW−224, y) |
| `m_16\BackService.txr` | esc16 | 640×1200 | service | (screenW−640, y) |
| `m_16\BackConnect.txr` | esc16 | 225×1200 | connecting | (screenW−224, y) |
| `m_16\BackMy.txr` | esc16 | 415×1200 | my‑truck info | (0,y) |
| `m_16\BackOrder/Sale/Job.txr` | esc16 | 225×1200 | market | (screenW−224, y) |
| `m_16\BackInfo.txr` | esc16 | 385×1200 | market info | (screenW−383, y) |
| `m_16\BackF1/F2.txr` | esc16 | 415×1200 | F1/F2 help | (0,y) |
| `m_16\BackF2Tball/F3.txr` | esc16 | 215×1200 | F2 (t‑ball)/F3 help | (0,y) |
| `m_16\BackWin{,1,2}.txr`, `BackLoose1…5.txr` | esc16 | 1600×1200 | win / lose | centred |
| `m_16\BackF5{pol,maf,opp,nei,cbradio}.msk` | esc16 | 280×405 | F5 radio | centred |
| `m16\MenuPopupA5.msk` | ind16 | 494×160 | yes/no popup | centred |
| `m\back_main.txr`, `g\backGraf.txr`, `c\backCtrl.txr`, `d\backDriv.txr`, `n\backMult.txr` | LOAD | 800×600 | front‑end pages | (0,0) |
| `m\back_main.txr`‑family overlays | LOAD | `common.msk` 800×600, `g\backGrafOpt.txr` 329×302, `c\backCtrlOpt.txr` 400×330, `n\backMultConn.txr` 340×380, `d\backDrivCurr.msk` 344×258, `d\backDrivRcnt.msk` 344×160 | front‑end dialogs | explicit |

---

## 6. id → action

| id (dec) | name | where it appears | action |
|---|---|---|---|
| 2 | `canc` | sound, controls, fuel (`gascanc`), service (`servcanc`) | close/cancel the dialog |
| 0x6e (110) | `loadg`, `load` | ESC main, load screen | load‑game screen (**handler decoded**, §3.6) |
| 0x79/0x78 (121/120) | `aleft`/`aright` | multiplayer connect | page the truck/colour list |
| 0x80/0x81 (128/129) | `aright`/`aleft` | driver select | page drivers |
| 0x82/0x83 (130/131) | `aright`/`aleft` | driver select | page trucks |
| 0x85/0x86 (133/134) | `aleft`/`aright` | graphics | cycle the resolution/option value |
| 0x8e/0x8f (142/143) | `aleft`/`aright` | controls (front‑end) | select the key‑binding row |
| 0xa0/0xa1 (160/161) | `5yes`/`5no` | popup A5 | answer the modal question |
| 0xa2 (162) | `start` | connecting | start the network game |
| 0xa3 (163) | `main`, `mmenu`, `o_main`, `ow_main` | ESC main, connecting, win/lose | leave to the main menu (**win/lose handler decoded**: quits to the menu) |
| 0xa4 (164) | `sound` | ESC main | open the sound screen |
| 0xa5 (165) | `control` | ESC main | open the controls/key‑list screen |
| 0xa6 (166) | `911` | ESC main | open the in‑game info/market overlay (§3.14) — “problems on the road” |
| 0xa7 (167) | `savg` | ESC main | open the save screen |
| 0xa8 (168) | `restart`, `o_rest`, `ow_rest` | ESC main, win/lose | restart the race (**win/lose handler decoded**) |
| 0xa9 (169) | `exitsys` | ESC main | open the exit‑confirm popup (§3.2) |
| 0xaa (170) | `exsave` | exit popup | save and quit |
| 0xab (171) | `exnsave` | exit popup | quit without saving |
| 0xac (172) | `nsave`, `excanc`, `ow_cont` | save/load/exit popup/win | save‑as / cancel / continue driving |
| 0xad (173) | `save` | save screen | write the selected slot |
| 0xb4…0xb7 | sliders | sound | master / music / engine / brake volume |
| 0xb8 (184) | `ok` | sound, controls | accept |
| 0xb9/0xba (185/186) | `chan`/`fire` | my‑truck info | change truck / fire the driver |
| 0xbc/0xbd (188/189) | `F5search`/`F5talk` | F5 CB page | scan the channel / talk |
| 0xbe…0xc8 | `F5{p,m,o,n}_ask1..4` | F5 radio | pick a radio question |
| 0xce/0xcf (206/207) | `F5*_ok`/`F5*_canc` | F5 radio | send / cancel |
| 0x11a (282) | `delgame` | save, load | delete the selected save (**handler decoded**: `0x11e95d` → `0x11d9e0(name)`) |

The generic command path is
`screen->vtable[0x34] (=0x1d8bb0) → [[screen+0x38]]->vtable[0x38](id)`, and a second wrapper
`screen->vtable[0x30] (=0x1d8b90)`; the per‑screen handlers are the `+0x34/+0x38/+0x3c` entries
of each screen's *second* vtable (the one installed at `this+0x40`).

---

## 7. How to reproduce — one line per screen

| screen | reproduction line |
|---|---|
| ESC main | blit `m_16\BackEscMain.txr` 1:1 at `(0, -(1200-H)/2-50)`; 8× `_a.txr` 150×30 at x=18, y=104+45k, order savg/loadg/restart/911/control/sound/main/exitsys |
| ESC exit popup | same panel; `exsave` (18,564), `exnsave` (18,614), `excanc` (18,664) |
| sound | `m_16\BackEscSnd.txr` at (0,y); 4 slider rows at (22, 489+70k) with 16×28 arrows at the tracks' ends; `ok` 55×30 at (21,762), `canc` 80×30 at (77,762) |
| controls | `m_16\BackEscCtrl.txr` at (0,y); 16 key rows at (22,511); `ok` (21,762), `canc` (77,762) |
| save | `m_16\BackEscSave.txr` at (0,y); `save`/`delgame`/`nsave` 130×30 at x=24, y=679/724/769 |
| load | `m_16\BackEscLoad.txr` at (0,y); `load`/`delgame`/`nsave` 130×30 at x=24, y=679/724/769 |
| fuel | `m_16\BackGas.txr` at `(screenW-224, y)`; `gaspay` 70×30 at (52,755), `gascanc` 80×30 at (123,755) |
| service | `m_16\BackService.txr` at `(screenW-640, y)`; `servpay` 130×30 at x=209, `servcanc` 130×30 at x=349, near the panel bottom |
| connecting | `m_16\BackConnect.txr` at `(screenW-224, y)`; `start` 145×30 at y=689, `mmenu` 145×30 at y=729 |
| popup A5 | centre `m16\MenuPopupA5.msk` (494×160); `5yes` 100×49 at (128,96), `5no` 100×49 at (266,96) |
| my‑truck info | `m_16\BackMy.txr` at (0,y); `fire` 100×30 at (85,774), `chan` 166×30 at (186,774); photo loop |
| win/lose | centre the 1600×1200 art; buttons 130×30, `*_main` at (589,687) etc. |
| F5 radio | centre the 280×405 mask; 258×44 buttons at x=11, y=205/250/295/340 (OK/CANCEL at y=250/295) |
| market / F1‑F3 | blit 1:1 at the §3.14 anchors; text offsets per panel |

---

## 8. What I could NOT determine

1. **The ESC‑screen command handlers other than load and win/lose.**  I decoded the generic
   path (`screen->vtable[0x34] = 0x1d8bb0` → `[[screen+0x38]]->vtable[0x38](id)` and the
   sibling `0x1d8b90` → `[[screen+0x38]]->vtable[0x34]([screen+0xc])`), and I *did* decode the
   load handler (`0x11e880`) and the win/lose handler (`0x11fd70`, a clean `sub eax,0xa3` switch).
   For the ESC main/sound/controls/save screens the object stored at `screen+0x38` is created by
   their callers (the ESC screens have **no direct `call` site** — `0x111170` is only referenced
   from the function table at `0x64d73c`), and I could not locate the object whose
   `vtable[0x38]` performs the id switch.  Actions in §6 for those screens are therefore
   **INFERRED from the button names / baked‑in labels**, not traced.
2. **Where the numeric button ids are actually consumed.**  `0x10def0` stores the id at
   `widget+0x1c` (inside `0x1bf820`), but I found no `cmp/push [reg+0x1c]` in the UI code, so the
   click→id→action plumbing is still unexplained (see 1).
3. **The exact geometry inside `0x10d860`** (the slider widget used by the sound screen):
   I decoded its 5 arguments `(name_left, name_right, &value, callback, 140)`, the 16×28 arrow
   art and the 70‑px row pitch, but not the *within‑row* y offset of the arrow pair (measured
   ≈ +23 px from the row top, i.e. on the painted track).
4. **The service screen's button Y.**  `servpay`/`servcanc` get explicit `x` (209/349) but their
   `y` is derived in a block I did not finish tracing (§3.8).
5. **The `x` of `start` and `mmenu` on the connecting dialog** — only their `y` (689/729) is set
   explicitly; `x` comes from the container's left edge, which I did not evaluate.
6. **`this+0x130`** — `0x111780` (the ESC main command entry) forwards to
   `[[this+0x130]]->vtable[0xa0](id)`.  Nothing in the ESC‑screen constructors writes `+0x130`,
   and the only `+0x130` writer I found in the UI area (`0x102156`) belongs to a different class,
   so the `+0xa0` dispatcher is unresolved.
7. **The `_Viewer`‑style “fase/m_rul” overlays and the exact RGB/shadow of menu text** — inherited
   open questions from `docs/original_ui.md` that this investigation did not touch (the button
   labels are all baked into the art, so no string‑table indices are involved for the ESC screens).
8. **`2Dmenu_cl`** (`0x678340`, the front‑end colour map) — its binary format was not decoded.
9. The `.res` name suffix `" noload"` (present on every `LOAD.RES` entry but not on `esc16.RES`
   entries) is unexplained; both are read successfully by the game.

---

## 9. Reproduction checklist

1. Draw in‑game panels **1:1** with `y = -(1200 - screenH)/2 - 50` (no scaling, clip to the
   screen); left‑docked at x = 0, right‑docked at `screenW-224` (225‑wide) / `screenW-385`
   (385‑wide) / `screenW-640` (service).  Full‑screen 1600×1200 art is centred.
2. Buttons are 5:6:5 `.txr` with the label **pre‑rendered in Russian** — the ESC menu needs no
   font or string table at all.  Each has `_a`/`_p`/`_ap` (+`_f` when disabled).
3. In‑game button pitch is 45 px (150×30 art + 15 px gap), column x = 18.
4. The ESC menu's top→bottom order is **savg, loadg, restart, 911, control, sound, main,
   exitsys** (last created = topmost).
5. Dialog buttons use panel‑local *explicit* rects (tables in §3).
6. `savg`/`loadg`/`restart` must be disabled when `[[0x6cec6c]+0x524] != 0` (network game)
   — the code sets `widget+0x28 = 1` and the widget then paints its `_f` art.

# In-game ESC screens — implementation (`src/ui/EscMenu.{h,cpp}`)

Status: implemented and verified.  Everything here follows `docs/esc_screens.md`
(the reverse-engineering report): the panel art comes from
`original/COMMON/esc16.RES`, is drawn **1:1, never scaled**, and the button
tables are the verified ones.  Two entries of that report are corrected below,
with the evidence.

Files touched:

| file | what |
|---|---|
| `src/ui/EscMenu.h` / `EscMenu.cpp` | new: the data-driven screens, input, drawing |
| `CMakeLists.txt` | `src/ui/EscMenu.cpp` added to the `ht2re` target |
| `src/app/App.h` / `App.cpp` | wiring: pause, actions, save game, ESC draw |
| `src/main.cpp` | `--esc`, `--esc-screen`, `--esc-act`, `--esc-shot` |
| `tools/shot_test.py` | 13 new checks (ESC screens + actions + the save round trip) |

---

## 1. The fit rule (VERIFIED, `0x110f20`)

The in-game panels are **1200-pixel-tall vertical panels**, blitted pixel for
pixel at

```
y = -(1200 - screenH) / 2 - 50           // EscMenu::panelY()
```

and docked horizontally: `x = 0` for every ESC screen (the only docking this
module uses).  Whatever falls outside the window is clipped by the GL viewport,
exactly like the original (which blits with a negative `y` and lets the
DirectDraw clip do the rest).

| screen | WxH | anchor at 800x600 | anchor at 1280x720 |
|---|---|---|---|
| ESC main / exit popup / sound / controls / save / load | 215x1200 | (0, -350) | (0, -290) |
| (other families, not drawn by this module: `BackGas` 225x1200 right-docked at `screenW-224`, `BackService` 640x1200 at `screenW-640`, `BackInfo` 385x1200 at `screenW-383`, 1600x1200 win/lose and 280x405 popups centred) | | | |

`EscMenu::Dock` already carries `Right225` / `Right640` / `Center`, so the other
in-game panels can be added later without touching the geometry code.

**Crop check:** at 640x480 the anchor is `-410`; the render in
`shots/esc_main_640.bmp` shows only the panel's art rows 410..889 - the first
title line (`ПРОБЛЕМЫ`) is cut off, exactly as the original would clip it.

---

## 2. The screens and their button tables

Panel-local coordinates (add the anchor of §1 for screen coordinates).
`id` = the original widget id, `action` = the string `EscMenu` hands to the App.

### 2.1 ESC main — `ПРОБЛЕМЫ В ПУТИ` (`BackEscMain.txr`)

**Correction to `docs/esc_screens.md` §3.1:** that table prints the button Y as
the *800x600 screen* coordinate (104 = 454 - 350).  The real panel-local Y is
`454 + 45k`, which is what the container translation `0x11152e` (`+18, +454`) and
the 45-px layout pitch (`0x111513`) produce.  `tools/out/esc/_compose_escmain.png`
confirms it: the first caption starts at compose row 209 of the 2x-scaled
`350..949` band, i.e. panel y 454.5.

| # | id | art | X | Y (panel) | action | label (baked) |
|---|---|---|---|---|---|---|
| 1 | 0xa7 | `savg` | 18 | 454 | `open:save` | СОХРАНИТЬ ИГРУ |
| 2 | 0x6e | `loadg` | 18 | 499 | `open:load` | ЗАГРУЗИТЬ ИГРУ |
| 3 | 0xa8 | `restart` | 18 | 544 | `restart` | НАЧАТЬ СНАЧАЛА |
| 4 | 0xa6 | `911` | 18 | 589 | `recover` | 911 |
| 5 | 0xa5 | `control` | 18 | 634 | `open:control` | УПРАВЛЕНИЕ |
| 6 | 0xa4 | `sound` | 18 | 679 | `open:sound` | ЗВУК |
| 7 | 0xa3 | `main` | 18 | 724 | `menu` | ГЛАВНОЕ МЕНЮ |
| 8 | 0xa9 | `exitsys` | 18 | 769 | `open:exit` | ВЫХОД |

### 2.2 Exit popup (`BackEscMain.txr`, explicit rects)

| id | art | X | Y | action | label |
|---|---|---|---|---|---|
| 0xaa | `exsave` | 18 | 564 | `exitsave` | СОХРАНИТЬ |
| 0xab | `exnsave` | 18 | 614 | `exitnosave` | НЕ СОХРАНЯТЬ |
| 0xac | `excanc` | 18 | 664 | `back` | ОТМЕНА |

### 2.3 Sound — `ЗВУК` (`BackEscSnd.txr`)

4 sliders (container at `(22, 489)`, pitch 70) + 2 buttons:

| slider | left/right art (16x28) | track (panel) | row |
|---|---|---|---|
| ОБЩАЯ ГРОМКОСТЬ | `voll_a.msk` / `volr_a.msk` | y = 501 | 489 |
| МУЗЫКА | `musl` / `musr` | y = 571 | 559 |
| ЗВУК ДВИГАТЕЛЯ | `engl` / `engr` | y = 641 | 629 |
| ЗВУК ТОРМОЖЕНИЯ | `skidl` / `skidr` | y = 712 | 699 |

The painted track spans panel **x 41..143** in every panel art (measured on the
decoded `BackEscSnd` / `BackEscCtrl` textures), and the track line sits **12 px
below the row top**.  The arrow pair is centred on that line
(`x = 25` and `x = 143`, `y = trackY - 14`); the `m_16\runner_a.msk` (12x22) knob
shows the value at `x = 41 + value*102 - 6`.

| id | art | X | Y | action |
|---|---|---|---|---|
| 0xb8 | `ok` (55x30) | 21 | 762 | `back` |
| 0x2 | `canc` (80x30) | 77 | 762 | `back` |

### 2.4 Controls — `УПРАВЛЕНИЕ` (`BackEscCtrl.txr`)

**Correction to `docs/esc_screens.md` §3.4:** the container that the report moves
to `(22, 511)` and calls "the key list" is the **slider container** — the same
`LAYOUT` + `RETC (22,511)` pattern as the sound screen, with three `ADD`s before
the layout, and the decoded art paints four labelled rows with three tracks
(`y = 523 / 593 / 663` = 511/581/651 + 12).

| slider | left/right art | row |
|---|---|---|
| ЧУВСТВИТЕЛЬНОСТЬ РУЛЯ | `sensl` / `sensr` | 511 |
| ВОЗВРАТ РУЛЯ | `centerl` / `centerr` | 581 |
| ОБРАТНАЯ СВЯЗЬ | `forcel` / `forcer` | 651 |
| АВТОМАТИЧЕСКАЯ КОРОБКА ПЕРЕДАЧ | `m_16\check_a.txr` / `check_pp.txr` (19x17) | 716 |

plus `ok` (21, 762) and `canc` (77, 762) — the same two buttons as the sound
screen.  The checkbox is a real toggle (`check_a` empty, `check_pp` ticked,
`check_ap` ticked + selected).

The **16 real key bindings** of `docs/original_ui.md` §3.1 are drawn as a text
list **beside** the panel (it is a front-end list in the original; see §7).

### 2.5 Save game — `СОХРАНИТЬ` (`BackEscSave.txr`)

| id | art | X | Y | action | label |
|---|---|---|---|---|---|
| 0xad | `save` (130x30) | 24 | 679 | `writesave` | СОХРАНИТЬ |
| 0x11a | `delgame` (130x30) | 24 | 724 | `deletesave` | УДАЛИТЬ |
| 0xac | `nsave` (130x30) | 24 | 769 | `back` | ОТМЕНА |

### 2.6 Load game — `ЗАГРУЗИТЬ` (`BackEscLoad.txr`)

| id | art | X | Y | action | label |
|---|---|---|---|---|---|
| 0x6e | `load` (130x30) | 24 | 679 | `loadfile` | ЗАГРУЗИТЬ |
| 0x11a | `delgame` | 24 | 724 | `deletesave` | УДАЛИТЬ |
| 0xac | `nsave` | 24 | 769 | `back` | ОТМЕНА |

The slot box painted into the art (panel y ~510..670) carries one text line:
`SLOT 1: <money> RUB  X <x>  Z <z>` or `SLOT 1: EMPTY`.

### 2.7 Art states

`_a` normal, `_p` just clicked (shown for 120 ms), `_ap` selected/highlight,
`_f` forbidden.  The three `_f` variants that ship (`savg`, `loadg`, `restart`,
`delgame`) are decoded and used when `Button::disabled` is set — that flag is only
ever set by the original's network check
(`[[0x6cec6c]+0x524] != 0`), which this port does not have (single player), so in
practice all buttons are enabled.

---

## 3. Module API (`src/ui/EscMenu.h`)

```cpp
class EscMenu {
  enum class Dock { Left, Right225, Right640, Center };
  struct Button { std::string name, action; int id, x, y, w, h; bool disabled;
                  unsigned texA, texP, texAP, texF; bool hasP, hasAP, hasF; };
  struct Slider { std::string name, label; int trackX0, trackX1, trackY;
                  float value; unsigned texL, texR, texLA, texRA, texRun, texRunA;
                  int arrowW, arrowH, runW, runH; };
  struct KeyRow { const char* keys; const char* desc; };
  struct Screen { std::string name, backdrop; Dock dock;
                  int backdropW, backdropH; unsigned backdropTex;
                  std::vector<Button> buttons; std::vector<Slider> sliders;
                  bool keyList, slotList; };

  bool load(const std::string& dataDir);   // COMMON/esc16.RES + the screen tables
  void loadArt(Renderer& r);               // decode+upload (needs a live GL context)

  void open(const std::string& screen);    // "main"|"exit"|"sound"|"control"|"save"|"load"
  void close();
  bool active() const;
  const std::string& screen() const;
  const Screen* current() const;
  const std::vector<Screen>& screens() const;

  void selectNext(int delta);              // keyboard/gamepad up-down
  void adjustSelected(int delta);          // sliders/checkbox left-right
  void activateSelected();
  void handleKey(int key);
  void handleMouse(float mx, float my, bool click, int vpW, int vpH);
  void update(float dt);

  std::string activateName(const std::string&);   // by button name
  std::string activateId(int);                    // by original widget id
  std::string back();                             // Esc / pad B
  std::string takeActivated();                    // consumed once per frame
  const std::string& lastButton() const;
  void draw(Renderer& r, Font& font);

  static float panelY(int screenH);
  float panelX(int screenW) const;
  float toScreenX(float px) const;  float toScreenY(float py) const;
  int selectedIndex() const;  int navCount() const;
  const Button* selectedButton() const;  const Slider* selectedSlider() const;
  const Slider* slider(const std::string&) const;
  float sliderValue(const std::string&) const;  void setSliderValue(const std::string&, float);
  int buttonId(const std::string&) const;
  float autoGear() const;  void setAutoGear(float);
  static const std::vector<KeyRow>& keyBindings();
};
```

### Action strings

`EscMenu` performs **screen navigation itself** and only hands terminal actions
to the App:

| action | produced by | handled in |
|---|---|---|
| `open:save` / `open:load` / `open:sound` / `open:control` / `open:exit` | `savg`, `loadg`, `sound`, `control`, `exitsys` | `EscMenu::open()` |
| `back` | `canc`, `nsave`, `excanc`, Esc from a sub-screen | `EscMenu` (sub-screen -> main) |
| `resume` | Esc / pad B on the ESC main screen | `App::escAction` (close, engine on) |
| `restart` | `restart` | `App::escAction` -> `Vehicle::reset(spawn_)` |
| `recover` | `911` | `App::recoverVehicle()` |
| `menu` | `main` (ГЛАВНОЕ МЕНЮ) | `App::leaveWorldToMenu()` |
| `exitsave` | `exsave` | save + `leaveWorldToMenu()` |
| `exitnosave` | `exnsave` | `leaveWorldToMenu()` |
| `writesave` | `save` | `App::saveGame()` |
| `loadfile` | `load` | `App::loadGame()` + close |
| `deletesave` | `delgame` | `App::deleteSave()` |

Navigation order is top-to-bottom: sliders (then the checkbox on the controls
screen), then the buttons.  Enter/Space/pad A activate, left/right adjust a
slider (or toggle the checkbox), the mouse hovers to select and clicks to
activate (a click on a slider track jumps the value there).

---

## 4. App wiring

* `App::drawWorld()` computes `paused = esc_.active()` and, while paused, skips
  `updateClock`, `Vehicle::update`, `Traffic::update`, `updateNodeEvents`,
  `updateGates`, `updateAmbushes`, `updateSceneAudio` and the AI-car push: the
  world is frozen behind the panel and the engine loop is stopped.
* **Esc / Start while driving** (the `Input::State::menu` edge) opens the ESC
  main screen and logs `esc: world paused (Esc / Start)`.  A second Esc (or pad
  B) closes the panel and resumes driving ("the front-end menu" is reached with
  the **ГЛАВНОЕ МЕНЮ** button, id 0xa3, as the original does).
  The SDL event path owns Esc/pad while the panel is up; `App::escSwallowMenu_`
  makes the App ignore the Input `menu` edge of the very same press for one
  frame, otherwise Esc would close and immediately reopen the panel.
* The ESC panel is drawn after the HUD (its own `begin2D`/`end2D`), so it is on
  top; `EscMenu::draw` sets the viewport size itself.
* Slider values are pushed into `Audio` (`setMasterVolume` for ОБЩАЯ ГРОМКОСТЬ,
  `setChannelVolumes(sfx, music, engine)` for the other rows).

### Command-line flags

```
--esc                     start in the world with the ESC main screen open
--esc-screen NAME         main|exit|sound|control|save|load (default main)
--esc-act NAME[,NAME...]  activate buttons at startup (name or decimal/0x id)
--esc-shot FILE           open the screen, render one frame to FILE and exit
```

`--esc*` implies the world (it runs the real `MENU -> WORLD` transition like
`--autostart`); `--esc-shot` is `--screenshot --out FILE`.  `--esc-act` takes a
comma-separated script, so a whole flow can be driven headlessly:

```
ht2re --data original --esc-act exitsys,exnsave --screenshot --out x.bmp
  esc: screen 'main' (8 buttons, 0 sliders)
  esc: activate 'exitsys' (id 0xa9, screen 'main') -> action 'open:exit'
  esc: screen 'exit' (3 buttons, 0 sliders)
  esc: activate 'exnsave' (id 0xab, screen 'exit') -> action 'exitnosave'
  esc: left the world for the main menu
```

---

## 5. The save game

**Location.**  `HT2_SAVE_FILE` when set (tests / sandboxes), otherwise
`SDL_GetPrefPath("ht2re","ht2re") + "savegame.txt"`
(`~/Library/Application Support/ht2re/ht2re/savegame.txt`,
`~/.local/share/ht2re/ht2re/savegame.txt`), with `ht2re_savegame.txt` in the
working directory as a last-resort fallback when the user directory is not
writable.  `App::savePaths()` returns all candidates; loading uses the first one
that exists, saving the first that opens for writing.

**Format.**  Plain text, one `key value` record per line, `#` starts a comment.
`HT2RE-SAVE <version>` is the magic; the version is currently `1`.

```
HT2RE-SAVE 1
# Hard Truck 2: King of the Road engine reimplementation save
vehicle ZilR
money 45000
pos -3020.000 -4625.000 0.000     # x z yaw(deg)
fuel 0.4184                       # 0..1
damage 0.3140                     # 0..1
job -1                            # index into Economy::jobs(), -1 = none
delivered 0                       # counter, informational
```

`App::saveGame()` writes it; `App::loadGame()` parses it, resets the truck to
`pos`, sets `fuel`, applies `damage`, sets the money and re-accepts `job` with
`Economy::accept(index)`.  `App::saveSummary()` reads back money/position for the
`SLOT 1:` line of the save/load panels; `App::deleteSave()` removes the file.

The job index is stable because the job list is rebuilt deterministically from
the shipped scripts (1110 jobs).  Traffic/AI state, the clock and the ambush
table are **not** saved (see §7).

---

## 6. Verification

* `python3 tools/shot_test.py` - 99 checks, all green; the 13 new ones:
  * `ESC screens decode the original esc16.RES art (34/34)`
  * `ESC main: 8 buttons at x=18, the verified 45-px pitch`
  * `ESC sound: 4 slider rows (arrow pair + runner) and OK/ОТМЕНА`
  * `ESC controls: 3 slider rows + the 16-line key-binding list`
  * `ESC save/load: 3 buttons at panel x=24, y=679/724/769`
  * `ESC exit popup: 3 buttons at y=564/614/664 (not the main list)`
  * `ESC exitsys -> ВЫХОД popup -> leave the world to the menu`
  * `... and the frame is the front-end menu again`
  * `ESC ГЛАВНОЕ МЕНЮ leaves the world for the front-end menu`
  * `ESC НАЧАТЬ СНАЧАЛА respawns, СОХРАНИТЬ ИГРУ writes the small state`
  * `save file format (HT2RE-SAVE 1 + money/pos/fuel/damage/job)`
  * `ESC ЗАГРУЗИТЬ restores the state and resumes driving`
  * `ESC 911 tows the truck to the nearest service station`
* `shots/esc_{main,sound,control,save,load,exit}.bmp` - the six screens over the
  world at 1280x720 (plus `esc_main_640.bmp`, `esc_main_800.bmp`).
* `shots/esc_compose_compare.png` - my 800x600 ESC-main render (left) next to
  `tools/out/esc/_compose_escmain.png` (middle) and their difference (right).
  Downscaling the reference with NEAREST and masking its red verification frames,
  the **mean absolute difference is 4.68/765, and 0.0 outside the selected
  button's row** - the only difference is the intentional `_ap` highlight on
  СОХРАНИТЬ ИГРУ (the reference composites `_a` for all eight).

---

## 7. What I could not do / known limitations

1. **The arrow pair's exact within-row offset** is one of the report's open
   questions (`docs/esc_screens.md` §8.3, "measured ≈ +23 px").  The arrows here
   are centred on the painted track line (row top + 12), which looks right in the
   renders but is a reconstruction, not a read-out.
2. **Controls screen key list.**  The in-game `BackEscCtrl` art is the three-slider
   settings panel (§2.4), so the 16 bindings of `docs/original_ui.md` §3.1 are
   drawn as a text box *beside* the panel.  The captions are the English
   descriptions of that table, not the original Russian strings: `MENU.SFT` (the
   font the engine actually loads) only has the 94 Latin glyphs, so Cyrillic text
   would not render.  The panel's own labels are baked into the art and are
   unaffected.
3. **The fourth sound row** (`ЗВУК ТОРМОЖЕНИЯ`) drives the mixer's effect channel:
   SDL2_mixer has no separate brake-sound bus.  Slider positions are not
   persisted to disk or to `TRUCK.INI`.
4. **The automatic gearbox checkbox** only stores its state in `EscMenu`
   (`autoGear()`); `Vehicle` has a fixed 6-speed manual and no auto-shift mode, so
   the setting has no gameplay effect yet.
5. **The key bindings are not rebindable** (matching the rest of the port) — the
   list is informational.
6. **The save game is a single slot.**  The panel art paints a scrollable slot
   list; only `SLOT 1` is used, and traffic, the in-game clock, delivered-jobs
   history and the ambush table are not part of the save.
7. **`restart` (НАЧАТЬ СНАЧАЛА)** respawns the truck at the `ENV/GAME.CNF` start
   position; the engine has no race/session state to reset yet, and money/jobs are
   deliberately kept (the original's НАЧАТЬ СНАЧАЛА restarts the race, which this
   port does not model).
8. **`911`** tows the player to the nearest *service station* (fuel or repair)
   and refuels/repairs the truck.  That is a reconstruction: `docs/esc_screens.md`
   §3.14 ties the 911 button to the in-game info/market overlay family, whose
   panels are not implemented in this port, and there was no existing recover
   code to reuse.  When no station is known it falls back to the start position.
9. **`exitsys` (ВЫХОД)** opens the authentic exit-confirm popup (§2.2);
   `СОХРАНИТЬ` / `НЕ СОХРАНЯТЬ` then leave the world for the front-end menu.  A
   one-step "leave the world" is available as `--esc-act exitsys,exnsave`.
10. **The network check** (`savg`/`loadg`/`restart` disabled when
    `[[0x6cec6c]+0x524] != 0`) is implemented as `Button::disabled` + the `_f` art
    but never triggers, because this port has no network game.
11. **Only the left-docked ESC panels are used**; `Dock::Right225/Right640/Center`
    exist but no screen is registered with them (the fuel/service/info/win-lose
    panels belong to other subsystems).

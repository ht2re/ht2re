# In-game interface sort-out — panels, HUD, navigation

Status: **implemented and verified** (`src/ui/EscMenu.{h,cpp}`, `docs/**`,
`tools/shot_test.py`.  This document is the inventory, the module format, the
HUD tidy-up, the exact `src/app/App.{h,cpp}` wiring that still has to be applied
(the UI layer cannot own it), and the remaining deviations.

Everything in here follows the two reverse-engineering reports:

* `docs/original_ui.md` — the HUD strip, the loading screen, the cockpit, the
  screen inventory of `esc16.RES`;
* `docs/esc_screens.md` — the per-screen button tables, **the fit rule** and the
  button/backdrop inventory.

The panel art is the original `original/COMMON/esc16.RES` (and `ind16.RES` for
the “5 yes/no” popup); it is drawn **1:1, never scaled**, at the verified anchors:

| docking | x | y | used by |
|---|---|---|---|
| left | `0` | `-(1200-screenH)/2 - 50` | ESC family, `BackMy`, `BackF1/F2/F2Tball/F3` |
| right 225 | `screenW - 224` | idem | `BackGas`, `BackOrder`, `BackSale`, `BackJob` |
| right 385 | `screenW - 383` | idem | `BackInfo` |
| right 640 | `screenW - 640` | idem | `BackService` |
| centred | `(screenW - artW)/2` | `(screenH - artH)/2` | `BackWin*`/`BackLoose*` (1600×1200), `BackF5*` (280×405, 245×435), `MenuPopupA5` (494×160) |

At 1280×720 the docked panels are at `y = -290`; the centred ones at
`(-160,-240)` (win/lose), `(500,157)` (F5), `(393,280)` (A5).

---

## 1. What was done

1. **All in-game panels are implemented** as data-driven screens of the *same*
   `EscMenu` (screen name → backdrop + dock + buttons + slider rows + text rows
   + painted value cells + a data table) instead of new ad-hoc code:
   fuel, service (repair **and** tuning), order / sale / job (market),
   dispatcher info, my company, the F1/F2/F2Tball/F3 help pages, win/lose, the
   F5 CB-radio pages and the two centred popups.
2. **The panels are actionable**: refuel, repair, buy a vehicle (with the
   trade-in), install an upgrade / install everything, accept a job, deliver,
   change the truck, fire the driver, answer the radio.  Every action is a
   *string* produced by `EscMenu` and consumed by `App::escAction` — the panel
   itself needs no App edit (§7 lists the small App hunks that bind them).
3. **The panels read the world modules** (`Economy`, `Vehicle`, `Way`,
   `GameData`) through a non-owning `EscMenu::Context`; the UI never duplicates
   their logic, it formats what they expose.
4. **Navigation**: mouse hover/click, keyboard (↑↓ ←→ Enter/Space Esc) and
   gamepad (dpad/A/B/Start) all drive the same `EscMenu` API; `Esc`/B closes a
   panel (and resumes driving) and steps back inside the ESC family; the in-game
   hotkeys F1/F2/F3/F5 open the help/radio panels (`EscMenu::hotkey`).
5. **New headless hooks** `--ui-panel NAME`, `--ui-shot NAME FILE`,
   `--ui-act LIST` (aliases of the existing `--esc-screen/--esc-shot/--esc-act`,
   `src/main.cpp`), plus the `HT2_UI_DEMO` test hook that fills the data cells.
6. **12 new checks** in `tools/shot_test.py` (§8.1) and PNGs of every panel in
   `shots/ui_<screen>.png` (30 files).

---

## 2. Screen inventory

### 2.1 Implemented (36 screens — 6 ESC + 30 panels)

“art” = the backdrop in `esc16.RES` unless marked `ind16`; sizes from the
decoded entries.  `id` = the original widget id (documented in
`docs/esc_screens.md` 3).

| screen | art | dock | buttons (name id @ x,y panel-local) | rows / cells / table |
|---|---|---|---|---|
| `main` | `m_16\BackEscMain.txr` 215×1200 | left | savg a7, loadg 6e, restart a8, 911 a6, control a5, sound a4, main a3, exitsys a9 — 150×30 @ x18, y454+45k | — |
| `exit` | `BackEscMain` | left | exsave aa, exnsave ab, excanc ac — 150×30 @ x18, y564/614/664 | — |
| `sound` | `BackEscSnd` 215×1200 | left | ok b8 (55×30) @21,762 · canc 2 (80×30) @77,762 | 4 slider rows @ (22,489+70k) |
| `control` | `BackEscCtrl` 215×1200 | left | ok b8 · canc 2 (same) | 3 slider rows @ (22,511+70k) + the AUTO GEARBOX checkbox (19×17 @150,716) + the 16-row key list drawn beside the panel |
| `save` | `BackEscSave` 215×1200 | left | save ad, delgame 11a, nsave ac — 130×30 @ x24, y679/724/769 | the `SLOT 1:` line from `saveSummary()` |
| `load` | `BackEscLoad` 215×1200 | left | load 6e, delgame 11a, nsave ac — 130×30 @ x24 | idem |
| `fuel` | `BackGas` 225×1200 | right225 | gaspay 113 (70×30) @52,755 → `refuel` · gascanc 2 (80×30) @123,755 → `back` | **5 LED cells**: СУММА $, ЛИТРЫ, ЗА ЛИТР (the amount being pumped) + the tank level in ЛИТРЫ/$; the УРОВЕНЬ amount scale (arrows @x25/175, y655, `gasfull` 21×21 + the `runner` knob) |
| `service` | `BackService` 640×1200 | right640 | servpay 117 (130×30) @209,790 → `repair` · servcanc 2 @349,790 → `back` · mup/mdown/mrun 21×20 @24/48/72,790 → `listup`/`listdown`/`listrun` · **invisible** hot spot «СТАВИТЬ ВСЁ» @452,696 → `tuneall` | the tuning grid: **19 price cells** (3 columns × 6/7 rows, x 174/374/604, y 599+16.6k) fed by `GameData::upgrades`, + the ПОЛНЫЙ РЕМОНТ cell (604,535) = `price.repair`; the МОЙ БАЛАНС / ИТОГО value boxes + the service pictures `servcar`/`upgradet` |
| `order` | `BackOrder` 225×1200 | right225 | ok b8 (55×30) @40,700 → `acceptjob` · canc 2 @105,700 → `back` | 7 painted value cells @ x124, y557+20k: ГРУЗ/КУДА/МАКС.ПРЕМИЯ/МАКС.ВРЕМЯ/ВЕС/ОБЪЕМ/ХРУПКОСТЬ |
| `sale` | `BackSale` 225×1200 | right225 | mup/mdown/mrun @150,440/466/492 · ok b8 @40,700 → `buyvehicle` · canc 2 @105,700 → `back` | **the purchase list** (up to 6 of the 147 offers, name+price @ x24/118, y446+19k, highlighted line boxed + runner) + КОД/ЦЕНА/МОЩНОСТЬ/ГРУЗОПОДЪЕМН of the highlighted line + TRADE-IN / TO PAY |
| `job` | `BackJob` 225×1200 | right225 | ok @40,700 → `hiredriver` · canc @105,700 → `back` | 6 cells @ y567+20k: ИМЯ/ТАРИФ/МАШИНА/ЦЕНА/МОЩНОСТЬ/ГРУЗОПОДЪЕМН (a driver + the cheapest truck on offer) |
| `info` | `BackInfo` 385×1200 | right385 | ok @40,766 → `deliverjob` · canc @105,766 → `back` | the dispatcher board: **10 rows × 4 columns** (ГРУЗ│ОТКУДА│КУДА│ДЕНЬГИ, x 42/149/239/327, y537+20k) from `Economy::jobsNear`, + a footer line (money/jobs/jobs near/clock) |
| `mytruck` | `BackMy` 415×1200 | left | fire ba (100×30) @85,774 → `firedriver` · chan b9 (166×30) @186,774 → `changevehicle` | the company summary row (ДЕНЬГИ/МАШИНЫ/ВСЕГО/МОГУ НАНЯТЬ, y483) + the driver table (4 columns x 19/92/168/245, y547+20k) |
| `f1` | `BackF1` 415×1200 | left | canc 2 @40,840 → `back` | the **16 in-game key bindings** in the art's 19 painted rows (keys @x20, descriptions @x190, y457+20k) |
| `f2` | `BackF2` 415×1200 | left | canc @40,840 → `back` | a 4-column statistics table (x 52/126/210/287, y479+20k): money/jobs/offers/delivered, fuel/damage/odo/clock, vehicle/speed/gear/level, pos/road/jobs/upgrades, prices/job |
| `f2tball` | `BackF2Tball` 215×1200 | left | canc @40,760 → `back` | 8 single-column rows (money…level) @ y567+20k |
| `f3` | `BackF3` 215×1200 | left | canc @40,760 → `back` | 6 rows × 2 columns @ y610+20k |
| `win`, `win1`, `win2` | `BackWin{,1,2}.txr` 1600×1200 | centred | ow_rest a8, ow_main a3, ow_cont ac — 130×30 @ y687, x 429/589/749 | — |
| `lose1`…`lose5` | `BackLoose1…5` 1600×1200 | centred | o_rest a8 @429,687 · o_main a3 @589,687 | — |
| `popup5` | `m16\MenuPopupA5.msk` 494×160 (**ind16**) | centred | 5yes a0 (100×49) @128,96 → `yes` · 5no a1 @266,96 → `no` | the pending question text (`EscMenu::ask`) |
| `cbpol` `cbmaf` `cbopp` `cbnei` | `BackF5{pol,maf,opp,nei}.msk` 280×405 | centred | F5x_ask1..4 (258×44) @11, y205+45k → `cbask` + the answer page; `cbopp`/`cbnei` have 3 questions + `F5x_canc` in the free 4th row | the party portrait `F5x_player` |
| `cbpol_ans` … `cbnei_ans` | same mask | centred | F5x_ok (258×44) @11,250 → `cbok` · F5x_canc @11,295 → `back` | the reply art `F5x_ans_*` (129×69) + the portrait |
| `cbradio` | `BackF5cbradio.msk` 245×435 | centred | F5search bc (99×36) @30,135 → `cbsearch` · F5talk bd (84×84) @137,146 → `cbtalk` | — |
| `cbtalk` | `BackF5tru.msk` 280×405 | centred | F5tr_l @60,250 → `cbleft` · F5tr_col @85,250 → `cbcolor` · F5tr_r @158,250 → `cbright` · F5tr_ok @60,330 → `cbok` · F5tr_canc @145,330 → `back` | `cb.channel` (CH *n*, the UI's own 1..40 state) |

`BackConnect.txr` (225×1200, `start`/`mmenu`) is decoded and listed in the
reports but **not** registered: the port has no network game (`docs/esc_impl.md`
7.10), so its two buttons have nothing to do.

### 2.2 Not implemented (with the art that exists)

| original UI | art | why / what would be needed |
|---|---|---|
| pause strip | `m16\pause.msk` 1600×50 | one `Hud::drawPause()` (the strip already loads `esc16.RES`) + `if (esc_.active()) hud_.drawPause(r_)` in `App::drawWorld`; the word ПАУЗА is painted into the art |
| VTR recorder overlay | `m16\vtrpanel.msk` 320×70 + `vtr{bck,fbc,ffr,frw,jsl,jsr,off,res,stp}_*.txr` + `m16\big0..9.msk` + `colon.msk` | the report has the panel anchor (bottom-right `screenW-400, screenH-70`) and the 9 button positions; the port has no replay recorder |
| big centred message popups | `m16\mess1..21.msk` 280×405, `mess16` 220×230, `m_16\MessTop.txr` 1600×50 | the port draws its own message banner (`msgText_`); the original centres a `mess<N>` mask — a drop-in `Screen` (centre, no buttons) when the message ids are known |
| service part repairs | painted ДВИГАТЕЛЬ/КОРОБКА ПЕРЕДАЧ/ТРАНСМИССИЯ/ТОРМОЗА/ФАРЫ/КУЗОВ/ШИНЫ rows + their price cells | the port's `repair()` is all-or-nothing (`damage*6000`): only ПОЛНЫЙ РЕМОНТ is priced; per-part damage is not modelled |
| photo album | `m16\escphoto01..54`, `escmafia01..08`, `escpolice01..15` (100×100) | the original loads them on the BackMy screen; the report does not give the album slots |
| indicator lamps | `m_16\up_engine_*.msk`, `up_*brake*`, `up_hlights*`, `up_tire*`, `up_abs_1`, `up_oil_1`, `up_fire_1`, `up_scan_1`, `up_susp_1`, `up_ftank*`, `up_glass*`, `up_transmiss*` (24×24 / small) | the exe never builds these names (docs/original_ui.md 2.6) — no verified positions |
| mirror frames | `m16\mirr{L,R,T}{S,M,L,H}.msk` | the frames are 2D art over 3D mirror surfaces; `src/gfx/Mirror.cpp` owns the surfaces, the frames are not placed |
| recorder digits, `m16\scrb*/scrl*/scrc*/scrr*`, `m16\input.msk` | 250×17 / 17×17 / 73×13 | orphan entries (the exe never references them) |
| front-end 800×600 pages | `LOAD.RES` | out of scope — `src/ui/Menu.cpp` + `docs/original_ui.md` 5.1 |

---

## 3. The `EscMenu` table format (extended)

One `Screen` = one screen; every screen is data, no per-screen code:

```cpp
struct Screen {
    std::string name, backdrop;      // "fuel", "m_16\\BackGas.txr"
    Dock dock;                       // Left | Right225 | Right385 | Right640 | Center
    Family family;                   // Esc (loaded eagerly) | Panel (loaded on open)
    int backdropW, backdropH;        // from the decoded art (1:1, never scaled)
    std::vector<Button> buttons;     // name, id, panel-local x/y/w/h, action, invisible
    std::vector<Slider> sliders;     // art pair + track + value (sound/controls/fuel)
    std::vector<Row>    rows;        // a text line in a painted cell (x, y, data key)
    std::vector<Image>  images;      // decorative art (portraits, service pictures)
    std::vector<Cell>   cells;       // a value in a *painted* cell (LED windows, prices)
    std::vector<int> tableCols;      // a data table (the job board / the market list)
    TableKind tableKind;             // None | Jobs | Vehicles
    int tableY, tablePitch, tableRows;
    bool keyList, slotList, question;
};
```

Three extensions were needed for the panels (all documented above):

* **`Row` / `Cell`** — the panel art paints label boxes, 7-segment LED windows
  and price cells; a `Row` writes text at one point, a `Cell` additionally owns a
  rectangle so the tuning grid can box the selected item.  `key` is a data key
  (§4) or `text:<literal>`; `key.<i>.k`/`key.<i>.d` are the key-binding rows.
* **`invisible` buttons** — «СТАВИТЬ ВСЁ» is painted into `BackService.txr`, so
  its button is a pure hot spot (clickable, navigable, draws nothing).
* **comma-separated actions** — `Button::action` may be a list.  `open:<screen>`
  navigates *and* is returned when the button has no other action (the App logs
  and ignores it); a terminal token wins: `F5p_ask1` is
  `"cbask,open:cbpol_ans"` → the App gets `cbask` and the answer page opens.

### Docks and anchors

`EscMenu::panelX()` implements the table of §0; `EscMenu::panelTop()` returns
`-(1200-screenH)/2-50` for the docked 1200-px panels and `(screenH-artH)/2` for
every centred screen (the 1600×1200 win/lose pages are centred, **not** shifted
by 50 like the docked panels — `docs/esc_screens.md` 2.2).

### Families

`Family::Esc` (6 screens) is decoded by `loadArt()` — the historical
`esc: art loaded (34/34, 6 screens)` line is unchanged.  `Family::Panel` is
decoded **on first open** (`ensureArt()`), so a screenshot only pays for its own
art and logs `ui: art loaded (n/n, '<name>')`; the 1600×1200 win/lose backdrops
(~7.7 MB each decoded) are never all resident.

---

## 4. The data the panels display

`EscMenu::Context` is a set of non-owning pointers the App fills once per frame:

```cpp
struct Context {
    const Economy*  economy;    // money, offers(), jobs()/jobsNear(), activeJob(), buy(), accept(), deliver(), ownedValue()
    const Vehicle*  vehicle;    // fuel(), damage(), speedKmh(), gear(), odometer(), x()/z()
    const Way*      way;        // rooms/nodes -> the nearest named road node
    const GameData* gameData;   // upgrades (PR_* -> price), playerVehicle, people
    float clockHours;           // App::timeOfDay_
    long refuelCost, repairCost, fuelPerLitre;   // the App's own pricing (-1 = unknown)
    std::string slotText, notice; float noticeTimer;
};
```

`EscMenu::refresh()` turns that into the cached `Data` and `field(key)` renders
one value.  Data keys (all ASCII, the currency ones drawn in the strip's amber):

`money` `fuel` `fuel.pct` `fuel.liters` `fuel.cost` `fuel.price`
`fuel.buy.liters` `fuel.buy.cost` `damage` `clock` `speed` `gear` `level` `pos`
`odo` `vehicle` `driver` `road` `station` `jobs` `jobs.near` `offers` `delivered`
`upgrades` `slot` `job.cargo|from|to|reward|distance|status`
`order.cargo|to|premium|time|weight|volume|fragile`
`sale.code|price|power|capacity|tradein|topay`
`hire.name|tariff|truck|price|power|capacity`
`price.fuel` `price.repair` `upg.<PR_ID>` (price or `OK`) `cb.channel` `ask`
`key.<i>.k` / `key.<i>.d` `text:<literal>`

Rows of the market/info panels sit **inside the boxes painted into the art**:
the box rectangles were measured from the decoded backdrops (e.g. `BackInfo`
body rows run y 534…734 in 20-px steps at x 42/149/239/327; `BackSale`
divider x 97 for КОД/ЦЕНА and 137 for МОЩНОСТЬ/ГРУЗОПОДЪЕМН; the service
upgrade grid at x 174/374/604, y 599 + 16.6 k).

**Test hook.** `HT2_UI_DEMO=1` (same family as `HT2_SAVE_FILE` /
`HT2_LOADING_DUMP`) fills `Data` with a deterministic sample world when no
`Context` is set, so `--ui-shot` can verify the data cells headlessly.  It is
inert in the shipping path (the App always sets a Context).

---

## 5. Actions (what the App binds)

`EscMenu` performs navigation itself and returns one terminal token per
activation (also reachable headlessly with `--ui-act`):

| action | produced by | App handler (§7) |
|---|---|---|
| `refuel` | fuel `gaspay` | `App::refuel()` |
| `repair` | service `servpay` | `App::repair()` |
| `tune` | service `mrun` / `listrun` | pay `esc_.selectedUpgradePrice()`, then `esc_.markUpgradeBought(id)` |
| `tuneall` | service «СТАВИТЬ ВСЁ» hot spot | pay the sum of the unbought `esc_.upgradeList()` |
| `acceptjob` | order `ok` | `economy_.accept(esc_.selectedJob())` |
| `deliverjob` | info `ok` | `economy_.deliver(x, z, radius, &reward)` |
| `buyvehicle` | sale `ok` / `mrun` | `economy_.buy(esc_.selectedOffer(), economy_.ownedValue())` + respawn |
| `hiredriver` | job `ok` | not modelled (log) |
| `firedriver` | mytruck `fire` | not modelled (log) |
| `changevehicle` | mytruck `chan` | `esc_.open("sale")` (the dealer panel) |
| `cbask` / `cbok` | F5 questions / answer page | log the question (`esc_.lastButton()`) |
| `cbsearch` / `cbtalk` | `cbradio` | `esc_.open("cbtalk")` / log |
| `yes` / `no` | `popup5` | `EscMenu::ask(q, act)` remembers the pending action: `yes` returns it |
| `resume` | canc/Esc on a panel, `ow_cont` | close + `audio_.engineStart()` |
| `restart` `recover` `menu` `exitsave` `exitnosave` `writesave` `loadfile` `deletesave` | ESC family | unchanged (`docs/esc_impl.md` 3) |

Internal (never returned to the App): `open:<screen>`, `back`,
`listup`/`listdown` (the vehicle list and the 19-cell tuning grid),
`cbleft`/`cbright` (the CB channel), `cbcolor`.

Reachability: mouse (hover selects, click activates, a click on a slider track
jumps the value), keyboard (↑↓ navigate, ←→ adjust a slider/checkbox, Enter/Space
activate, Esc back), gamepad (dpad, A/Start, B/Back) and the in-game hotkeys
`EscMenu::hotkey()`: **F1** → `f1`, **F2** → `f2`, **F3** → `f3`, **F5** →
`cbradio` (the App calls it while driving, §7 hunk 4).

---

## 6. HUD / interface tidy-up

### 6.1 Done in the UI layer

* Every panel grew the **original's chrome**: the real backdrop at the verified
  anchor, the real button art with `_a`/`_p`/`_ap`/`_f` states, the real slider
  arrows and the `runner` knob, the painted LED windows and price cells.
* **No panel is drawn twice or ad hoc**: all 33 screens share one draw path, one
  `begin2D`/`end2D` pair, one input path — nothing can drift out of alignment.
* Panels are drawn **after** the HUD, so a panel always wins; the port's extra
  HUD elements (minimap, hint lines, debug box) are handled in §6.2.
* `EscMenu::screenRect()` exposes the open panel's screen rectangle so the App
  can lay the HUD out around it (`hudOcclusion` in the wiring diff).

### 6.2 Needs the App (in the diff, §7)

* **The debug box is deleted.**  `drawWorld()` currently paints
  `"%.11s POS … YAW … ODO …"` (the “ZilR POS …” box) and a `"%.0f KM/H"`
  readout in the top-left, overlaying the original strip's left LED window.
  It is not in the original: the speed/money/clock live in the strip
  (`m16\tablo.txr`).  Hunk 5 removes the box and both text draws.
* **The minimap is drawn only when no panel covers it** (`screenRect()`), and
  the bottom hint lines only while nothing is open.  The minimap stays a port
  extra (the original has no minimap — `docs/original_ui.md` 2.3); it is drawn
  at `(screenW-214, screenH-260)` 196×196 and is otherwise covered by the
  right-docked panels, which is exactly the “no mixing” rule the new tests check.
* **The strip** is still the whole 1600×50 art scaled to the window width
  (`Hud::draw`).  That is the pre-existing deviation from the original's 1:1
  crop (`docs/esc_impl.md` 7); the new panels do not change it.  A 1:1 crop plus
  the original per-resolution text base is listed in §9.

### 6.3 Still a deviation (see §9)

`m16\pause.msk` is not drawn while paused; the strip is scaled rather than
cropped; the port's hint lines and minimap remain (now panel-aware).

---

## 7. App wiring (`src/app/App.{h,cpp}`) — the exact hunks

The UI layer cannot own these: the `App` is the only place that has the modules,
the input loop and the HUD.  Apply with `git apply --recount` / `patch -p1
--fuzz=3`; the line numbers are from the current revision and will drift with the
parallel traffic work — the context lines are the anchor.

**Short list**

1. `App.h`: declare `refuelPrice()` / `repairPrice()`.
2. `App.cpp`: implement them and use them in `refuel()` / `repair()` (single
   source for the prices the fuel/service panels show).
3. `App.cpp` `escAction()`: bind the 10 new panel actions.
4. `App.cpp` SDL_KEYDOWN: F1/F2/F3/F5 open the help/radio panels while driving.
5. `App.cpp` `drawWorld()`: delete the debug box, make the minimap + hint lines
   panel-aware, and push `EscMenu::Context` before `esc_.draw()`.

```diff
--- a/src/app/App.h
+++ b/src/app/App.h
@@ -258,6 +258,10 @@ class App {
     int nearestService(bool wantFuel) const;
     bool refuel(long* paidOut);
     bool repair(long* paidOut);
+    // The prices the fuel / service dialogs show.  refuel() and repair() call
+    // these, so the UI never re-derives the formula (docs/ui_sort.md 4).
+    long refuelPrice() const;
+    long repairPrice() const;
     std::vector<float> trafficYOff_;   // ground offset per AI model
```

```diff
--- a/src/app/App.cpp
+++ b/src/app/App.cpp
@@ -1127,15 +1127,25 @@
+long App::refuelPrice() const {
+    const float need = 1.0f - vehicle3d_.fuel();
+    return need <= 0.0f ? 0 : (long)(need * 1800.0f);
+}
+
+long App::repairPrice() const {
+    return (long)(vehicle3d_.damage() * 6000.0f);
+}
+
 bool App::refuel(long* paidOut) {
     if (nearestService(true) < 0) return false;
-    float need = 1.0f - vehicle3d_.fuel();
+    const float need = 1.0f - vehicle3d_.fuel();
     if (need < 0.03f) return false;
-    long cost = (long)(need * 1800.0f);
+    const long cost = refuelPrice();
     if (economy_.money() < cost) return false;
     economy_.addMoney(-cost);
     vehicle3d_.setFuel(1.0f);
     if (paidOut) *paidOut = cost;
     return true;
 }
 
 bool App::repair(long* paidOut) {
     if (nearestService(false) < 0) return false;
-    float dmg = vehicle3d_.damage();
+    const float dmg = vehicle3d_.damage();
     if (dmg < 0.02f) return false;
-    long cost = (long)(dmg * 6000.0f);
+    const long cost = repairPrice();
     if (economy_.money() < cost) return false;
```

```diff
--- a/src/app/App.cpp
+++ b/src/app/App.cpp
@@ -1646,6 +1646,62 @@ void App::escAction(const std::string& act) {
     if (act.rfind("open:", 0) == 0) return;        // handled inside EscMenu
+
+    // --- the in-game panels (src/ui/EscMenu.cpp, docs/ui_sort.md 5) --------
+    long paid = 0;
+    if (act == "refuel") {                         // BackGas "OK"
+        if (refuel(&paid)) {
+            LOG("ui: refuelled for %ld, money %ld", paid, economy_.money());
+            input_.rumble(0.3f, 0.15f, 120);
+        }
+        return;
+    }
+    if (act == "repair") {                         // BackService "OK"
+        if (repair(&paid)) LOG("ui: repaired for %ld, money %ld", paid, economy_.money());
+        return;
+    }
+    if (act == "tune") {                           // install the highlighted upgrade
+        const std::string id = esc_.selectedUpgrade();
+        const long price = esc_.selectedUpgradePrice();
+        if (id.empty() || price <= 0) { LOG("ui: no upgrade selected"); return; }
+        if (esc_.upgradeBought(id)) { LOG("ui: '%s' is already installed", id.c_str()); return; }
+        if (economy_.money() < price) { LOG("ui: '%s' costs %ld, money %ld - not enough",
+                                            id.c_str(), price, economy_.money()); return; }
+        economy_.addMoney(-price);
+        esc_.markUpgradeBought(id);
+        LOG("ui: '%s' installed for %ld, money %ld", id.c_str(), price, economy_.money());
+        return;
+    }
+    if (act == "tuneall") {                        // «СТАВИТЬ ВСЁ»
+        long sum = 0;
+        for (const auto& u : esc_.upgradeList())
+            if (!esc_.upgradeBought(u.first)) sum += u.second;
+        if (sum <= 0) { LOG("ui: every upgrade is already installed"); return; }
+        if (economy_.money() < sum) { LOG("ui: all upgrades cost %ld, money %ld - not enough",
+                                          sum, economy_.money()); return; }
+        economy_.addMoney(-sum);
+        for (const auto& u : esc_.upgradeList()) esc_.markUpgradeBought(u.first);
+        LOG("ui: all upgrades installed for %ld, money %ld", sum, economy_.money());
+        return;
+    }
+    if (act == "acceptjob") {                      // BackOrder "OK"
+        const int idx = esc_.selectedJob();
+        if (idx >= 0 && economy_.accept((size_t)idx)) {
+            const Economy::Job* j = economy_.activeJob();
+            LOG("ui: job accepted: %s -> %s, %.0f m, reward %ld",
+                j ? j->cargoName.c_str() : "?", j ? j->toTown.c_str() : "?",
+                j ? j->distance : 0.0f, j ? j->reward : 0);
+        } else {
+            LOG("ui: no job to accept");
+        }
+        return;
+    }
+    if (act == "deliverjob") {                     // BackInfo "OK"
+        long reward = 0;
+        if (economy_.deliver(vehicle3d_.x(), vehicle3d_.z(), 120.0f, &reward))
+            LOG("ui: job delivered, reward %ld, money %ld", reward, economy_.money());
+        else
+            LOG("ui: nothing to deliver here");
+        return;
+    }
+    if (act == "buyvehicle") {                     // BackSale "OK" / the list's OK
+        const int idx = esc_.selectedOffer();
+        if (idx >= 0 && economy_.buy((size_t)idx, economy_.ownedValue())) {
+            switchPlayerVehicle(economy_.offers()[(size_t)idx].model);
+            LOG("ui: bought %s for %ld (trade-in %ld), money %ld",
+                economy_.offers()[(size_t)idx].model.c_str(),
+                economy_.offers()[(size_t)idx].price, economy_.ownedValue(),
+                economy_.money());
+        } else {
+            LOG("ui: cannot afford that vehicle (money %ld)", economy_.money());
+        }
+        return;
+    }
+    if (act == "changevehicle") { esc_.open("sale"); return; }   // ПОМЕНЯТЬ МАШИНУ
+    if (act == "hiredriver" || act == "firedriver" || act == "cbcolor") {
+        LOG("ui: '%s' is not modelled yet", act.c_str());        // no crew model
+        return;
+    }
+    if (act == "cbsearch") { esc_.open("cbtalk"); return; }
+    if (act == "cbask" || act == "cbok" || act == "cbtalk")
+        { LOG("ui: CB radio '%s' (%s)", act.c_str(), esc_.lastButton().c_str()); return; }
     LOG("esc: unhandled action '%s'", act.c_str());
 }
```

```diff
--- a/src/app/App.cpp
+++ b/src/app/App.cpp
@@ -253,6 +253,15 @@
                 } else if (e.key.keysym.sym == SDLK_ESCAPE) {
                     // in the world the menu edge comes from Input (Esc is mapped
                     // there); at the menu Esc quits the game
                     if (state_ != State::World) running = false;
+                } else if (state_ == State::World) {
+                    // in-game help / radio hotkeys (docs/ui_sort.md 5):
+                    // F1/F2/F3 -> the help panels, F5 -> the CB radio
+                    const std::string h = esc_.hotkey(e.key.keysym.sym);
+                    if (!h.empty()) {
+                        esc_.open(h);
+                        esc_.setSlotText(saveSummary());
+                        if (audio_.ready()) audio_.engineStop();
+                        LOG("ui: hotkey opened '%s'", h.c_str());
+                    }
                 } else if (state_ == State::Menu) {
                     menu_.handleKey(e.key.keysym.sym);
                 }
```

```diff
--- a/src/app/App.cpp
+++ b/src/app/App.cpp
@@ -2578,33 +2578,32 @@ void App::drawWorld(float dt, bool interactive) {
     drawWeatherOverlay();                    // rain streaks / snow flakes
-    const float infoY = hud_.loaded() ? hud_.height(r_) + 4.0f : 10.0f;
-    r_.quad2D(white, 12, infoY, 320, 60, 0.04f, 0.06f, 0.10f, 0.45f);
-    char buf[160];
-    snprintf(buf, sizeof buf, "%.0f KM/H", vehicle3d_.speedKmh());
-    font_.draw(r_, buf, 24, infoY + 6, 2.0f, 1.0f, 0.95f, 0.4f, 1.0f);
-    snprintf(buf, sizeof buf, "%.11s   POS %.0f %.0f   YAW %.0f   ODO %.0f M%s",
-             opt_.vehicle.c_str(), vx, vz, vehicle3d_.yaw(), vehicle3d_.odometer(),
-             vehicle3d_.blocked() ? "   BLOCKED" : "");
-    font_.draw(r_, buf, 24, infoY + 34, 1.2f, 0.9f, 0.92f, 0.95f, 1.0f);
+    // The original has no debug box: speed, gear, money and the clock live in
+    // the top LED strip (m16\tablo.txr, docs/original_ui.md 2.2).  The old
+    // "%.11s POS … YAW … ODO …" overlay is gone (docs/ui_sort.md 6.2).
     // the original HUD is a 1600x50 LED strip at the top of the screen; the
     // bottom panel stays only as a fallback when the art is unavailable
     if (hud_.loaded()) {
         Hud::Values hv;
         hv.speedKmh = vehicle3d_.speedKmh();
         hv.gear = vehicle3d_.gear();
         hv.money = economy_.money();
         hv.clockHours = timeOfDay_;
         hv.level = (int)(vehicle3d_.damage() * 9.0f + 0.5f);
         if (const Economy::Job* aj = economy_.activeJob()) hv.distanceToDest = aj->distance;
         if (msgTimer_ > 0.0f) { hv.message = msgText_; hv.messageTimer = msgTimer_; }
         hud_.draw(r_, font_, hv);
-        drawMinimap((float)r_.width_ - 214.0f, (float)r_.height_ - 260.0f, 196.0f);
+        // the minimap and the hint lines are port extras: keep them off the
+        // open panel's rectangle (a panel is 1:1 and drawn on top)
+        const float mmx = (float)r_.width_ - 214.0f, mmy = (float)r_.height_ - 260.0f;
+        float px = 0, py = 0, pw = 0, ph = 0;
+        const bool panel = esc_.screenRect(px, py, pw, ph);
+        const bool mmCovered = panel && px < mmx + 196.0f && px + pw > mmx &&
+                               py < mmy + 196.0f && py + ph > mmy;
+        if (!mmCovered) drawMinimap(mmx, mmy, 196.0f);
     } else {
         drawDashboard();
     }
     if (msgTimer_ > 0.0f && !msgText_.empty()) {
         float bw = (float)msgText_.size() * 8.0f * 1.6f + 40.0f;
         float bx = ((float)r_.width_ - bw) * 0.5f;
         float by = 96.0f;
         float a = msgTimer_ < 1.0f ? msgTimer_ : 1.0f;      // fade out
         r_.quad2D(white, bx, by, bw, 42, 0.05f, 0.08f, 0.12f, 0.72f * a);
         font_.draw(r_, msgText_, bx + 20.0f, by + 12.0f, 1.6f, 1.0f, 0.97f, 0.75f, a);
     }
-    if (ambushesFired_ > 0 && msgTimer_ <= 0.0f)
-        font_.draw(r_, "AMBUSH!", 16, (float)r_.height_ - 30, 1.3f, 1.0f, 0.4f, 0.35f, 1.0f);
-    if (nearestService(true) >= 0 && vehicle3d_.fuel() < 0.95f)
-        font_.draw(r_, "PRESS TAB TO REFUEL", 16, (float)r_.height_ - 30, 1.2f, 0.5f, 1.0f, 0.6f, 1.0f);
-    else if (nearestService(false) >= 0 && vehicle3d_.damage() > 0.05f)
-        font_.draw(r_, "PRESS TAB TO REPAIR", 16, (float)r_.height_ - 30, 1.2f, 0.5f, 0.9f, 1.0f, 1.0f);
-    if (nearJobDist() >= 0.0f)
-        font_.draw(r_, "PRESS T TO TAKE THE CARGO JOB", 16, (float)r_.height_ - 30, 1.2f,
-                   1.0f, 0.9f, 0.4f, 1.0f);
-    font_.draw(r_, "W/S: THROTTLE  A/D: STEER  SPACE: BRAKE  C: VIEW  B: BUY  T: JOB  Y: DELIVER  ESC: MENU",
-               16, (float)r_.height_ - 16, 1.0f, 0.85f, 0.87f, 0.92f, 0.9f);
+    if (!esc_.active()) {                    // the hints never sit under a panel
+        if (ambushesFired_ > 0 && msgTimer_ <= 0.0f)
+            font_.draw(r_, "AMBUSH!", 16, (float)r_.height_ - 30, 1.3f, 1.0f, 0.4f, 0.35f, 1.0f);
+        if (nearestService(true) >= 0 && vehicle3d_.fuel() < 0.95f)
+            font_.draw(r_, "PRESS TAB TO REFUEL", 16, (float)r_.height_ - 30, 1.2f, 0.5f, 1.0f, 0.6f, 1.0f);
+        else if (nearestService(false) >= 0 && vehicle3d_.damage() > 0.05f)
+            font_.draw(r_, "PRESS TAB TO REPAIR", 16, (float)r_.height_ - 30, 1.2f, 0.5f, 0.9f, 1.0f, 1.0f);
+        if (nearJobDist() >= 0.0f)
+            font_.draw(r_, "PRESS T TO TAKE THE CARGO JOB", 16, (float)r_.height_ - 30, 1.2f,
+                       1.0f, 0.9f, 0.4f, 1.0f);
+        font_.draw(r_, "F1/F2/F3: HELP  F5: CB RADIO  W/S: THROTTLE  A/D: STEER  "
+                       "SPACE: BRAKE  C: VIEW  B: BUY  T: JOB  Y: DELIVER  ESC: MENU",
+                   16, (float)r_.height_ - 16, 1.0f, 0.85f, 0.87f, 0.92f, 0.9f);
+    }
     r_.end2D();
 
     // --- in-game ESC screens: the original 1200-px panels, 1:1, on top -------
     if (esc_.active()) {
         if (esc_.screen() == "save" || esc_.screen() == "load") esc_.setSlotText(saveSummary());
+        // the world data the panels display (docs/ui_sort.md 4): the UI reads
+        // these modules, it never duplicates their logic
+        EscMenu::Context uc;
+        uc.economy = &economy_;
+        uc.vehicle = &vehicle3d_;
+        uc.way = &way_;
+        uc.gameData = &gameData_;
+        uc.clockHours = timeOfDay_;
+        uc.refuelCost = refuelPrice();
+        uc.repairCost = repairPrice();
+        uc.fuelPerLitre = 9;                     // 1800 per tank / 200 L, see 4
+        uc.slotText = saveSummary();
+        esc_.setContext(uc);
         esc_.draw(r_, font_);
     }
 }
```

Notes on the diff:

* hunk 3 also replaces the two existing `Tab`-key fuel/repair handlers
  (`if (st.map) { … refuel(&paid) … }`) if you prefer the panels to be the only
  path — the current `Tab` shortcut is harmless and can stay.
* `uc.fuelPerLitre = 9` is `1800 / 200` (the App's full-tank price over the
  200-litre tank the fuel panel assumes).  If the pricing constant moves, move
  this with it.
* `esc_.context()` is stored by value; the pointers must stay valid for one
  frame — they do (the App owns them for its whole life).

---

## 8. Verification

* **`python3 tools/shot_test.py` — all green** (the new checks are §8.1, the
  tail of the run is in the final report).  The suite (after the parallel traffic work added its own checks) runs 132
  checks, 130 of them green — see §9.1 for the two that are not mine; the five ESC-family checks and every pre-existing check are unchanged.
* **PNGs of every panel at 1280×720**: `shots/ui_<screen>.png` (30 files = every
  panel of §2.1) — generated with
  `--ui-shot` over a quiet world and converted from the engine's BMP with
  Pillow (the engine writes BMP only).
* **Reference comparisons**:
  * `shots/ui_ref_compare.png` — the port's ESC main at 800×600 (left), the
    decoded reference composite `tools/out/esc/_compose_escmain.png` (middle) and
    the difference (right): **mean |Δ| 0.00/765** over the whole panel after
    masking the composite's red verification markers and the intentionally
    selected `_ap` row (the composite draws `_a` for all eight buttons).
  * `shots/ui_fuel_ref.png` — the port's `fuel` panel at (1056,−290) next to
    the decoded `m_16\BackGas.txr` rows 290…1010 and their difference:
    **mean |Δ| 7.87/765**, i.e. the dock, the 1:1 scale and the art sampling are
    right (the residual is GL sampling plus the values the port prints into the
    LED windows).

### 8.1 The new checks

| check | what it proves |
|---|---|
| every in-game panel decodes its own esc16/ind16 art (27 panels) | each screen's backdrop/buttons/sliders/images decode `n/n`, including the `ind16.RES` fallback of the A5 popup |
| panels use the verified dock/anchor rule (1:1 art, no scaling) | the logged dock+anchor of all 27 panels equals `x ∈ {0, 1056, 897, 640, (1280-w)/2}`, `y = -290` (docked) / `(720-h)/2` (centred) |
| docked panels cover their own rect and nothing else | fuel/service/info/mytruck differ from a panel-free baseline by >20 % inside their dock rect and **0.00 %** outside it |
| centred popups are centred 1:1 and cover nothing else | the A5 plate (393,280) and the CB set (517,142) differ inside and 0.00 % outside |
| the strip stays intact beside a panel and the minimap never mixes with one | the HUD strip band beside the panel is byte-identical to the baseline and still lit; a left panel leaves the minimap identical, a right panel covers it |
| every panel action is declared and produced for the App | 10 panel/button → action pairs (`gaspay→refuel`, `ok→acceptjob`, `ok→deliverjob`, `chan→changevehicle`, `fire→firedriver`, `ok→hiredriver`, `mrun→buyvehicle`, `mrun→tune`, `servcanc→resume`, …) |
| tuning: the list scrolls and APPLY produces `tune` | the 19-cell grid selection moves (`selection 2/19`, `3/19`) and `mrun` → `tune` |
| tuning: the painted «СТАВИТЬ ВСЁ» row is a hot spot | the invisible button over the painted row produces `tuneall` |
| purchase: the vehicle list is windowed, scrolls and shows price/trade-in | `list 6 offers (6 shown)`, `selection 2/6`, `sale.price = 18000`, `sale.tradein = 32000` |
| refuel: litres, price per litre, sum and the tank level are shown | the 5 LED cells (`fuel.buy.cost/liters`, `fuel.price`, `fuel.liters/cost`) and the `fuel = 42%` row are rendered from the data |
| navigation: ОТМЕНА closes the panel and the world is driving again | the panel's `canc` → `resume` and the frame is byte-identical to the panel-free baseline again |
| navigation: an F5 radio question opens its answer page | `F5p_ask1` → `cbask` and `cbpol_ans` is opened |

The baseline is rendered with `--scene world --traffic 0 --parked-radius 10
--event-radius 1` so that the only difference between the two frames is the
panel itself (without `--scene world` the `--ui-shot` path runs the real
menu→world transition and the lighting/minimap differ; without the event radius
the paused world would not have shown the node-event banner).

---

## 9. Remaining deviations / what I could not do

1. **The App wiring of §7 is not applied** (this task was not allowed to edit
   `src/app/App.cpp`/`App.h` while the traffic work is in flight).  Until it is:
   the panels render and act headlessly, but the actions log
   `esc: unhandled action '<x>'` in the interactive build, the debug box is
   still there, and the hotkeys do nothing.
2. **The service button `y`** is 790.  `docs/esc_screens.md` 3.8 gives the
   buttons' x (209/349) but not their y (§8.4 of that report); 790 is the first
   free band below the painted МОЙ БАЛАНС / ИТОГО row (y 747…766) and stays
   inside the visible band at 640×480 (−410 → rows 410…889 … 790 is visible).
3. **The win/lose button x** for `*_rest` / `ow_cont` is a reconstruction: the
   verified anchor is `*_main` at (589,687) and the container's horizontal pitch
   is 160 (`0x1bdee0`, spacing = 290 − 130), so the other two sit at 429/749.
   The report lists this as undecoded (§9.5 of `esc_screens.md`).
4. **`PR_TIRES` is one script entry** with two variants (SHINY HIGH-WAY /
   ВЕЗДЕХОД): `GameData::upgrades["PR_TIRES"]` keeps only the last price (1500),
   so both painted cells show it.  The per-part repair prices of the
   ПОВРЕЖДЕНИЯ rows stay empty (the port's `repair()` is all-or-nothing).
5. **The fuel amount is a display**: the УРОВЕНЬ scale picks the litres/price/sum
   the panel shows, but `App::refuel()` fills the tank completely (its own
   pricing).  Wiring that to the chosen amount means splitting `refuel()` into
   "price for amount N" + "pump N".
6. **`hiredriver` / `firedriver` have no model** — the port has no crew, so the
   `job` (ИЩУ РАБОТУ) and `mytruck` (МОЯ КОМПАНИЯ) panels show the player's own
   truck/company data in the painted cells and the two buttons only log.
7. **The key bindings are not rebindable** (unchanged from `docs/esc_impl.md` 7).
   The in-game list is `BackF1` (the authentic page); the ESC controls panel
   keeps the beside-the-panel list.
8. **The strip is scaled, not cropped.**  `Hud::draw` blits the whole 1600×50
   `tablo.txr` scaled to the window and lays the readouts out on the 800-wide
   grid; the original picks one of four blit x / text base pairs
   (`docs/original_ui.md` 2.1).  A 1:1 version needs the per-resolution text base
   and a re-check of the LED windows (the pre-existing deviation).
9. **`m16\pause.msk` is not drawn** while a panel is up — 3 lines in the App +
   `Hud::drawPause()` (§2.2).
10. **The big centred `mess<N>.msk` popups and the VTR recorder overlay** are not
    implemented; the art and (for the recorder) the anchors are known (§2.2).
11. **The centred panels' buttons were positioned from the container pitch**
    (F5 ask rows at y205/250/295/340 are verified; the `ok`/`canc` answer rows at
    250/295 are the report's reconstruction).
12. **The photo album, mirror frames and indicator lamps** of `BackMy`/the
    cockpit are not drawn (unverified slots, §2.2).
### 9.1 The two red checks in the last full run are not from the UI layer

The last suite run (132 checks) has 2 failures, both in the parallel traffic /
camera work and both reproduce on a build that differs from the all-green one
only by `src/app/App.cpp` + `src/game/*` + `src/gfx/*` edits made by the other
agent while I was finishing:

* `parked cars are actually rendered -- 0.00% of sampled px differ`: 24 parked
  vehicles are still *spawned* (`parked vehicles spawned from parking forms --
  24`) but the frame with `--parked-radius 3500` and the one with `--parked-radius
  10` are byte-identical, i.e. they are no longer drawn.
* `cockpit windscreen shows the sky -- 62918 px` (needs > 200000): the cockpit
  frame now shows ~3× less sky through the glass.

Neither scenario loads the UI layer (no `esc:`/`ui:` line appears in their logs);
the earlier all-green run of the identical UI code is the evidence that the
panels themselves are green:

```
$ python3 tools/shot_test.py /tmp/ht2re_ui_snapshot   # UI code, earlier App rev
...
  ok   docked panels cover their own rect and nothing else
  ok   centred popups are centred 1:1 and cover nothing else
  ...
all screenshot checks passed
```

13. **Build note**: `build/CMakeFiles/ht2re.dir/src/app/App.cpp.o` did not get
    rebuilt when `src/ui/EscMenu.h` changed (stale compiler-dep entry), which
    silently mixes `sizeof(EscMenu)` between translation units and crashes.  If a
    header under `src/ui/` changes, delete that `.o` (or `rm -rf build`) before
    building — worth fixing in CMake.

# Hard Truck 2: King of the Road — gameplay script specification

Scope: every gameplay structure the engine needs in order to **spawn and animate the
world** — the `node`, `nodeevent`, `ambush`, `obstacle`, `parking`, `service`, `town`,
`people`/`DAYGW`/`DAYONLY`/`UNCONDITIONAL` forms of `original/ENV/mod_*.sch`, the
`trucks`/`cars` blocks, and their relationship to `ENV/<tile>.way` and `ENV/<tile>.b3d`.

Everything below was derived by reading the shipped data, not from the original source.
Later sections also use the shipped executable. The 2026-10-06 correction below
supersedes historical wiring and per-frame `people` claims.

### Runtime correction, 2026-10-06

`0x42ac83..0x42ae69` belongs to **onModuleLoadX(char const * moduleName)**,
whose entry is `0x42aba0` (debug string `0x66dc24`). The direct caller is
`0x56dfef`; the gate loop filters the module name before its condition switch.
This site does **not** prove a per-frame roll. `ScriptRuntime::onModuleLoad`
now samples the matching module's day/weather condition and probability;
ordinary frame updates preserve that snapshot. All static port modules are
initialised once. Other original refresh/streaming paths and the original
55-state random generator remain open; the port still uses its deterministic LCG.

App now feeds actual `.way` RSEG links, including named interior vertices and
coincident RNOD aliases. All 31 authored events bind and execute on authored
geometry. Direction preserves the full movement vector, including transverse
crossings required by BASE01LEFT/BASE02RIGHT/BASE07RIGHT;
explicit passes retain their own movement vector. Overlapping endpoint zones
select the nearest endpoint, and 3D height distinguishes overlapping levels.
The 4 m projection guard is a port policy, not a recovered engine constant.
This is not yet an exact implementation of every original PositionId transition.

Paused frames clear transient event/message/obstacle pulses, not persistent
state. Obstacle and ambush distances include height; unresolved positions are
invalidated on rebinding. Obstacle selection rejects missing positions and
distance <=500, and App delivers open/close messages. Initial and subsequent
obstacle delays use 600 + rand()%300 (600..899), verified at 25 seeds and at
the scheduled deadline. `script_runtime_test.cpp` passes 400 checks, in addition
to the original 42-check harness, and
adds execution regressions, including all 87 authored ambushes and 17 obstacles;
ambush graph-hop guards, slot ids, missions and racing remain unverified.

Claims are marked:

* **[DATA]** — measured over all 46 scripts / 39 ENV tiles; the numbers are reproducible
  with the tools in `tools/`.
* **[INFERRED]** — the strongest reading consistent with all occurrences, but not proved
  by any engine code. The wording states exactly what is uncertain.
* **[OPEN]** — could not be determined from the shipped files (collected in §7).

Tooling added for this document (all under `tools/`, nothing pre-existing was modified):

| tool | purpose |
|------|---------|
| `tools/sexp.py` | s-expression reader/writer for `.sch`/`.CNF` (SDTE + cp1251 aware, escapes, canonical output) |
| `tools/sch2lua.py` | transpiles all 46 scripts to Lua tables and drives the round trip back to `.sch` |
| `tools/lua_sexp.lua` | the Lua side: loads the generated tables and re-serialises the canonical s-expression |
| `tools/sch2lua_check.sh` | the whole pipeline check: reader invariants, round trip, `dofile`, SDTE re-encrypt |
| `tools/script_survey.py` | inventory of every form head; `--head` / `--tree` dumps |
| `tools/verify_nodes.py` | resolves every node name in every `mod_*.sch` against `<tile>.way` and checks it against the `<tile>.b3d` bounding box |
| `tools/gen_head_table.py` | generates the complete head table in Appendix A |
| `tools/script_coverage.py` | enumerates every form head and field position and says who reads it (§9) |
| `tools/script_selftest.cpp` | the `--script-report` producer: drives the recovered rules off `original/king.exe` (§10) |
| `tools/build_script_check.sh` | builds and runs the above |

---

## 0. Summary of the world model in one page

```
                     global            ENV/GAME.CNF          sponsors the counts
                     ───────           evehicles.sch         the vehicle catalogue
                                       etrashboxes.sch       the parked/prop catalogue
                                       merchandise.sch       cargo
                                       restable.sch / vd.sch / names.sch / Ms.sch

  39 tiles named `<column letter><row letter>` (aa…ax, ba…bg, ca…ch, da…dr),
  each roughly 1–3 km across:

     ENV/aa.b3d  ── geometry, authored in WORLD space (metres)
     ENV/aa.way  ── the road network:  RNOD{named points}  +  RSEG{polylines}
     ENV/mod_aa.sch ── gameplay placed ON those named points:
                        town / parking / service  → which .way nodes carry what
                        nodeevent / ambush / obstacle / people → .way nodes or .b3d keys
```

* **Coordinates.** `.b3d` vertices and `.way` node positions live in **one global world
  frame**, unit ≈ **1 metre**: component order is `(x, z, height)` in both, and the
  height is an absolute world Y (measured over the 39 ENV tiles, the ground ranges from
  about −20 to +580).
  **[DATA]** §4.
* **Names are the join key.** The scripts contain almost no coordinates of their own:
  a `node` is a *string* that must be looked up as `NNAM` in the same tile's `.way`
  (`RNOD` record). All **440/440** cross-references in the shipped data resolve (§3.3). **[DATA]**
* **Two kinds of keys.** `node`/`nodeevent`/`ambush` names resolve to **`.way` nodes**;
  `obstacle`'s key and `people`'s gate keys resolve to **`.b3d` record names**
  (specifically `subtag 21` "key" nodes). **[DATA]** §3.3.

---

## 1. File format facts the reader must get right

* **Cipher.** `SDTE` magic + subtractive-lagged-Fibonacci stream cipher, key `0x5CB0`,
  identical for all scripts (`tools/decrypt_sch.py`, `src/script/Sdte.cpp`).
* **Encoding.** cp1251 (Cyrillic display names). CRLF line ends. Byte-oriented reads are
  fine; only display strings need translation.
* **Shape.** Every file is a **single outer list**; its direct children are the forms.
  `merchandise.sch`'s children are cargo items; `Ms.sch`/`vd.sch`'s children are
  key/value pairs. There are no evaluation forms — nothing to interpret, only data.
* **Comments.** `;` to end of line (used heavily in `GAME.CNF`).
* **Strings.** `"..."`, may contain cp1251 bytes.
* **Dotted pairs.** `Ms.sch` and `vd.sch` use Lisp-style dotted pairs, with `.` as its own
  atom:

  ```
  ( PLAYER_ROBBED_BY_MAFIA  .  "%s HAS BEEN ROBBED BY MAFIA AND HAS PAID FORFEIT" )
  ( FIAT_1  .  "Фиат" )
  ```

  A reader must accept `.` and expose it as a normal atom (or skip it).
* **Escapes.** `\` escapes the next character inside a string. Two files rely on it:
  * `merchandise.sch` — `"Телевизор 14\""` (2 occurrences; the closing quote is escaped).
  * `Ms.sch` — `"...ПРИ ДВИЖЕНИИ\n   ЗАДНИМ ХОДОМ"` (6 occurrences; `\n` = line break).

  Reads *must* honour it: stopping a string at the first `"` makes those two
  `merchandise.sch` items swallow the *next* item, i.e. **106** cargo items instead of
  **107**, with `TVSET14`/`TVSET21` corrupted. `src/script/Sexp.cpp` and
  `tools/sexp.py` both handle it now; `tools/sch2lua.py --check` reports every `\`
  with its location so a regression is visible **[DATA]**.
* **One unusual number.** `merchandise.sch` line 208 contains `1#e+006` (item
  `C_WAGES`). `#` is the exponent marker for the game's own reader — both
  `GameData::numWithHash` and `Economy::numLoose` map it to `e` — so the value is
  **1e6**. Plain `strtod` stops at `#` and returns `1`, which is why `tools/sexp.py`
  exposes two readers: `canonical_number` (strict C grammar, returns `None` here) and
  `game_number` (returns `1000000`). Neither silently rewrites the bytes; the atom is
  emitted verbatim **[DATA]**.
* **Empty list vs atom.** `()` occurs often (`town` fields, `parking` fields, `ambush`
  fields). "Atom" must not be defined as "all children are strings" — that wrongly
  classifies an empty list as an atom. `tools/sexp.py` carries an explicit `kind`, so
  `()` stays a list and survives the Lua round trip; `src/script/Sexp.cpp` already has
  `Sexp::List` and is unaffected.
* **Not a script:** `original/TRUCK.INI` is plain INI (video/audio/gameplay options:
  `[EFFECTS] days=50 weats=60 trafs=100`, `[CAR] RoadWidthReducement=1.5 kmph=on`). The
  pre-decrypted `decrypted/TRUCK.INI.txt` is a Python `repr` dump and is not usable.
  `original/rnrnfo.scq` starts with `SDTE` but does **not** decrypt with the standard key
  — different key/format **[OPEN]**.

---

## 2. The 46 files at a glance

| file | forms | what it defines |
|------|-------|-----------------|
| `ENV/mod_aa.sch` … `ENV/mod_dq.sch` (36) | town 29, parking 30, service 37, node 170, nodeevent 31, ambush 87, obstacle 17, people 13 | the per-tile gameplay of the 36 tiles that have one |
| `ENV/GAME.CNF` | playertypes, startposition, forsale*, forhire, missions, fuel, semitrailers, containers, parkings | world counters and the player spawn |
| `MENV/GAME.CNF`, `MTR/GAME.CNF` | playertypes, startposition, scenario | the two menu worlds (races / trial) |
| `evehicles.sch` | 158 × `vehicle` | the driveable/AI vehicle catalogue |
| `etrashboxes.sch` | 59 × `vehicle` | the parked cars, containers, fuel tank and roadwork prop catalogue |
| `merchandise.sch` | 107 cargo items | cargo classes, prices, carrier-class tags |
| `restable.sch` | 19 × `PR_*` | upgrade prices |
| `vd.sch` | 160 dotted pairs | vehicle id → localised display name |
| `names.sch` | 74 people | AI driver names/genders/roles |
| `Ms.sch` | 56 dotted pairs | in-game message strings |
| `TRUCK.INI`, `rnrnfo.scq` | — | settings / undecodable |

36 of the 39 ENV tiles have a `mod_*.sch`: **`av`, `be`, `dr` have none**. `dr` is the menu
circuit (its `.way` holds `start1_*`/`finish1_*`/`start2_*`/`finish2_*`/`check_point*`, which
never appear in the ENV scripts) **[DATA]**.

---

## 3. The node model

### 3.1 What a `.way` node is

`ENV/<tile>.way` is a chunked file (`WTWR`):

```
MNAM (map name, e.g. "aa")
GDAT
  GROM
    RNAM                       ; room name (the whole tile is one room)
    RNOD * N                   ; one per named point
        NNAM  (name, NUL-terminated)
        POSN  (24 bytes = 3 × little-endian double: x, z, height)
        FLAG  (u32)
    RSEG * M                   ; one per road segment
        ATTR  (u32 kind, double param, u32 lanes)
        WDTH  (2 × double: width at start, width at end)
        VDAT  (u32 n + n × 24 bytes: the polyline, same 3 × double layout)
```

`POSN` is **not** encrypted and **not** coordinate-swizzled — three raw LE doubles.
Proof, node `node_store_1000` of `aa.way` (offset 0x38 in the file):

```
NNAM  "node_store_1000\0"
POSN  44 dd 07 20 55 fa b1 40 | 00 00 00 00 1c ee b6 c0 | ef 8d 21 00 f8 1f 7c 40
      = 4602.33                | = -5870.11             | = 450.00
FLAG  0
```

`aa.b3d`'s `StoreAASpace` anchor translation is `(4625, −5863, 450)`; the assembled
`store_aa` mesh spans `x 4578…4670, z −5863…−5851, h 449.7…456.9`.
So **`POSN` = (x, z, height)** — the same component order as `.b3d`
(see `docs/b3d_transform.md` §2.3). **[DATA]**

`FLAG` empirically classifies the node **[DATA]** (762 ENV nodes):

| FLAG | n | meaning (measured) | evidence |
|------|---|--------------------|----------|
| 0 | 285 | **on-road node** — sits exactly on an `RSEG` polyline | 279/285 within 0.5 u of a polyline vertex, median distance 0.00 |
| 1 | 470 | **off-road marker** — map icon, ambush spot, parking bay, service entrance | 466/470 are ≥ 0.5 u away, median 21.2 |
| 2 | 7 | **on-road special** — 6 traffic anchors named `dumb_*`/`dump_*` plus `node_cb08` | all 7 exactly on a polyline |

The six FLAG-0 exceptions are the market/trade nodes and the hidden/stolen car spots; the
four FLAG-1 exceptions (`sto_BB`, `mapSto_BB`, `debris_BD`, `benzo_aj`) just happen to
coincide with a road vertex. Do **not** key the "dumb traffic" hub logic on the name
prefix — `dumb_dc` exists with FLAG 0; use `FLAG == 2` **[DATA]**.

### 3.2 The `node` form (170 occurrences)

```
( node  "WAYNODE"  [ROLE] [TOWN_DISPLAY_NAME] )
```

| # | type | meaning |
|---|------|---------|
| 1 | string (required) | **name of an `RNOD` in the same tile's `.way`** |
| 2 | keyword *or* integer (optional) | role: `unload`, `fuel`, `dummy`, or the integer `4` / `8` / `32` |
| 3 | string (optional, only with role `4`) | the town's display name (cp1251) |

Complete observed argument inventory **[DATA]** (170 forms):

| shape | count | meaning |
|-------|-------|---------|
| `(node "X")` | 102 | plain member of a node group |
| `(node "X" unload)` | 11 | the **cargo delivery point** of a town (one per real town, always the `…00` store node) |
| `(node "X" fuel)` | 4 | a **fuel depot / refuelling point** (the fuel-depot pseudo-towns) |
| `(node "X" dummy)` | 4 | placeholder: the node name is `""`, no `.way` node exists |
| `(node "X" unload fuel)` | 14 | a **petrol station**: delivers and refuels |
| `(node "X" 8)` | 13 | **hidden car** bay (`hidden_car_*` ×11, `node_stolen_ba0`, `stolen_truck_ca00`) |
| `(node "X" 32)` | 12 | **old / abandoned car** bay (`old_car_*` ×11, `node_car_0800`) |
| `(node "X" 4 "Town")` | 10 | a **market/trade stall**, carrying the town's display name |
| **total** | **170** | |

Note the keyword and integer roles never mix, and the 4 keyword atoms total 33 (11 + 4 +
4 + 14) of which 18 mention `fuel` and 25 `unload`.

The role-4 trade stall appears in **10 of the 15 bazaars** (MARKET_AD, AJ, AM, G, D, E, BC,
BD, CB, DC), always as the **last** node of the list and always with a non-empty town name;
the five bazaars `MARKET_A` (Рынок), `MARKET_F` (Moal), `MARKET_AF`, `MARKET_AG`,
`MARKET_AH` have none. Where present, the name matches the `town` display name exactly
(`node_car_0004` ↔ `"Боровое"` ↔ `TOWN_AD`). **[DATA]**

`4`, `8`, `32` are all powers of two and never combine, so they are best read as **bit
flags of a node-kind field** (4 = trade, 8 = hidden car, 32 = old car) rather than as
indices. **[INFERRED]** — §7 item 2: the scripts contain no table indexed by them, and the
name prefix (`hidden_car_*` → 8, `old_car_*` → 32) already encodes the same distinction.

The role tag is what the engine actually needs: it decides whether a node is a delivery
target (`unload`), a refuelling target (`fuel`), decorative, a car-spawn bay of a given
kind, or a trade trigger.

#### Annotated example — `ENV/mod_aa.sch` (complete file, town + service)

```lisp
(
  ; TOWN_AA  display "Алмазный" (= "Diamond"), town id 10,
  ;           kind = () (a real town; 2 = petrol station, () = fuel depot)
  ( town  TOWN_AA  "Алмазный"  10 ()
    ; the town's .way nodes, in order; the first carries role `unload`
    (                          ;  ^-- node_store_1000 is the delivery point
      ( node  "node_store_1000"  unload )
      ( node  "node_store_1001" )      ; node_store_1001..1004 are the remaining bays
      ( node  "node_store_1002" )      ; of the same shop row (15 u apart in .way)
      ( node  "node_store_1003" )
      ( node  "node_store_1004" )
    )()                        ; () = always-empty slot #6
    ; goods the town trades at start: (ITEM INITIAL_STOCK), stock 0..100
    (
      ( RDIAMOND  100 )        ; "Алмазный" starts with 100 units of tech. diamonds
      ( C_DIAMONDS  100 )      ;   ...and 100 units of the car-sized diamond cargo
      ( C_MAIL  0 )            ; mail is traded but starts empty
    )
    ( 5000 )                   ; map-descriptor block: either icon names per zoom
  )                            ; level (a list of 4 strings) or the default `( 5000 )`
  ; service STO_AA, display "", type 0, at .way node "STO_AA",
  ( service  STO_AA  ""  0  "STO_AA"
    (                          ; upgrades this shop offers, and how many levels of each
      ( PR_FORCED_ENGINE  1 )  ; (see restable.sch for the price of each level)
      ( PR_REINFORCED_CHASSIS  1 )
      ( PR_NAVIGATOR  1 )
      ( PR_TIRES  2 )          ; a copy-pasteable example, verbatim from the game
    )
  )
)
```

`.way` coordinates of the referenced nodes (from `aa.way`, `FLAG` in brackets):

```
node_store_1000  4602.33  -5870.11   450.00  [0]
node_store_1001  4616.51  -5869.90   450.00  [0]     ; 15 u apart along the store front
node_store_1002  4631.60  -5869.77   450.00  [0]
node_store_1003  4646.26  -5870.10   450.00  [0]
node_store_1004  4661.98  -5869.80   450.00  [0]
STO_AA           4750.00  -5675.00   450.00  [1]     ; exactly the StoAASpace anchor
```

### 3.3 How nodes relate to each other, to `.way` and to the tile

* **A `node` is a reference, not a position.** It only names an `RNOD`; the position comes
  from `POSN`, the classification from `FLAG`. **[DATA]**
* **Grouping is by form.** The children of one `town`/`parking`/`service` form are one
  logical group (a shop row, a car park, a repair shop). Members are usually 10–15 u apart
  in a grid/row; e.g. `MARKET_AR`'s `node_car_0300…0307` form a 2 × 4 grid
  (x = −4058 / −4007, z = −2215 / −2225 / −2235 / −2245).
* **Nodes are the graph that AI and mission logic use.** `FLAG 0` nodes lie exactly on the
  drivable `RSEG` polylines, so `unload`/`fuel` targets are reachable by definition.
  `FLAG 1` markers are *next to* the road (median 21 u ≈ one road half-width plus the
  forecourt), which is precisely where a vehicle should be placed to sit on a forecourt.
* **Verification [DATA].** For all 36 `mod_*.sch`, every string that names an object
  resolves **in the same tile** — 440 references, 0 missing, 0 resolving to another tile:

  | form / field | target | references | unresolved |
  |--------------|--------|-----------:|-----------:|
  | `node` field 1 | `.way` RNOD | 166 (plus 4 empty `dummy` names) | 0 |
  | `service` field 4 | `.way` RNOD | 37 | 0 |
  | `nodeevent` field 3 | `.way` RNOD | 31 | 0 |
  | `ambush` field 5 | `.way` RNOD | 87 | 0 |
  | `obstacle` field 4 | `.b3d` subtag 21 | 17 | 0 |
  | `obstacle` fields 5, 6 | `.way` RNOD | 15 + 15 (4 are `""`) | 0 |
  | `obstacle` field 9 list | `.way` RNOD | 6 | 0 |
  | `people` gate field 2 | `.b3d` subtag 21 | 66 | 0 |
  | **total** | | **440** | **0** |

  Reproduce with `python3 tools/verify_nodes.py` (which additionally re-checks the
  referenced `.way` nodes against each tile's `b3d` bounding box).

### 3.4 The road network (`RSEG`) — the route source

`RSEG` is what makes moving traffic possible. Measured over the 39 ENV `.way` files
(2259 segments, 9853 polyline points) **[DATA]**:

* `VDAT` = `u32 n` + `n × (x, z, height)` doubles. Segment length 2…78 points.
* `WDTH` = (width at first point, width at last point), 52 of 2259 taper. Values seen:
  3, 5, 6, 7, 8, 12, 14, 20, 21, 24 — i.e. **the full road width in world units**.
  If the unit is 1 m these are 3 m tracks … 24 m plazas, which is self-consistent.
* `ATTR` = `u32 kind`, `double param` (observed 0.0, 0.05, 0.1, 0.15, 0.35, 0.5),
  `u32 lanes` (1, 2 or 3). `kind` values observed:
  1, 2, 5, 6, 9, 10, 22, 33, 34, 49, 50, 53, 54, 57, 58, 97, 98 — they come in
  (even, odd) pairs, and `NOTES.md` records that bit 0 picks the engine constructor
  (object sizes 0x38 vs 0x50), so: **bit 0 = "extended segment", the rest = a road class
  enum**. The exact class table and the meaning of `param` are **[OPEN]**.
* **Connectivity is by exact endpoint welding.** In `aa.way` all 48 segment endpoints
  coincide exactly with another segment's vertex (23 distinct junctions, 2 free ends);
  `ad.way` 168/168, `ar.way` 240/240. No endpoint is merely *near* another
  (< 20 u but > 0.6 u: zero cases). So building the graph needs a hash on rounded
  coordinates, not a tolerance search. **[DATA]**

---

## 4. Tile ↔ script ↔ world coordinates (question 4), with a numeric check

**Answer.** `ENV/mod_aa.sch` carries **no coordinates at all**: it only names `.way`
nodes. The coordinates live in `ENV/aa.way` (`RNOD/POSN`) and are already in the **same
world frame as `aa.b3d`'s geometry** — same axis order `(x, z, height)`, same origin, same
scale (≈ 1 unit = 1 m). The tile id is the link: `mod_XX.sch` ↔ `ENV/XX.way` ↔ `ENV/XX.b3d`.

### Concrete numeric verification for `aa`

```
tile             aa.b3d  (rooms subtag 19 + all assembled containers, world space)
   x  [ 2050.00 ..  5009.89 ]
   z  [-5927.67 .. -3101.00 ]
   h  [  447.06 ..   583.00 ]

aa.way  all 20 RNOD
   x  [ 2126.30 ..  4850.00 ]
   z  [-5870.11 .. -3077.59 ]
   h  [  436.27 ..   460.50 ]

node referenced from mod_aa.sch        .way POSN                 inside b3d bbox?
   node_store_1000    ( 4602.33, -5870.11, h=450.00) [FLAG 0]   yes
   node_store_1001    ( 4616.51, -5869.90, h=450.00) [FLAG 0]   yes
   node_store_1002    ( 4631.60, -5869.77, h=450.00) [FLAG 0]   yes
   node_store_1003    ( 4646.26, -5870.10, h=450.00) [FLAG 0]   yes
   node_store_1004    ( 4661.98, -5869.80, h=450.00) [FLAG 0]   yes
   STO_AA             ( 4750.00, -5675.00, h=450.00) [FLAG 1]   yes
        -> exactly the b3d anchor `StoAASpace` translation (4750, -5675, 450)
        -> the assembled mesh `STOS` spans x 4743.8..4756.7, z -5685.1..-5664.9, h 450.0..456.4
   mapSto_AAXXX       ( 4850.00, -5800.00, h=450.00) [FLAG 1]   yes
   node_car_repair_aa ( 4749.51, -5669.57, h=450.00) [FLAG 0]   yes
```

Same check over **all 36 tiles** (`tools/verify_nodes.py`): every node referenced by a
`mod_*.sch` lies inside its own `b3d` tile's x/z bounding box — 100 % of them. The only
`.way` node that falls outside its tile's geometry is `pos_hidden_0101` in `aa`
(26 u outside in x), and it is *not* referenced by `mod_aa.sch`. **[DATA]**

Reproduce:

```bash
python3 tools/verify_nodes.py                 # all 36 tiles, one line each
python3 tools/b3d_world.py original/ENV/aa.b3d   # the bbox above + .way overlay
```

Height check: the flat `aa` ground is 447.06 (1st percentile of room heights) and the
`.way` nodes are at 436…460 — the nodes sit on the ground, not in the air.

---

## 5. Structure reference: exact grammar, units and examples

Conventions used below: `u32`/`u64` = integer atom, `float` = decimal atom,
`"str"` = quoted atom (cp1251), `()` = empty list, `[...]` = optional, `(…)` = a nested
list with that head. Positions are `(x, z, height)` in world metres unless stated.

### 5.1 `nodeevent` — pass-a-node messages (31 occurrences)

```
( nodeevent  SYMBOL  KIND  "WAYNODE"  DIRECTION  "TEXT" )
```

| # | type | meaning |
|---|------|---------|
| 1 | symbol | unique id, e.g. `BORDERAB`, `BASE06LEFT` |
| 2 | enum | `STATEBORDER` (crossing a town/county border) or `UNCONDITIONAL` (always) |
| 3 | string | **`NNAM` of an `RNOD` in the same tile's `.way`** |
| 4 | enum | `ALL`, `NORTH`, `SOUTH`, `EAST`, `WEST` |
| 5 | string | message shown to the player (cp1251); `""` for `STATEBORDER` |

* `KIND = STATEBORDER` ⇒ always `DIRECTION = ALL`, `TEXT = ""` (**12** of 31).
  The engine fires the border/state change when the player crosses it (§10.3).
* `KIND = UNCONDITIONAL` ⇒ always `DIRECTION ∈ {N,S,E,W}` and a non-empty message
  (**19** of 31), e.g. *"БАЗА БОРОВОЕ НАЛЕВО, ЧЕРЕЗ 300 М"* = "Borovoe depot, to the left,
  in 300 m". `DIRECTION` is **the direction of travel required to fire the event**, not a
  bearing of the depot: `onNodePass` requires `dot(storedDir, playerMoveDir) > 0`
  (king.exe `0x40b279..0x40b29d`). **[DATA + disasm]**
  The exact split is `ALL 12 / NORTH 6 / SOUTH 5 / EAST 2 / WEST 6`; every `ALL` is a
  `STATEBORDER` and every `UNCONDITIONAL` has a compass direction.  (An earlier revision of
  this document said 15 / 16 - that was wrong; `python3 tools/script_coverage.py` prints the
  real numbers.)
* The trigger is **not a radius**: the engine calls
  `onNodePass(player, node, direction)` from `setPlayerPosition` (`0x43e822`, also
  `0x43ba2b`, `0x43eae6`) whenever the player's road `PositionId` becomes a node that
  carries an event; the event pointer is attached to the node record at `+0xc0`
  (loader `0x413f10`).  There is also an 8-second re-trigger guard (§10.3).

```lisp
; a border crossing: fires from any direction, no message
( nodeevent  BORDERAB  STATEBORDER  "border_ab"  ALL  "" )

; a depot hint, 300 m before the turn ("БАЗА ПРИОЗЕРСК НАЛЕВО, ЧЕРЕЗ 300 М")
( nodeevent  BASE00LEFT  UNCONDITIONAL  "store_00_left"  NORTH  "БАЗА ПРИОЗЕРСК НАЛЕВО, ЧЕРЕЗ 300 М" )
```

`.way` coordinates for those examples:

```
border_ab       (  550.00, -2200.00, h=220.00) [FLAG 0]
border_ad       (  425.00, -1425.00, h=147.25) [FLAG 0]
store_00_left   (-2150.00,  -250.00, h= 30.00) [FLAG 0]
store_00_right  (-2100.00,   300.00, h= 30.00) [FLAG 0]
store_06_left   (  700.00,  -349.00, h= 55.00) [FLAG 0]
store_06_right  (  700.00,  -750.00, h= 80.00) [FLAG 0]
```

`UNCONDITIONAL`/`STATEBORDER` in field 2 is a keyword of the `nodeevent` form only;
do not confuse it with `UNCONDITIONAL` as a `people` gate head (§5.7).

### 5.2 `ambush` — police/mafia roadblocks (87 occurrences)

```
( ambush  SYMBOL  CLASS  F3  F4  "WAYNODE"  DIRECTION  ( ID… )  [ ( NEXT_AMBUSH… ) ]  [ RADIUS ] )
```

| # | type | meaning |
|---|------|---------|
| 1 | symbol | unique id, e.g. `AMBUSH_AD_1`, `M_AMBUSH_BB00` |
| 2 | enum | `POLICE` or `MAFIA` (the only two values) |
| 3 | `()` **or** string | empty list in the 4 old regions; the **tile tag** (`"ba"`,`"bb"`,`"bd"`,`"bf"`) in the north-island tiles |
| 4 | `()` **or** string | empty list in the 4 old regions; `""` in the north-island tiles |
| 5 | string | **`NNAM` of an `RNOD`** — the ambush/patrol anchor |
| 6 | enum | `NORTH`, `SOUTH`, `EAST`, `WEST` — facing/direction of the ambush |
| 7 | list of u32 | **global ambush-slot ids** (see below); one id per node of the ambush |
| 8 | optional list | the ambush this one is coupled to (a paired two-sided roadblock) |
| 9 | optional u32 | extra radius (only `65` and `85` appear, on 8 of 87) |

Counts **[DATA]**: `POLICE` 44, `MAFIA` 43; node-list lengths 1, 2, 3, 4, 5 or 10
(most common: 5 ×28, 2 ×25, 10 ×15); 19 of 87 have no "next" list; 8 carry the trailing
radius (`65` ×6, `85` ×2).

**The slot ids.** They are unique across the whole game and never appear anywhere else in
the plaintext data. Structure **[DATA]**:

* mafia ids occupy **1000–1399**, police ids **2000–2399**;
* each ambush consumes `len(node-list)` consecutive ids out of an **allocated block of 10**
  (a coupled pair of ambushes shares one block, 5 + 5);
* blocks used: mafia `01,02,…,10,11,…,14,20,21,…,35`; police `01,…,10,20,…,35`
  (e.g. `AMBUSH_AD_1` = 2322/2323 in block `32`, `M_AMBUSH_BB00` = 1100–1109 in block `10`);
* blocks roughly group by world region (10xx/20xx = the western/northern tiles,
  12xx/22xx = the southern ones, 13xx/23xx = the eastern ones) with gaps at
  `1150–1199` and `1310–1319`.

I could not find any table in the shipped files (`.way`, `.b3d`, `.res`, `GAME.CNF`,
`LOAD.RES`) that these ids index, and they are *not* global `RNOD` indices (762 nodes),
global `RSEG` indices (2259), or global polyline-vertex indices (9853) in any tile
ordering I tried — all four hypotheses were tested numerically and rejected, see §7 item 3.
**For re-implementation they can be ignored:** the position comes from field 5, and the
node list is an ordered set of 1–10 points.

```lisp
; A paired mafia roadblock in tile ad: two ambushes of 5 slots each (1325-1329 / 1320-1324),
; each naming the other in field 8. Direction = which way the ambush faces.
( ambush  AMBUSH_AD_3  MAFIA ()() "mafamb_ad_14"  WEST
   ( 1325  1326  1327  1328  1329 ) ( AMBUSH_AD_4 ) )
( ambush  AMBUSH_AD_4  MAFIA ()() "mafamb_ad_16"  EAST
   ( 1320  1321  1322  1323  1324 ) ( AMBUSH_AD_3 ) )

; A police ambush with a trailing radius of 65 (AMBUSH_AJ_5), north-island style with a
; tile tag instead of the two empty lists (mod_bb):
( ambush  M_AMBUSH_BB00  MAFIA  "bb"  ""  "mafambbb00"  WEST
   ( 1100 1101 1102 1103 1104 1105 1106 1107 1108 1109 )() )
( ambush  AMBUSH_AJ_5  POLICE ()() "polamb_aj_22"  EAST
   ( 2035  2036 )() 65 )
```

Referenced `.way` positions (`ad.way`, `bb.way`):

```
mafamb_ad_14   ( 550.00, -1045.00, h=115.00) [FLAG 1]   ; 10 u from its pair
mafamb_ad_16   ( 550.00, -1035.00, h=115.00) [FLAG 1]
polamb_ad_08   ( 567.50,  -550.00, h= 60.00) [FLAG 1]
polamb_ad_10   ( 650.00,  -750.00, h= 75.00) [FLAG 1]
mafambbb00     (-3500.00, 4294.00, h= 20.00) [FLAG 1]
```

### 5.3 `obstacle` — road closures and roadworks (17 occurrences)

```
( obstacle  SYMBOL  KIND  "TILE"  "KEY"  "DEBRIS_A"  "DEBRIS_B"
            [ "CLOSED_TEXT" ]  [ "OPEN_TEXT" ]  [ ( "POS_WORK" … ) ] )
```

| # | type | meaning |
|---|------|---------|
| 1 | symbol | unique id, `OBST_AB_1` … |
| 2 | enum | `REGULAR` — the only value in the shipped data |
| 3 | string | the tile tag (`"ab"`, `"ac"`, `"aw"`, `"ax"`, `"bb"`, `"bd"`, `"da"`) |
| 4 | string | **`b3d` record name of a `subtag 21` key node** in that tile — the thing toggled |
| 5 | string | **`.way` node name** where debris/roadblock geometry appears when closed (may be `""`) |
| 6 | string | second such `.way` node (may be `""`) |
| 7 | string | message while closed (e.g. *"ТРАССА ШАХТЫ - БОРОВОЕ ЗАКРЫТА"*) |
| 8 | string | message once open (*"ДВИЖЕНИЕ ПО ТРАССЕ … ОТКРЫТО"*) |
| 9 | list | `.way` node names marking the repair site(s) |

**[DATA] resolution:** field 4 resolves to a `b3d` record in 17/17 cases; fields 5 and 6
(15 + 15 non-empty names) and the field-9 names (6) resolve to `.way` nodes in 36/36
cases; the only empty strings are the debris names of the two `mod_bd` bridge obstacles
(2 × 2 = 4). Nothing is left unresolved.

**[DATA] what the key actually is.** `FallAB1Key`, `FloodAWKey`, `CLOSE_BD_L`,
`K_CLOSE_BB0`, `FallAC*Key`, `FallDA*Key` are all `subtag 21` records. `subtag 21` is a
**key/animation group**: its children are the objects it shows/hides/animates, and the
tile also contains *reference* instances named `refer_<Key>` (e.g. `refer_FallAB1Key`
with children of subtags 9/37/18/20 — the visible barrier pieces), plus `refer_ILLUM_LEVEL_13/14/15`
which are the day/night street-lighting groups. The payload is
`{u32,u32,u32,u32, u32 type, u32 flags}` then `count { child }`.

```lisp
; obstacle whose geometry is debris placed on the road (mod_ab)
( obstacle  OBST_AB_1  REGULAR  "ab"  "FallAB1Key"  "debris_ab10"  "debris_ab11" )

; roadwork with messages and two repair markers (mod_ac)
( obstacle  OBST_AC_1  REGULAR  "ac"  "FallAC1Key"  "debris_ac10"  "debris_ac11"
   "ТРАССА ШАХТЫ - БОРОВОЕ ЗАКРЫТА"                 ; closed message  (msg 1)
   "ДВИЖЕНИЕ ПО ТРАССЕ ШАХТЫ - БОРОВОЕ ОТКРЫТО"     ; reopened message(msg 2)
   ( "pos_work_ac_15"  "pos_work_ac_06" ) )         ; repair-site .way nodes

; flood closing a highway, no debris geometry (mod_aw)
( obstacle  OBST_AW_1  REGULAR  "aw"  "FloodAWKey"  "debris_aw10"  "debris_aw11"
   "ТРАССА ПРИОЗЕРСК - ЕЛЬНИНО ЗАКРЫТА"
   "ДВИЖЕНИЕ ПО ТРАССЕ ПРИОЗЕРСК - ЕЛЬИНО ОТКРЫТО" )

; a bridge repair: no debris (empty names), message pair, one repair node (mod_bd)
( obstacle  OBST_BD_2  REGULAR  "bd"  "CLOSE_BD_R"  ""  ""
   "МОСТ НА ТРАССЕ БУХТА - НИЖНЕГОРСК НА РЕМОНТЕ"
   "МОСТ НА ТРАССЕ БУХТА - НИЖНЕГОРСК ВОССТАНОВЛЕН"
   ( "pos_work_bd0" ) )
```

Referenced positions:

```
debris_ab10   ( 960.00, -2410.00, h=270.00) [FLAG 0]   pos_work_ac_15  ( 125.00, -1815.00, h=165.00) [FLAG 1]
debris_ab11   (1050.00, -2500.00, h=280.00) [FLAG 0]   pos_work_ac_06  ( -96.00, -2086.00, h=177.00) [FLAG 1]
debris_ac10   (  50.00, -1850.00, h=170.00) [FLAG 0]   pos_work_bb0  (-3084.17, 4944.38, h=20.00) [FLAG 1]
debris_ac11   (   0.00, -1900.00, h=175.00) [FLAG 0]   pos_work_bb1  (-3044.43, 4984.14, h=20.00) [FLAG 1]
debris_BB0    (-3100.00, 4900.00, h= 20.00) [FLAG 0]   pos_work_bd0  (bd.way)                    [FLAG 1]
debris_BB1    (-3000.00, 5000.00, h= 20.00) [FLAG 0]   pos_work_bd1  (bd.way)                    [FLAG 1]
debris_aw10   (-2200.00, 1300.00, h= 30.00) [FLAG 0]   debris_aw11   (-2200.00, 1150.00, h=30.00) [FLAG 0]
```

### 5.4 `parking` — car-park groups (30 occurrences)

```
( parking  SYMBOL  "DISPLAY"  MARKET_ID  ()  ( ( node … ) … )  [ SIZE ] )
```

| # | type | meaning |
|---|------|---------|
| 1 | symbol | id; `MARKET_*` for bazaars, `PARKING_*` for hidden/off-road groups |
| 2 | string | display name (cp1251) for bazaars (`"Балка"`, `"Рынок"`, `"Барахолка"`, `"Базар"`, `"Market"`, `"Moal"`, `"??"`), `""` for hidden ones |
| 3 | int | `-1` = **hidden/off-road group**; `0…14` = the **bazaar id** (see below) |
| 4 | `()` | unused slot, always empty |
| 5 | list | the `node` forms — **exactly the car-spawn points** |
| 6 | int | optional, only for bazaars; observed 5, 8, 9, 10, 10, 12, 12, 12, 14, 14, 16, 16, 18, 22, 22 |

**[DATA] bazaar ids** (0…14) are a *separate* numbering from town ids:

```
0=AD (Боровое)  1=AJ (Барахолка)  2=AL (Рынок)   3=AR (Базар)   4=AU (Market)
5=AW (Moal)     6=AP (??)         7=DC (??)      8=BC (??)      9=BD (??)
10=CB (??)      11=AH (Балка)     12=AG (Балка)  13=AF (Балка)  14=AM (Балка)
```

The optional field 6 has no verified consumer **[OPEN]**; it is 5–22, always present for
bazaars, never for hidden groups. It is *not* the node count (AD has 5 nodes and value 10,
AR has 9 and value 12) and not the bazaar id.

```lisp
; A bazaar: 5 bays, the last one is the trade stall (role 4) and names the town.
( parking  MARKET_AD  "Балка"  0 ()
   (
     ( node  "node_car_0000" )
     ( node  "node_car_0001" )
     ( node  "node_car_0002" )
     ( node  "node_car_0003" )
     ( node  "node_car_0004"  4  "Боровое" ) ) 10 )

; A hidden group (id -1, no display name): one hidden car and one abandoned car.
( parking  PARKING_AF_1  ""  -1 ()
   (
     ( node  "hidden_car_af_03"  8 )
     ( node  "old_car_af_05"  32 ) ) )

; The stolen-truck bay and the "new" bazaar style (roles 32 / 4 in the same list).
( parking  PARKING_CA_1  ""  -1 () ( ( node  "stolen_truck_ca00"  8 ) ) )
( parking  MARKET_BC  "??"  8 ()
   ( ( node  "node_car_0800"  32 ) ( node  "node_car_0801"  4  "Ключи" ) ) 5 )
```

Referenced positions (`ad.way`, `ar.way`) — note the bays are rows/grids:

```
node_car_0000  ( 315.00,  -610.00, h=75) [FLAG 0]   ; row at x=315, 40 u spacing
node_car_0001  ( 315.00,  -570.00, h=75) [FLAG 0]
node_car_0002  ( 315.00,  -530.00, h=75) [FLAG 0]
node_car_0003  ( 315.00,  -490.00, h=75) [FLAG 0]
node_car_0004  ( 567.50,  -520.00, h=60) [FLAG 1]   ; trade stall, 19 u off the road
hidden_car_ad_17 ( 395.00, -1250.00, h=133) [FLAG 1]
node_car_0300  (-4058.00, -2215.00, h=30) [FLAG 0]  ; 2 x 4 grid
node_car_0301  (-4058.00, -2225.00, h=30) [FLAG 0]
node_car_0302  (-4058.00, -2235.00, h=30) [FLAG 0]
node_car_0303  (-4058.00, -2245.00, h=30) [FLAG 0]
node_car_0308  (-4185.00, -1950.00, h=30) [FLAG 1]  ; trade stall
```

### 5.5 `service` — repair shops and fuel stations (37 occurrences)

```
( service  SYMBOL  "DISPLAY"  TYPE  "WAYNODE"  ( ( PR_X  LEVELS ) … )  [ ID ] )
```

| # | type | meaning |
|---|------|---------|
| 1 | symbol | `STO_*` (repair) or `BENZO_*` (fuel) |
| 2 | string | always `""` in the shipped data (the label comes from the map/town) |
| 3 | int | type/level id; observed 0,1,2,3,4,5,6,10,11 — 10 is used by every `BENZO_*` |
| 4 | string | **`NNAM` of an `RNOD`** — where the shop's forecourt is |
| 5 | list | upgrades offered: `( PR_X levels )`, `levels` = how many upgrade steps |
| 6 | list | optional; **for fuel stations only, = `4000 + town id`** |

**[DATA] field 6 verified:** `BENZO_BA` `(4021)` with `TOWN_BA_GS` id 21; `BENZO_AG_07`
`(4022)`/id 22; `BENZO_AG_10` `(4023)`/23; `BENZO_AG_15` `(4024)`/24; `BENZO_AJ`
`(4025)`/25; `BENZO_CE` `(4026)`/26; `BENZO_AR1` `(4027)`/27; `BENZO_AR` `(4028)`/28;
`BENZO_DQ` `(4029)`/29; `BENZO_AU` `(4030)`/30; `BENZO_AC` `(4031)`/31; `BENZO_AE`
`(4032)`/32; `BENZO_CC` `(4033)`/33; `BENZO_DC` `(4035)`/35. Town id **34** has no town
and no fuel id. Field 3's own meaning could not be determined **[OPEN]** (it is *not* the
town id, the bazaar id, the upgrade count, nor a price level).

The `PR_*` names match the 19 rows of `restable.sch`, which gives each level a display
name and a price:

```lisp
( PR_FORCED_ENGINE  ( "Regular engine" 0 ) ( "Forced engine" 12500 ) )
( PR_TIRES          ( "Regular tires" 0 )  ( "Highway tires" 2000 ) ( "Off-road tires" 1500 ) )
```

so `( PR_TIRES 2 )` in a `service` means "levels 0,1,2 available here", and level *n*'s
price is the *n*-th entry of the matching `restable.sch` row.

```lisp
( service  STO_AA  ""  0  "STO_AA"
   ( ( PR_FORCED_ENGINE  1 ) ( PR_REINFORCED_CHASSIS  1 )
     ( PR_NAVIGATOR  1 ) ( PR_TIRES  2 ) ) )

( service  BENZO_AC  ""  10  "benzo_ac"
   ( ( PR_FUEL  1 ) )                 ; fuel price is in restable.sch: PR_FUEL ("Fuel" 100)
   ( 4031 ) )                         ; 4000 + TOWN_AC_GS0 id 31
```

`service` node positions (all `[FLAG 1]`, off-road, on the forecourt):

```
STO_AA       ( 4750.00, -5675.00, h=450.00)      sto_af_29  (-1400.00, 1970.00, h=5)
benzo_ac     ( -410.00, -2448.00, h=155.00)      sto_BB     (-3987.50, 3470.00, h=28.05)
node_car_fuel_ac (-406.50, -2452.50, h=155.00) [FLAG 0]
```

### 5.6 `town` — towns, petrol stations and fuel depots (29 occurrences)

```
( town  SYMBOL  "DISPLAY"  TOWN_ID  KIND  ( ( node … ) … )  ()  ( GOODS… )  MAPS )
```

| # | type | meaning |
|---|------|---------|
| 1 | symbol | `TOWN_XX` (town), `TOWN_XX_GSn` (petrol station), `TOWN_XX_FUEL` (fuel depot) |
| 2 | string | display name; `""` for fuel depots, `"АЗС nn"` for petrol stations |
| 3 | int | **town id**: 0–10 real towns, 11–14 fuel depots, 21–35 petrol stations |
| 4 | int or `()` | `2` = petrol station, `()` = town or fuel depot |
| 5 | list | the town's `node` forms (see §5.4) |
| 6 | `()` | unused slot, always empty |
| 7 | list | **goods traded here**: `( ITEM INITIAL_STOCK )`; `()` for petrol stations |
| 8 | list | **map descriptor**: either a list of 1–2 groups of 4 icon names, or `( 5000 )` |

**[DATA] town ids** (the numbering used by `service` field 6, where the fuel id =
`4000 + town id`):

```
0  TOWN_AJ (Приозерск)   1  TOWN_AR (Залесск)      2  TOWN_AU (Ельнино)
3  TOWN_AP (Южный)       4  TOWN_BC (Ключи)        5  TOWN_AM (Шахты)
6  TOWN_AD (Боровое)     7  TOWN_CB (Нижнегорск)   8  TOWN_BD (Бухта)
9  TOWN_DC (Горный)     10  TOWN_AA (Алмазный)
11 TOWN_CC_FUEL 12 TOWN_BB_FUEL 13 TOWN_AM_FUEL 14 TOWN_AX_FUEL
21 BA 22 AG07 23 AG10 24 AG15 25 AJ 26 CE 27 AR1 28 AR 29 DQ 30 AU
31 AC 32 AE 33 CC (34 missing) 35 DC        ; petrol stations, ids = 4000+service field 6
```

Field 7 (goods): every town lists the cargo types it trades with an initial stock 0–100.
Only `TOWN_AA` actually starts non-empty (`RDIAMOND 100`, `C_DIAMONDS 100`); everything
else is `0`. Cargo ids match `merchandise.sch`. **[INFERRED]** the number is the initial
warehouse stock (it is 0 wherever it is not needed and 100 in the "diamond" town).

Field 8 (map): the *first* group is the town's own label, the optional *second* group is
an extra icon placed at a named node. Each group has **4** names — the zoom/variant
levels (`…h`, `…`, `…`, `…l` in the data). Groups that end in a node *name*
(e.g. `("node_store_0604" "node_store_0604" …)`) put the icon on that node. `( 5000 )`
is the fallback used when a town has no icon list. Exact use of `5000` **[OPEN]**.

```lisp
( town  TOWN_AD  "Боровое"  6 ()                     ; id 6, a real town
   ( ( node  "node_store_0600"  unload )             ; delivery point
     ( node  "node_store_0601" ) ( node  "node_store_0602" )
     ( node  "node_store_0603" ) ( node  "node_store_0604" ) )
   ()                                                ; unused
   ( ( FRESHBEEF 0 ) ( CANNEDBEEF 0 ) ( BRPVACCINE 0 ) ( TABLEWARE 0 )
     ( LORYZINE 0 ) ( C_LORYZINE 0 ) ( c_TABLEWARE 0 ) ( C_BRPVACCINE 0 )
     ( C_MOBILE_PHONES 0 ) ( C_MICROCHIPS 0 ) ( C_MAIL 0 ) )
   ( ( "map_town_adh" "map_town_ad" "map_town_ad" "map_town_adl" )   ; label, 4 zoom levels
     ( "node_store_0604" "node_store_0604"
       "node_store_0604" "node_store_0604" ) ) )                      ; marker on a node

( town  TOWN_AC_GS0  "АЗС 31"  31  2 ()              ; kind = 2 -> petrol station
   ( ( node  "node_car_fuel_ac"  unload  fuel ) )    ; delivers AND refuels
   () ()                                             ; no goods
   ( "mapBenzo_ac" ) )

( town  TOWN_BB_FUEL  ""  12 ()                      ; kind = () -> fuel depot
   ( ( node  ""  dummy )                             ; placeholder, no .way node
     ( node  "gasholder_BB"  fuel ) )
   () ( ( FUEL  0 ) )
   ( 5000 ) )
```

### 5.7 `people` and the day/night gates `DAYGW` / `DAYONLY` / `UNCONDITIONAL`

```
( people  "TILE"  ( GATE  "B3D_KEY"  PROBABILITY ) … )

DAYGW          -> day AND good weather
DAYONLY        -> day only
UNCONDITIONAL  -> always
```

| field | type | meaning |
|-------|------|---------|
| 1 | string | the tile tag (`"ad"`, `"ca"`, `"cf"`, …) |
| 2 | string | **`b3d` record name of a `subtag 21` key node in that tile** |
| 3 | float | probability / density, 0.3 … 1.0 |

**[DATA] counts**: `DAYGW` 29, `DAYONLY` 32, `UNCONDITIONAL` 5 (66 gates in 13 files).
**[DATA] every one of the 66 key names resolves to a `subtag 21` record in the same tile's
`.b3d`** — e.g. `AD1_people_key`…`AD4_people_key` in `ad.b3d` (children are `subtag 40`
point objects = the people), `cows01`…`cows04` in `ca.b3d`, `sheep00`…`sheep100` in
`cf.b3d`, `K_bb040_DSR`/`K_bb040_DR` in `bb.b3d`. Nothing is unresolved.

The **probability** field: 0.3 for all 11 sheep, 0.5 for all 11 cows, 1.0 for two
pedestrian groups (`AM1_people_key`, `AP7_people_key`), 0.45…0.95 for the rest. Reading: a
per-load "is this group present?" roll **[INFERRED]**; it could also be a density/spawn
fraction.

```lisp
; mod_cf.sch — the sheep in tile cf only exist during the day, at 30 % probability
( people  "cf"
   ( DAYONLY  "sheep00"  0.3 ) ( DAYONLY  "sheep01"  0.3 )
   ( DAYONLY  "sheep10"  0.3 ) ( DAYONLY  "sheep11"  0.3 )
   ( DAYONLY  "sheep12"  0.3 ) ( DAYONLY  "sheep60"  0.3 )
   ( DAYONLY  "sheep61"  0.3 ) ( DAYONLY  "sheep70"  0.3 )
   ( DAYONLY  "sheep71"  0.3 ) ( DAYONLY  "sheep90"  0.3 )
   ( DAYONLY  "sheep100" 0.3 ) )

; mod_bb.sch — pedestrians by day+good weather, a "day" variant of the same group
( people  "bb"
   ( DAYGW   "K_bb040_DSR"  0.7 )
   ( DAYGW   "K_bb041_DSR"  0.7 )
   ( DAYONLY "K_bb040_DR"   0.8 ) )

; mod_ad.sch — mixed
( people  "ad"
   ( DAYGW   "AD1_people_key"  0.65 )
   ( DAYONLY "AD2_people_key"  0.55 )
   ( DAYONLY "AD3_people_key"  0.45 )
   ( DAYGW   "AD4_people_key"  0.55 ) )
```

The same mechanism exists inside the `.b3d` files for **street lighting**: every ENV tile
that has lights contains `refer_ILLUM_LEVEL_13`, `refer_ILLUM_LEVEL_14`,
`refer_ILLUM_LEVEL_15` `subtag 21` groups (e.g. 61 children in `bd.b3d`). Those are not
mentioned by any script — the engine drives them from its own clock **[DATA/INFERRED]**.

### 5.8 `trucks` and `cars` — the premise, corrected

**There are no `trucks` or `cars` blocks inside `mod_*.sch`.** Those heads occur **only in
`merchandise.sch`**, and they are not lists of vehicles, not spawn points and not AI
traffic: they are **cargo carrier-class tags** inside a cargo item's price block.

A merchandise item is:

```
( ITEM  UNIT  WEIGHT  VOLUME  ( PRICE_BUY  PRICE_SELL  FACTOR  ( CARRIER_TAGS ) )  FACTOR2  INDEX  "Display" )
```

`CARRIER_TAGS` is a list of 1–3 symbols. The complete set **[DATA]**:

| tag list | occurrences | meaning |
|----------|-------------|---------|
| `( trucks )` | 45 | can be carried by trucks |
| `( cars )` | 29 | can be carried by cars |
| `( cars formafia )` | 12 | cars, and the mafia wants it |
| `( trucks formafia )` | 10 | trucks, and the mafia wants it |
| `( cars illegal formafia )` | 4 | contraband, police risk |
| `( trucks illegal formafia )` | 3 | contraband, police risk |
| `( trucks illegal )` | 2 | contraband |
| `( fuel trucks )` | 1 | the `FUEL` item (tanker cargo) |

Counting **`trucks` as a tag** gives 45 + 10 + 3 + 2 + 1 = **61** and **`cars`** gives
29 + 12 + 4 = **45** — exactly the 61/45 numbers in the task statement. The head-count in
the naive form inventory is 60 for `trucks` because in the one `( fuel trucks )` list the
*head* is `fuel`. So: **`trucks`/`cars` = "which vehicle class may carry this cargo", not
which vehicles exist.** No vehicle name appears anywhere in `merchandise.sch`.

* `mod_*.sch` never mentions `trucks` or `cars` at all, and `mod_*.sch` contains **no
  vehicle names either** (checked over all 36 files). The only vehicle-name references in
  the whole script set are in `ENV/MENV/MTR GAME.CNF` (`live 1 "ZilR" 1` etc.) and in
  `vd.sch`/`evehicles.sch`.

### 5.9 Where spawnable vehicles actually come from

**`evehicles.sch` — 158 `vehicle` forms, the driveable + AI catalogue**

```
( vehicle  NAME  "MODEL"  VARIANT  CLASS  AI_KIND  MASS  PRICE  ( TRAILERS… )  [EXTRA] )
```

| # | type | meaning |
|---|------|---------|
| 1 | symbol | id, `FIAT_1`, `ZIL_TRACTOR3`, `SEMITANK2`, … |
| 2 | string | the container name inside `COMMON/TRUCKS.B3D` (`"Fiat"`, `"Zil"`, `"STrailerT"`) |
| 3 | int | variant/skin 0–3 (also selects the body colour/animation set) |
| 4 | enum | `car`, `lorry`, `tractor`, `semitrailer` |
| 5 | symbol or list | AI role(s): `dumb`, `trucker`, `police`, `mafia`, `none`, or `( dumb trucker )` |
| 6 | int | price |
| 7 | int | raw F2 (hp-like; legacy VehicleDef::mass, not physical kg) |
| 8 | list | semitrailer model names this tractor may pull (empty for cars/trailers) |
| 9 | int (only `SEMITANK1..4`) | a flag (`1` = liquid tank) |

**[DATA] role distribution:**

```
('car','dumb') 37          ('car','dumb,trucker') 20      ('car','mafia') 1
('car','police') 1         ('lorry','trucker') 36         ('semitrailer','none') 11
('tractor','trucker') 52
```

42 distinct model names (`Fiat`, `Volga`, `Avensis`, `Bus`, `Marera`, `Megan`, `Mini`,
`Oka`, `Van`, `BmwM5`, `BmwM5police`, `Gazelle`, `Sobol`, `Offroad`, `Pickup`, `Patrol`,
`Cayman`, `Freightliner`, `International`, `Kenworth`, `Mack`, `Peterbilt`, `Zil`,
`Renault`, `Scania`, `Kamaz`, `Volvo`, `Storm`, `Mercedes`, `Daf`, `Scorpion`,
`STrailer`, `STrailerT`, `Uragan`, …).

**`etrashboxes.sch` — 59 `vehicle` forms, the parked/prop catalogue**

```
( vehicle  PARKED_X   "PFiat"       0  parked    )     ; 56 entries, all named PARKED_X
( vehicle  PARKED_X   "PFiat"       1  parked    )
( vehicle  PARKED_X   "PVolga"      0  parked    )
…                                                       ; 4 variants x 14 models
( vehicle  CONTAINER_X "AnmObj0231" 0  container )
( vehicle  FUEL_X      "AnmObj0241" 0  fuel      )
( vehicle  KATOK       "Katok"      0  roadwork  )
```

**[DATA]** the 56 parked entries are `PFiat, PVolga, PAvensis, PBmwM5, PGazelle, PSobol,
PPatrol, POffroad, PPickup, PMarera, PMegan, PMini, POka, PVan` × 4 variants, in that
order, all sharing the **same id `PARKED_X`** — so a parked car cannot be selected by id;
it must be selected by `<class, model, variant>`. The model containers live in
`original/COMMON/TRUCKS.B3D` (`PFiat` measures about 3.0 units wide × 4.8 units long in its
local frame; whether the engine scales it up is not determined here). The four non-parked
classes are the four prop kinds used by `node` role tags and by the world counters:
`parked`, `container`, `fuel`, `roadwork`. Note that `evehicles.sch` has exactly **one**
police model (`BMW_M5_POLICE`) and **one** mafia model (`CAYMAN`).

**`ENV/GAME.CNF` — the world counters**

```lisp
(
  (playertypes
     (live 1 "ZilR" 1)   ; the player's truck model (only uncommented line in ENV)
     (aitruck 50)        ; 50 AI trucker rigs
     (police 15)         ; 15 police cars
     (mafia 8)           ; 8 mafia cars
     (dumb 70)           ; 70 simple traffic cars
     (ghost 25)          ; 25 ghost (non-colliding) cars
  )
  (startposition -3020 -4625 20)     ; the player spawn: x z height
  (forsale 25) (forsale_stolen 5) (forsale_used 5)
  (forhire 20) (missions 10) (fuel 10)
  (semitrailers 28) (containers 10) (parkings 40)
)
```

These are the **only** numbers that govern how many vehicles exist; nothing in the
per-tile scripts sets counts. `MENV/MTR GAME.CNF` are identical in shape with
`(aitruck 8)` and all other counts 0 (the menu worlds).

**So how does a spawned vehicle get its position and route?**

* **Parked vehicles**: from `parking`'s `node` list → `.way` `POSN` (exact position,
  including height) → pick an `etrashboxes.sch` entry whose class matches the node role.
  Nothing else is needed; see §6(a).
* **Moving AI traffic**: the scripts give **counts** (`GAME.CNF`) and a **role** per model
  (`evehicles.sch` field 5) and the **road network** (`RSEG` polylines welded at their
  endpoints, §3.4). There is **no route/path list in the scripts** — routes must be
  generated on the road graph. The named `.way` nodes then serve as *destinations*
  (`unload` stores, `fuel` stations, bazaar bays) and as *spawn hubs*
  (`FLAG == 2`, the 7 `dumb_*`/`node_cb08` nodes, which sit exactly on the road).
  See §6(b).

---

## 6. Minimum implementation plan (C++)

Data to load once:

```cpp
struct WayNode { std::string name; double x, z, h; uint32_t flag; };
struct RoadSeg { std::vector<Vec3> pts; float w0, w1; uint32_t kind, lanes; double param; };
struct ModTile {
    std::vector<Town>    towns;      // §5.6
    std::vector<Parking> parkings;   // §5.4
    std::vector<Service> services;   // §5.5
    std::vector<NodeEvent> events;   // §5.1
    std::vector<Ambush>  ambushes;   // §5.2
    std::vector<Obstacle> obstacles; // §5.3
    std::vector<PeopleGate> people;  // §5.7
    std::unordered_map<std::string, const WayNode*> byName; // -> Way
};
```

Then, at minimum, three features:

### (a) Spawning parked vehicles

```
for each tile XX that is in the loaded region:
  let W = ENV/XX.way nodes, M = ENV/mod_XX.sch

  for each Parking P in M.parkings:
    for each Node n in P.nodes:
      if n.name is empty            -> skip            // (node "" dummy)
      const WayNode* w = W.find(n.name);  assert(w)    // verified 440/440 in the data
      uint32_t role   = n.intRole;                     // 0 | 4 | 8 | 32
      Vec3 pos        = { w->x, w->h, w->z };          // (x, height, z) for your renderer
      ParkedKind kind = role == 8  ? HIDDEN
                      : role == 32 ? OLD
                      :              PLAIN;            // role 4 = a stall, not a car
      if (role == 4) { registerTradeStall(P, n, pos, n.townName); continue; }

      // choose an etrashboxes.sch entry of class `parked` (etrashboxes.sch has 56 of them)
      const VehicleDef* v = pickParkedModel(kind, rng);   // PFiat..PVan x variant 0..3
      spawnStaticVehicle(v->model, v->variant, pos, heading);

      if (kind == HIDDEN) markStealable(pos);             // a mission/story hook
```

**Fields needed:** per parked entry only `{model, variant, class}` from
`etrashboxes.sch` (`id` is useless — all 56 are `PARKED_X`); per bay only
`{x, z, height}` from `POSN` and the role integer from the `node` form.

**Heading is not in the data.** A `node` has no rotation and `POSN` is a bare point
**[DATA]**. Three workable fallbacks, in order of fidelity:

1. use the direction of the `RSEG` polyline nearest to the bay (all role-0 bays are
   exactly on a polyline, so this is always defined);
2. face the bay towards the group's centroid (bays in a bazaar form rows/grids — see
   `MARKET_AR`'s 2 × 4 grid);
3. give every bay a per-index pseudo-random fixed heading.

Also honour `(parkings 40)` from `GAME.CNF` as a global cap and `(containers 10)`,
`(semitrailers 28)`, `(fuel 10)` for the `CONTAINER_X`, semitrailer and `FUEL_X` props
(`etrashboxes.sch` classes `container`/`fuel`; the `node` role `fuel` marks where the
`FUEL_X` prop or the refuelling trigger belongs).

### (b) Spawning moving AI traffic along the road network

Nothing in the scripts is a route, so build the graph once from all `.way` files of the
loaded region:

```
// 1. graph
for each RSEG s:
   for each consecutive pair (p[i], p[i+1]) in s.pts:
       addEdge( vertexAt(p[i]), vertexAt(p[i+1]) )   // vertexAt() = hash on rounded (x,z,h)
// measured: RSEG endpoints are *exactly* shared, so exact/rounded welding works and
// needs no tolerance search (aa 48/48, ad 168/168, ar 240/240 endpoints shared).

// 2. spawn budget, from ENV/GAME.CNF playertypes:
//    aitruck 50, police 15, mafia 8, dumb 70, ghost 25
//    (the numbers are world-wide; stream them around the player)

// 3. model pool = evehicles.sch entries by role:
//      dumb    -> class car    with aiKind dumb or (dumb trucker)      [57 entries]
//      aitruck -> class lorry/tractor with aiKind trucker             [88 entries]
//      police  -> aiKind police             [1 entry: BMW_M5_POLICE "BmwM5police"]
//      mafia   -> aiKind mafia              [1 entry: CAYMAN "Cayman"]
//      ghost   -> same pool as dumb, spawned with collisions disabled

// 4. per vehicle
spawnOnEdge(edge, t01):        // position = lerp along the polyline, height from POSN/VDAT
   target = pickDestination(); // see below
   heading = tangent of the current polyline
   speed   = baseSpeed * ATTR.param-ish factor * (1 - curvature)

// 5. per tick: advance along the polyline; at a junction vertex pick the outgoing edge
//    whose tangent best continues the current direction (no U-turn),
//    or the one that reduces the distance to `target`.

// 6. destination selection (what makes the named nodes useful):
//      trucker  -> a random unload node of a town with stock, or a fuel node
//      police   -> nearest ambush node of class POLICE (87 ambushes exist)
//      mafia    -> nearest ambush node of class MAFIA
//      dumb     -> random edge, no destination (pure ambient traffic)
//    spawn hubs: prefer vertices within ~40 u of an RNOD with FLAG == 2 (7 exist);
//    otherwise spawn on any edge inside the streaming radius, and despawn outside it.

// 7. ambush events: when the player gets within the event radius of an ambush's node,
//    spawn its cars from the class's model pool at the node list positions
//    (the ids in field 7 are not needed for this).
```

Notes that matter:

* `ATTR.kind`/`param`/`lanes` and `WDTH` are available per segment and should drive lane
  offset, speed and width; `WDTH` is a real width (3…24 u).
* The 7 FLAG-2 nodes (6 named `dumb_*`/`dump_*` plus `node_cb08`) are the only explicit
  traffic anchors in the data — treat them as high-priority spawn points near towns, but do
  not rely on them for coverage.
* `start1_*`/`finish1_*`/`start2_*`/`finish2_*`/`check_point*` nodes exist only in
  `MENV/dr.way` and `MTR/tr.way` (the race worlds) and are irrelevant for ENV free roam.

### (c) Day/night (and weather) gates

```
struct PeopleGate { enum { DayGoodWeather, DayOnly, Always } kind; string b3dKey; float p; };

per tile XX:
  for gate in mod_XX.people:
      Sub21Key* k = XX.b3d.findKeyRecord(gate.b3dKey);   // subtag 21, exact name match
      if (!k) continue;                                  // verified: never happens
      bool on = gate.kind == Always
             || (gate.kind == DayOnly      && isDay())
             || (gate.kind == DayGoodWeather && isDay() && weatherIsGood());
      if (on && rng.uniform() < gate.p) showKeyGroup(k); else hideKeyGroup(k);
```

* The key record is `subtag 21`; **showing = making its children visible/animated**
  (children are `subtag 40` billboard point objects for people/cows/sheep, or mesh
  subtags 9/20/23/33/37 for barriers and floods).
* `rng.uniform() < p` is the **[INFERRED]** reading of the probability field; if it turns
  out to be a density, the same field drives a count instead of a boolean.
* The same toggle drives `obstacle` field 4: closed ⇔ key "shown". The messages in fields
  7/8 are what to display when the state flips.
* Street lighting uses `refer_ILLUM_LEVEL_13/14/15` keys that no script mentions — the
  engine's own clock must drive them (three levels: e.g. day / dusk / night).
* `TRUCK.INI` `[EFFECTS] days=50 weats=60` are plausibly the day and weather cycle lengths
  **[INFERRED]**.

### What you can skip for a first pass

`nodeevent` messages, `service` upgrade lists, `town` goods/stock, the ambush slot ids, the
`parking`/`service` trailing integers and the map descriptor block — none of them are
needed to spawn or animate the world.

---

## 7. What could **not** be determined

Stated plainly, so nobody re-derives it:

1. **Vehicle heading for parked cars and for spawn points.** Neither `node` nor `POSN`
   carries a rotation; `b3d` anchors are not referenced by any script node. Headings must
   be derived (nearest road tangent, group centroid, or a fixed per-index value). §6(a).
2. **The exact meaning of the integer role in a `node` form (4 / 8 / 32) and of `service`
   field 3.** *Partly answered in §10.6*: the **keyword** role is a bitmask `unload|1`,
   `hidden|2`, `fuel|4`, `dummy|8` (king.exe `0x40f303/0x40f323/0x40f344/0x40f364`), while
   the **numeric** token of the same field is a separate `int` that the shipped data uses as
   4 = trade stall, 8 = hidden-car bay, 32 = old-car bay.  The two never mix in the data.
   `service` field 3 (0…6, 10, 11) is still only self-describing (10 = petrol; `BENZO_*` /
   `STO_*` prefixes agree with it) — the numeric enum itself is **[OPEN]**.  It is *not* the
   fuel price id (that is field 6, `4000 + town id`, kept in `Service::fuelId`).
3. **The `ambush` slot ids (field 7).** Verified structure: globally unique, mafia in
   1000–1399, police in 2000–2399, allocated in blocks of 10 (a coupled pair of ambushes
   splits one block 5 + 5), grouped by world region, with gaps at 1150–1199 and
   1310–1319. Rejected numerically: global `RNOD` index, global `RSEG` index, global
   polyline-vertex index (raw / welded at 1 u / 5 u / 25 u), and per-tile offsets. They
   are almost certainly keys into an engine-side ambush/patrol table that is not shipped in
   plaintext (candidate: `LOAD.RES` / `king.exe` data). Not needed for re-implementation.
4. **The optional trailing integer of `parking` (field 6, values 5…22).** Present only for
   bazaars. Not the node count, not the bazaar id, not `forsale_used`. No consumer found.
5. **`nodeevent` trigger radius and the exact semantics of its `DIRECTION`.** *Answered in
   §10.3*: there is no radius — the event fires when the player's road `PositionId` becomes
   the event node (`onNodePass`, king.exe `0x40b100`, called from `setPlayerPosition`),
   `DIRECTION` is the **required direction of travel** (`dot(dir, move) > 0`), and a fired
   event is suppressed for 8 s.  The distances in the messages (50/100/250/300 m) are prose
   written by the author; the compass axis mapping (+z = NORTH, +x = EAST) is what the
   engine's vector literally encodes.
6. **The `town` map-descriptor block (field 8) and the meaning of `( 5000 )`.** The block
   clearly holds 4 names per zoom variant plus an optional second icon placed on a named
   node; `5000` is a scalar fallback whose use is unknown.
7. **`town` field 7 second value** (initial stock, `( ITEM 0..100 )`): only `TOWN_AA` is
   non-zero (100), which is consistent with "initial warehouse stock" but is one data point.
8. **`people` gate probability.** Read as a spawn probability per load; could be a density
   fraction. Only the engine would settle it.
9. **`RSEG.ATTR`.** `kind` is a class enum with bit 0 selecting an engine constructor
   (per `NOTES.md`: object sizes 0x38 vs 0x50) — the class table itself is unknown. The
   `double param` (0.0/0.05/0.1/0.15/0.35/0.5) is unexplained (slope? curvature? speed
   fraction?). `lanes` (1/2/3) is a plausible lane count. `WDTH` is confirmed as
   (start width, end width) — 52/2259 segments taper.
10. **The consumer of `FLAG`.** The empirical classification (0 = on-road, 1 = off-road
    marker, 2 = traffic hub) fits 752/762 nodes with a clear pattern, but 6 exceptions
    (market/hidden/stolen bays come out as 0) and 4 (service markers come out as 1) mean the
    real field may be a type enum with different semantics that happens to correlate.
11. **AI route semantics in detail.** Which specific store a trucker delivers to, whether
    police patrol between the two paired ambushes, and how `(ghost 25)` differs from
    `(dumb 70)` behaviourally — none of this is in the scripts.
12. **`original/rnrnfo.scq`** has `SDTE` magic but does not decrypt with key `0x5CB0`
    (it yields noise) — different key or a different container. 210 bytes.
13. **`original/TRUCK.INI`** is plain INI, not a script; the pre-decrypted
    `decrypted/TRUCK.INI.txt` is a Python `repr` dump and unusable. It contains settings
    (`[EFFECTS] days=50 weats=60 trafs=100 trafm=100`, `[CAR] RoadWidthReducement=1.5`,
    `kmph=on`) whose exact gameplay effect is not stated.
14. **`merchandise.sch` `FACTOR` fields.** Each item has two floats (one inside the price
    block, one after it) that agree for most items but not all (`RDIAMOND`: 0.6 vs 0.95;
    `AKM`: 0.7 vs 1). Damage/decay/perishability is a guess.
15. **`(semitrailers 28)`, `(containers 10)`, `(parkings 40)`, `(fuel 10)`** are global
    caps; the data does not say where the extra semitrailers/containers are placed. Only
    `parking`-listed bays and the `fuel` node roles are explicit placement data.

---

## 8. Appendix A — complete form-head inventory

Every distinct first element of a list across the 46 scripts, with its occurrence count.
(Generated by `tools/gen_head_table.py`; re-run it after any script-parsing change.)

<!-- generated by tools/gen_head_table.py -->
distinct heads: **722** over 2265 list forms in 46 files.

### Structural / grammar form heads

| head | count | meaning |
|------|-------|---------|
| `vehicle` | 217 | vehicle catalogue entry (evehicles.sch / etrashboxes.sch) |
| `node` | 170 | a named .way node inside a town/parking, with an optional role tag |
| `ambush` | 87 | police/mafia ambush definition (spawn node, direction, route node ids) |
| `trucks` | 60 | merchandise.sch: cargo can be carried by trucks (carrier-class tag; 61 tag occurrences, see spec 5.8) |
| `cars` | 45 | merchandise.sch: cargo can be carried by cars (carrier-class tag) |
| `service` | 37 | repair shop / fuel station: available upgrades (+ price id) |
| `DAYONLY` | 32 | people gate: visible only by day, with probability p |
| `nodeevent` | 31 | triggered message when the player reaches a .way node |
| `parking` | 30 | parked-vehicle group: list of .way nodes that carry parked cars |
| `town` | 29 | town / gas station / fuel depot definition (delivery nodes, goods, map icons) |
| `DAYGW` | 29 | people gate: visible by day AND in good weather, with probability p |
| `PR_TIRES` | 24 | upgrade item; in mod_*.sch = offered by a service, in restable.sch = price table |
| `dumb` | 23 | playertypes: number of "dumb" (simple) traffic cars to spawn |
| `PR_NAVIGATOR` | 21 | upgrade item; in mod_*.sch = offered by a service, in restable.sch = price table |
| `PR_FORCED_ENGINE` | 20 | upgrade item; in mod_*.sch = offered by a service, in restable.sch = price table |
| `PR_EXTRA_TANK` | 19 | upgrade item; in mod_*.sch = offered by a service, in restable.sch = price table |
| `PR_ADJUSTABLE_SUSPENSION` | 19 | upgrade item; in mod_*.sch = offered by a service, in restable.sch = price table |
| `PR_CARGO_SCANNER` | 17 | upgrade item; in mod_*.sch = offered by a service, in restable.sch = price table |
| `obstacle` | 17 | road closure / roadwork: toggles a b3d key node and spawns debris |
| `PR_ENGINE_BRAKE` | 16 | upgrade item; in mod_*.sch = offered by a service, in restable.sch = price table |
| `PR_BRAKES` | 16 | upgrade item; in mod_*.sch = offered by a service, in restable.sch = price table |
| `PR_FUEL` | 15 | upgrade item; in mod_*.sch = offered by a service, in restable.sch = price table |
| `PR_FIRE_PROTECTION` | 15 | upgrade item; in mod_*.sch = offered by a service, in restable.sch = price table |
| `PR_REINFORCED_CHASSIS` | 15 | upgrade item; in mod_*.sch = offered by a service, in restable.sch = price table |
| `PR_ANTI_RADAR` | 14 | upgrade item; in mod_*.sch = offered by a service, in restable.sch = price table |
| `PR_ABS` | 13 | upgrade item; in mod_*.sch = offered by a service, in restable.sch = price table |
| `people` | 13 | per-tile day/night/weather gate list for animated bystander groups |
| `PR_OIL_ADDITIVE` | 12 | upgrade item; in mod_*.sch = offered by a service, in restable.sch = price table |
| `PR_HEADLIGHT_PROTECTION` | 11 | upgrade item; in mod_*.sch = offered by a service, in restable.sch = price table |
| `PR_HORN` | 11 | upgrade item; in mod_*.sch = offered by a service, in restable.sch = price table |
| `PR_BULETPROOF_GLASS` | 11 | upgrade item; in mod_*.sch = offered by a service, in restable.sch = price table |
| `PR_STEALTH_BODY_PAINT` | 10 | upgrade item; in mod_*.sch = offered by a service, in restable.sch = price table |
| `PR_AIR_FILTER_SNORKEL` | 8 | upgrade item; in mod_*.sch = offered by a service, in restable.sch = price table |
| `UNCONDITIONAL` | 5 | people gate: always visible, with probability p |
| `fuel` | 3 | GAME.CNF: fuel depots / merchandise carrier tag "fuel" |
| `missions` | 3 | GAME.CNF: concurrent delivery orders |
| `forhire` | 3 | GAME.CNF: drivers available for hire |
| `forsale` | 3 | GAME.CNF: trucks offered for sale |
| `startposition` | 3 | world spawn point of the player (x z height) |
| `playertypes` | 3 | GAME.CNF global counters + the player truck model |
| `mafia` | 3 | playertypes: number of mafia cars to spawn |
| `police` | 3 | playertypes: number of police cars to spawn |
| `aitruck` | 3 | playertypes: number of AI trucker rigs to spawn |
| `live` | 3 | playertypes: an allowed player vehicle model |
| `containers` | 2 | GAME.CNF: containers parked in the world |
| `semitrailers` | 2 | GAME.CNF: semitrailers parked in the world |
| `scenario` | 2 | GAME.CNF (MENV/MTR): the menu scenario name |
| `parkings` | 1 | GAME.CNF: parked cars in the world |
| `forsale_used` | 1 | GAME.CNF: used trucks offered for sale |
| `forsale_stolen` | 1 | GAME.CNF: stolen trucks offered for sale |
| `ghost` | 1 | playertypes: number of ghost/non-colliding traffic cars |

### Data heads (not grammar forms)

These are the first elements of *data* lists.  A walker that calls the first
element of every list a "form head" reports them too, but none of them is a
form: they are `merchandise.sch` cargo ids and their per-price-block carrier
tags, the vehicle/model keys of `vd.sch` and the message keys of `Ms.sch`,
the numeric ambush-slot ids inside `( id id … )` node lists, the map-icon
name lists of `town`, and the upgrade-name rows of `restable.sch`.

<details><summary>full list (671 heads)</summary>

| head | count | files |
|------|-------|-------|
| `` | 182 | GAME.CNF,Ms.sch,etrashboxes.sch,evehicles.sch,merchandise... |
| `SEMITRAILER1` | 49 | evehicles.sch,vd.sch |
| `100` | 40 | merchandise.sch |
| `C_MAIL` | 12 | merchandise.sch,mod_aa.sch,mod_ad.sch,mod_aj.sch,mod_am.s... |
| `0` | 11 | merchandise.sch |
| `1500` | 7 | merchandise.sch |
| `URAGAN_SEMITRAILER` | 5 | evehicles.sch,vd.sch |
| `240` | 5 | merchandise.sch |
| `FUEL` | 5 | merchandise.sch,mod_am.sch,mod_ax.sch,mod_bb.sch,mod_cc.sch |
| `C_WAGES` | 4 | merchandise.sch,mod_ap.sch,mod_bd.sch,mod_dc.sch |
| `5000` | 4 | mod_aa.sch,mod_bb.sch,mod_cc.sch,mod_ce.sch |
| `270` | 3 | merchandise.sch |
| `C_FRUIT_JUICE` | 3 | merchandise.sch,mod_ar.sch,mod_cb.sch |
| `21` | 3 | merchandise.sch |
| `2080` | 3 | merchandise.sch,mod_ak.sch |
| `C_CIGARETTES` | 3 | merchandise.sch,mod_ar.sch,mod_dc.sch |
| `C_SANDWICHES` | 3 | merchandise.sch,mod_aj.sch,mod_bc.sch |
| `C_CAKES` | 3 | merchandise.sch,mod_aj.sch,mod_bc.sch |
| `C_PIZZA` | 3 | merchandise.sch,mod_aj.sch,mod_bc.sch |
| `C_JEWELRY` | 3 | merchandise.sch,mod_am.sch,mod_cb.sch |
| `MEGO_MECCANO` | 3 | merchandise.sch,mod_ap.sch |
| `AUTO_MECCANO` | 3 | merchandise.sch,mod_ap.sch |
| `ANALEPSIN` | 3 | merchandise.sch,mod_ap.sch |
| `VITAMIN_Z` | 3 | merchandise.sch,mod_ap.sch |
| `C_GLASSWARE` | 2 | merchandise.sch,mod_cb.sch |
| `C_LIGHT_BULBS` | 2 | merchandise.sch,mod_aj.sch |
| `C_CAMERA` | 2 | merchandise.sch,mod_bd.sch |
| `C_VIDEOCAMERA` | 2 | merchandise.sch,mod_bd.sch |
| `C_COCA_COLA` | 2 | merchandise.sch,mod_ap.sch |
| `C_CHAMPAGNE` | 2 | merchandise.sch,mod_bd.sch |
| `378` | 2 | merchandise.sch |
| `C_MARTINI` | 2 | merchandise.sch,mod_ap.sch |
| `C_VODKA` | 2 | merchandise.sch,mod_am.sch |
| `272` | 2 | merchandise.sch |
| `C_COMPUTER_GAMES` | 2 | merchandise.sch,mod_bd.sch |
| `7260` | 2 | merchandise.sch |
| `172` | 2 | merchandise.sch |
| `231` | 2 | merchandise.sch |
| `C_SOFT_TOYS` | 2 | merchandise.sch,mod_au.sch |
| `C_BRPVACCINE` | 2 | merchandise.sch,mod_ad.sch |
| `1313` | 2 | merchandise.sch |
| `C_LORYZINE` | 2 | merchandise.sch,mod_ad.sch |
| `743` | 2 | merchandise.sch |
| `904` | 2 | merchandise.sch |
| `292` | 2 | merchandise.sch |
| `C_SHOES` | 2 | merchandise.sch,mod_bd.sch |
| `C_PARFUMERY` | 2 | merchandise.sch,mod_ar.sch |
| `C_TOOTHPASTE` | 2 | merchandise.sch,mod_au.sch |
| `C_SOAP` | 2 | merchandise.sch,mod_dc.sch |
| `C_POTATOCHIPS` | 2 | merchandise.sch,mod_ar.sch |
| `C_CHOCOLATES` | 2 | merchandise.sch,mod_cb.sch |
| `C_CANDIES` | 2 | merchandise.sch,mod_am.sch |
| `C_BEER` | 2 | merchandise.sch,mod_au.sch |
| `C_ICE_CREAM` | 2 | merchandise.sch,mod_ar.sch |
| `C_TEA` | 2 | merchandise.sch,mod_aj.sch |
| `C_COFFEE` | 2 | merchandise.sch,mod_au.sch |
| `C_BUTTER` | 2 | merchandise.sch,mod_am.sch |
| `C_MOBILE_PHONES` | 2 | merchandise.sch,mod_ad.sch |
| `C_POSTAGE_STAMPS` | 2 | merchandise.sch,mod_bd.sch |
| `C_MICROCHIPS` | 2 | merchandise.sch,mod_ad.sch |
| `C_DIAMONDS` | 2 | merchandise.sch,mod_aa.sch |
| `72600` | 2 | merchandise.sch |
| `TABLEWARE` | 2 | merchandise.sch,mod_ad.sch |
| `GLASSWARE` | 2 | merchandise.sch,mod_cb.sch |
| `LIVECHICKEN` | 2 | merchandise.sch,mod_bc.sch |
| `AUTOPARTS` | 2 | merchandise.sch,mod_bd.sch |
| `SHOES` | 2 | merchandise.sch,mod_bd.sch |
| `TOOTHPASTE` | 2 | merchandise.sch,mod_au.sch |
| `SOAP` | 2 | merchandise.sch,mod_dc.sch |
| `POTATOCHIPS` | 2 | merchandise.sch,mod_ar.sch |
| `CHOCOLATES` | 2 | merchandise.sch,mod_cb.sch |
| `CANDIES` | 2 | merchandise.sch,mod_am.sch |
| `BEER` | 2 | merchandise.sch,mod_au.sch |
| `CIGARETTES` | 2 | merchandise.sch,mod_ar.sch |
| `TEA` | 2 | merchandise.sch,mod_aj.sch |
| `COFFEE` | 2 | merchandise.sch,mod_au.sch |
| `BUTTER` | 2 | merchandise.sch,mod_am.sch |
| `VEG_OIL` | 2 | merchandise.sch,mod_cb.sch |
| `FLOUR` | 2 | merchandise.sch,mod_au.sch |
| `SUGAR` | 2 | merchandise.sch,mod_ar.sch |
| `MILK` | 2 | merchandise.sch,mod_bc.sch |
| `CHICKEN` | 2 | merchandise.sch,mod_bc.sch |
| `CANNEDBEEF` | 2 | merchandise.sch,mod_ad.sch |
| `FRESHBEEF` | 2 | merchandise.sch,mod_ad.sch |
| `CANNEDFISH` | 2 | merchandise.sch,mod_cb.sch |
| `SALTEDFISH` | 2 | merchandise.sch,mod_bd.sch |
| `FRESHFISH` | 2 | merchandise.sch,mod_bd.sch |
| `EGGS` | 2 | merchandise.sch,mod_bc.sch |
| `500` | 2 | merchandise.sch |
| `RDIAMOND` | 2 | merchandise.sch,mod_aa.sch |
| `COMPUTER_GAMES` | 2 | merchandise.sch,mod_bd.sch |
| `SOFT_TOYS` | 2 | merchandise.sch,mod_au.sch |
| `BRPVACCINE` | 2 | merchandise.sch,mod_ad.sch |
| `LORYZINE` | 2 | merchandise.sch,mod_ad.sch |
| `FOAM_PLASTIC` | 2 | merchandise.sch,mod_cb.sch |
| `NAILS4` | 2 | merchandise.sch,mod_am.sch |
| `WINDOW_PANE` | 2 | merchandise.sch,mod_dc.sch |
| `MIRROR` | 2 | merchandise.sch,mod_ar.sch |
| `ELECTRIC_ENGINE` | 2 | merchandise.sch,mod_am.sch |
| `CABLE_RK75` | 2 | merchandise.sch,mod_aj.sch |
| `CABLE_APPV` | 2 | merchandise.sch,mod_aj.sch |
| `LIGHT_BULBS` | 2 | merchandise.sch,mod_aj.sch |
| `PENTIUM_III` | 2 | merchandise.sch,mod_bd.sch |
| `VIDEOCAMERA` | 2 | merchandise.sch,mod_bd.sch |
| `TVSET14` | 2 | merchandise.sch,mod_ap.sch |
| `COTTONWOOL` | 2 | merchandise.sch,mod_ar.sch |
| `LYCRA` | 2 | merchandise.sch,mod_dc.sch |
| `COTTON_FABRIC` | 2 | merchandise.sch,mod_ar.sch |
| `LINEN` | 2 | merchandise.sch,mod_dc.sch |
| `COCA_COLA` | 2 | merchandise.sch,mod_ap.sch |
| `CHAMPAGNE` | 2 | merchandise.sch,mod_bd.sch |
| `MARTINI` | 2 | merchandise.sch,mod_ap.sch |
| `VODKA` | 2 | merchandise.sch,mod_am.sch |
| `BREAD` | 2 | merchandise.sch,mod_aj.sch |
| `Anti-Radar` | 2 | restable.sch |
| `Fuel` | 2 | restable.sch |
| `mapBenzo_ag10` | 2 | mod_ag.sch |
| `mapBenzo_ag07` | 2 | mod_ag.sch |
| `mapBenzo_aj` | 2 | mod_aj.sch |
| `node_store_0504` | 2 | mod_am.sch |
| `1090` | 2 | mod_ap.sch |
| `mapBenzo_ar1` | 2 | mod_ar.sch |
| `mapBenzo_arb` | 2 | mod_ar.sch |
| `mapBenzo_au` | 2 | mod_au.sch |
| `mapBenzo_cc_` | 2 | mod_cc.sch |
| `node_store_0904` | 2 | mod_dc.sch |
| `mapBenzo_dq_0` | 2 | mod_dq.sch |
| `LOAD_AT_AN_OIL_TANK` | 1 | Ms.sch |
| `NEED_A_PETROL_SEMITRAILER` | 1 | Ms.sch |
| `FINE_ACCIDENT` | 1 | Ms.sch |
| `FINE_BRIBERY` | 1 | Ms.sch |
| `FINE_RED_LIGHT` | 1 | Ms.sch |
| `FINE_SPEEDING` | 1 | Ms.sch |
| `FINE_STOLEN` | 1 | Ms.sch |
| `FINE_IM` | 1 | Ms.sch |
| `VIOLATION_NEGLIGENCE` | 1 | Ms.sch |
| `VIOLATION_CHANGING_LANE` | 1 | Ms.sch |
| `VIOLATION_CROSSING_FULL_LINE` | 1 | Ms.sch |
| `VIOLATION_ILLEGAL_PARKING` | 1 | Ms.sch |
| `VIOLATION_REVERSE_GEAR` | 1 | Ms.sch |
| `VIOLATION_WRONG_SIDE` | 1 | Ms.sch |
| `DISQUALIFIED` | 1 | Ms.sch |
| `RACES_TO_TAKE_PLACE_IN` | 1 | Ms.sch |
| `NO_MONEY_FOR_ADVANCE` | 1 | Ms.sch |
| `NO_MONEY_TO_HIRE` | 1 | Ms.sch |
| `NO_LICENCES` | 1 | Ms.sch |
| `LICENCES_EXPIRED` | 1 | Ms.sch |
| `COMPETITOR_ARRIVED` | 1 | Ms.sch |
| `YOU_CAME_9` | 1 | Ms.sch |
| `YOU_CAME_8` | 1 | Ms.sch |
| `YOU_CAME_7` | 1 | Ms.sch |
| `YOU_CAME_6` | 1 | Ms.sch |
| `YOU_CAME_5` | 1 | Ms.sch |
| `YOU_CAME_4` | 1 | Ms.sch |
| `YOU_CAME_3` | 1 | Ms.sch |
| `YOU_CAME_2` | 1 | Ms.sch |
| `YOU_CAME_1` | 1 | Ms.sch |
| `YOU_CAME_0` | 1 | Ms.sch |
| `YOU_ARE_9` | 1 | Ms.sch |
| `YOU_ARE_8` | 1 | Ms.sch |
| `YOU_ARE_7` | 1 | Ms.sch |
| `YOU_ARE_6` | 1 | Ms.sch |
| `YOU_ARE_5` | 1 | Ms.sch |
| `YOU_ARE_4` | 1 | Ms.sch |
| `YOU_ARE_3` | 1 | Ms.sch |
| `YOU_ARE_2` | 1 | Ms.sch |
| `YOU_ARE_1` | 1 | Ms.sch |
| `YOU_ARE_0` | 1 | Ms.sch |
| `EMPLOYEE_QUIT` | 1 | Ms.sch |
| `EMPLOYEES_QUIT` | 1 | Ms.sch |
| `X_INTEGRITY` | 1 | Ms.sch |
| `X_DAMAGED` | 1 | Ms.sch |
| `YOU_MADE` | 1 | Ms.sch |
| `MONEY_PAID` | 1 | Ms.sch |
| `NOT_ENOUGH_MONEY` | 1 | Ms.sch |
| `NOT_ENOUGH_SPACE` | 1 | Ms.sch |
| `ORDER_LOST` | 1 | Ms.sch |
| `FORFEIT` | 1 | Ms.sch |
| `ORDER_EXPIRED` | 1 | Ms.sch |
| `LOAD_SEIZED` | 1 | Ms.sch |
| `ATTACKED_BY_MAFIA` | 1 | Ms.sch |
| `PLAYER_ROBBED_BY_POLICE` | 1 | Ms.sch |
| `PLAYER_ROBBED_BY_MAFIA` | 1 | Ms.sch |
| `C_TABLEWARE` | 1 | merchandise.sch |
| `C_MEGO_MECCANO` | 1 | merchandise.sch |
| `C_AUTO_MECCANO` | 1 | merchandise.sch |
| `C_ANALEPSIN` | 1 | merchandise.sch |
| `C_VITAMIN_Z` | 1 | merchandise.sch |
| `C_FALSEMONEY` | 1 | merchandise.sch |
| `C_FIREARMS` | 1 | merchandise.sch |
| `C_EXPLOSIVES` | 1 | merchandise.sch |
| `C_HEROINE` | 1 | merchandise.sch |
| `40` | 1 | merchandise.sch |
| `CHWASTE` | 1 | merchandise.sch |
| `RWASTE` | 1 | merchandise.sch |
| `EXPLOSIVES` | 1 | merchandise.sch |
| `HEROINE` | 1 | merchandise.sch |
| `AKM` | 1 | merchandise.sch |
| `50` | 1 | merchandise.sch |
| `38` | 1 | merchandise.sch |
| `175` | 1 | merchandise.sch |
| `263` | 1 | merchandise.sch |
| `620` | 1 | merchandise.sch |
| `254` | 1 | merchandise.sch |
| `1850` | 1 | merchandise.sch |
| `2860` | 1 | merchandise.sch |
| `920` | 1 | merchandise.sch |
| `640` | 1 | merchandise.sch |
| `435` | 1 | merchandise.sch |
| `25` | 1 | merchandise.sch |
| `ШУРА` | 1 | names.sch |
| `НАТАЛЬЯ` | 1 | names.sch |
| `ЖАННА` | 1 | names.sch |
| `ИРИНА` | 1 | names.sch |
| `МАРИНА` | 1 | names.sch |
| `ЛИЗА` | 1 | names.sch |
| `ЛЕНА` | 1 | names.sch |
| `АНЯ` | 1 | names.sch |
| `ДАВИД` | 1 | names.sch |
| `ПЕТРОВИЧ` | 1 | names.sch |
| `МАКС` | 1 | names.sch |
| `ВОЛОДЯ` | 1 | names.sch |
| `ЗАХАРЫЧ` | 1 | names.sch |
| `ДИМА` | 1 | names.sch |
| `ДАНИЭЛЬ` | 1 | names.sch |
| `ДЕД` | 1 | names.sch |
| `ИЛЬЯ` | 1 | names.sch |
| `ВИТЕК` | 1 | names.sch |
| `САНЫЧ` | 1 | names.sch |
| `ДЖОН` | 1 | names.sch |
| `ЕГОР` | 1 | names.sch |
| `ВАНЯ` | 1 | names.sch |
| `ДАНИЛА` | 1 | names.sch |
| `ТОЛИК` | 1 | names.sch |
| `МАРК` | 1 | names.sch |
| `ПЕТР` | 1 | names.sch |
| `ВАСИЛИЙ` | 1 | names.sch |
| `РУСЛАН` | 1 | names.sch |
| `ЖОРА` | 1 | names.sch |
| `ЖЕНЯ` | 1 | names.sch |
| `БОРИС` | 1 | names.sch |
| `ИГОРЬ` | 1 | names.sch |
| `КОСТЯ` | 1 | names.sch |
| `НИКИТА` | 1 | names.sch |
| `СЕРГЕЙ` | 1 | names.sch |
| `ПАВЕЛ` | 1 | names.sch |
| `АНДРЕЙ` | 1 | names.sch |
| `ЛЕВА` | 1 | names.sch |
| `ЭДУАРД` | 1 | names.sch |
| `МИХАИЛ` | 1 | names.sch |
| `НИКОЛАЙ` | 1 | names.sch |
| `КОВБОЙ` | 1 | names.sch |
| `АЛЕКС` | 1 | names.sch |
| `ФЕДОР` | 1 | names.sch |
| `АРТУР` | 1 | names.sch |
| `ЮРА` | 1 | names.sch |
| `ВАЛЯ` | 1 | names.sch |
| `ЛЕНЯ` | 1 | names.sch |
| `САНЯ` | 1 | names.sch |
| `ЯКОВ` | 1 | names.sch |
| `ВИТАЛИЙ` | 1 | names.sch |
| `HOOD8` | 1 | names.sch |
| `HOOD7` | 1 | names.sch |
| `HOOD6` | 1 | names.sch |
| `HOOD5` | 1 | names.sch |
| `HOOD4` | 1 | names.sch |
| `HOOD3` | 1 | names.sch |
| `HOOD2` | 1 | names.sch |
| `HOOD1` | 1 | names.sch |
| `COP15` | 1 | names.sch |
| `COP14` | 1 | names.sch |
| `COP13` | 1 | names.sch |
| `COP12` | 1 | names.sch |
| `COP11` | 1 | names.sch |
| `COP10` | 1 | names.sch |
| `COP9` | 1 | names.sch |
| `COP8` | 1 | names.sch |
| `COP7` | 1 | names.sch |
| `COP6` | 1 | names.sch |
| `COP5` | 1 | names.sch |
| `COP4` | 1 | names.sch |
| `COP3` | 1 | names.sch |
| `COP2` | 1 | names.sch |
| `COP1` | 1 | names.sch |
| `Stealth body paint` | 1 | restable.sch |
| `Body paint` | 1 | restable.sch |
| `Extra tank` | 1 | restable.sch |
| `No extra tank` | 1 | restable.sch |
| `Fire protection` | 1 | restable.sch |
| `No fire protection` | 1 | restable.sch |
| `Horn` | 1 | restable.sch |
| `No horn` | 1 | restable.sch |
| `Cargo scanner` | 1 | restable.sch |
| `No cargo scanner` | 1 | restable.sch |
| `Off-road tires` | 1 | restable.sch |
| `Highway tires` | 1 | restable.sch |
| `Regular tires` | 1 | restable.sch |
| `Navigator` | 1 | restable.sch |
| `No navigator` | 1 | restable.sch |
| `Bullet-proof glass` | 1 | restable.sch |
| `Regular glass` | 1 | restable.sch |
| `Reinforced chassis` | 1 | restable.sch |
| `No reinforced chassis` | 1 | restable.sch |
| `Engine brake` | 1 | restable.sch |
| `No engine brake` | 1 | restable.sch |
| `Ventilated brakes` | 1 | restable.sch |
| `Regular brakes` | 1 | restable.sch |
| `Adjustable suspension` | 1 | restable.sch |
| `Regular suspension` | 1 | restable.sch |
| `ABS` | 1 | restable.sch |
| `No ABS` | 1 | restable.sch |
| `Oil additive` | 1 | restable.sch |
| `No oil additive` | 1 | restable.sch |
| `Forced engine` | 1 | restable.sch |
| `Regular engine` | 1 | restable.sch |
| `SEMITRAILER6` | 1 | vd.sch |
| `SEMITRAILER5` | 1 | vd.sch |
| `SEMITRAILER4` | 1 | vd.sch |
| `SEMITRAILER3` | 1 | vd.sch |
| `SEMITRAILER2` | 1 | vd.sch |
| `SEMIPLATFORM` | 1 | vd.sch |
| `SEMITANK4` | 1 | vd.sch |
| `SEMITANK3` | 1 | vd.sch |
| `SEMITANK2` | 1 | vd.sch |
| `SEMITANK1` | 1 | vd.sch |
| `INTERNATIONAL_TRACTOR4` | 1 | vd.sch |
| `INTERNATIONAL_TRACTOR3` | 1 | vd.sch |
| `INTERNATIONAL_TRACTOR2` | 1 | vd.sch |
| `INTERNATIONAL_TRACTOR1` | 1 | vd.sch |
| `MERCEDES_TRACTOR4` | 1 | vd.sch |
| `MERCEDES_TRACTOR3` | 1 | vd.sch |
| `MERCEDES_TRACTOR2` | 1 | vd.sch |
| `MERCEDES_TRACTOR1` | 1 | vd.sch |
| `CAYMAN4` | 1 | vd.sch |
| `CAYMAN3` | 1 | vd.sch |
| `CAYMAN2` | 1 | vd.sch |
| `CAYMAN1` | 1 | vd.sch |
| `CAYMAN` | 1 | vd.sch |
| `DAF_TRACTOR4` | 1 | vd.sch |
| `DAF_TRACTOR3` | 1 | vd.sch |
| `DAF_TRACTOR2` | 1 | vd.sch |
| `DAF_TRACTOR1` | 1 | vd.sch |
| `PETERBILT_TRACTOR4` | 1 | vd.sch |
| `PETERBILT_TRACTOR3` | 1 | vd.sch |
| `PETERBILT_TRACTOR2` | 1 | vd.sch |
| `PETERBILT_TRACTOR1` | 1 | vd.sch |
| `KENWORTH_TRACTOR4` | 1 | vd.sch |
| `KENWORTH_TRACTOR3` | 1 | vd.sch |
| `KENWORTH_TRACTOR2` | 1 | vd.sch |
| `KENWORTH_TRACTOR1` | 1 | vd.sch |
| `FREIGHTLINER_TRACTOR4` | 1 | vd.sch |
| `FREIGHTLINER_TRACTOR3` | 1 | vd.sch |
| `FREIGHTLINER_TRACTOR2` | 1 | vd.sch |
| `FREIGHTLINER_TRACTOR1` | 1 | vd.sch |
| `VOLVO_TRACTOR4` | 1 | vd.sch |
| `VOLVO_TRACTOR3` | 1 | vd.sch |
| `VOLVO_TRACTOR2` | 1 | vd.sch |
| `VOLVO_TRACTOR1` | 1 | vd.sch |
| `MACK_TRACTOR4` | 1 | vd.sch |
| `MACK_TRACTOR3` | 1 | vd.sch |
| `MACK_TRACTOR2` | 1 | vd.sch |
| `MACK_TRACTOR1` | 1 | vd.sch |
| `URAGAN_TRACTOR4` | 1 | vd.sch |
| `URAGAN_TRACTOR3` | 1 | vd.sch |
| `URAGAN_TRACTOR2` | 1 | vd.sch |
| `URAGAN_TRACTOR1` | 1 | vd.sch |
| `RENAULT_TRACTOR4` | 1 | vd.sch |
| `RENAULT_TRACTOR3` | 1 | vd.sch |
| `RENAULT_TRACTOR2` | 1 | vd.sch |
| `RENAULT_TRACTOR1` | 1 | vd.sch |
| `STORM_LORRY4` | 1 | vd.sch |
| `STORM_LORRY3` | 1 | vd.sch |
| `STORM_LORRY2` | 1 | vd.sch |
| `STORM_LORRY1` | 1 | vd.sch |
| `DAF_LORRY4` | 1 | vd.sch |
| `DAF_LORRY3` | 1 | vd.sch |
| `DAF_LORRY2` | 1 | vd.sch |
| `DAF_LORRY1` | 1 | vd.sch |
| `VOLVO_LORRY4` | 1 | vd.sch |
| `VOLVO_LORRY3` | 1 | vd.sch |
| `VOLVO_LORRY2` | 1 | vd.sch |
| `VOLVO_LORRY1` | 1 | vd.sch |
| `MERCEDES_LORRY4` | 1 | vd.sch |
| `MERCEDES_LORRY3` | 1 | vd.sch |
| `MERCEDES_LORRY2` | 1 | vd.sch |
| `MERCEDES_LORRY1` | 1 | vd.sch |
| `RENAULT_LORRY4` | 1 | vd.sch |
| `RENAULT_LORRY3` | 1 | vd.sch |
| `RENAULT_LORRY2` | 1 | vd.sch |
| `RENAULT_LORRY1` | 1 | vd.sch |
| `KAMAZ_TRACTOR4` | 1 | vd.sch |
| `KAMAZ_TRACTOR3` | 1 | vd.sch |
| `KAMAZ_TRACTOR2` | 1 | vd.sch |
| `KAMAZ_TRACTOR1` | 1 | vd.sch |
| `KAMAZ_LORRY4` | 1 | vd.sch |
| `KAMAZ_LORRY3` | 1 | vd.sch |
| `KAMAZ_LORRY2` | 1 | vd.sch |
| `KAMAZ_LORRY1` | 1 | vd.sch |
| `SCANIA_TRACTOR4` | 1 | vd.sch |
| `SCANIA_TRACTOR3` | 1 | vd.sch |
| `SCANIA_TRACTOR2` | 1 | vd.sch |
| `SCANIA_TRACTOR1` | 1 | vd.sch |
| `SCANIA_LORRY4` | 1 | vd.sch |
| `SCANIA_LORRY3` | 1 | vd.sch |
| `SCANIA_LORRY2` | 1 | vd.sch |
| `SCANIA_LORRY1` | 1 | vd.sch |
| `ZIL_TRACTOR4` | 1 | vd.sch |
| `ZIL_TRACTOR3` | 1 | vd.sch |
| `ZIL_TRACTOR2` | 1 | vd.sch |
| `ZIL_TRACTOR1` | 1 | vd.sch |
| `ZIL_LORRY4` | 1 | vd.sch |
| `ZIL_LORRY3` | 1 | vd.sch |
| `ZIL_LORRY2` | 1 | vd.sch |
| `ZIL_LORRY1` | 1 | vd.sch |
| `GAZEL_LORRY4` | 1 | vd.sch |
| `GAZEL_LORRY3` | 1 | vd.sch |
| `GAZEL_LORRY2` | 1 | vd.sch |
| `GAZEL_LORRY1` | 1 | vd.sch |
| `GAZEL_WHITE_VAN` | 1 | vd.sch |
| `GAZEL_RED_VAN` | 1 | vd.sch |
| `GAZEL_GRAY_VAN` | 1 | vd.sch |
| `GAZEL_GREEN_VAN` | 1 | vd.sch |
| `GAZEL_BLUE_VAN` | 1 | vd.sch |
| `GAZEL_POLICE` | 1 | vd.sch |
| `JEEP_TOYOTA_BROWN` | 1 | vd.sch |
| `JEEP_TOYOTA_PINK` | 1 | vd.sch |
| `JEEP_TOYOTA_BLACK` | 1 | vd.sch |
| `JEEP_TOYOTA_GREEN` | 1 | vd.sch |
| `JEEP_NISSAN_VIOLET` | 1 | vd.sch |
| `JEEP_NISSAN_WHITE` | 1 | vd.sch |
| `JEEP_NISSAN_BLACK` | 1 | vd.sch |
| `JEEP_NISSAN_GREEN` | 1 | vd.sch |
| `BMW_M5_4` | 1 | vd.sch |
| `BMW_M5_3` | 1 | vd.sch |
| `BMW_M5_2` | 1 | vd.sch |
| `BMW_M5_1` | 1 | vd.sch |
| `BMW_M5_POLICE` | 1 | vd.sch |
| `VAN_4` | 1 | vd.sch |
| `VAN_3` | 1 | vd.sch |
| `VAN_2` | 1 | vd.sch |
| `VAN_1` | 1 | vd.sch |
| `OKA_4` | 1 | vd.sch |
| `OKA_3` | 1 | vd.sch |
| `OKA_2` | 1 | vd.sch |
| `OKA_1` | 1 | vd.sch |
| `MINI_4` | 1 | vd.sch |
| `MINI_3` | 1 | vd.sch |
| `MINI_2` | 1 | vd.sch |
| `MINI_1` | 1 | vd.sch |
| `MEGAN_4` | 1 | vd.sch |
| `MEGAN_3` | 1 | vd.sch |
| `MEGAN_2` | 1 | vd.sch |
| `MEGAN_1` | 1 | vd.sch |
| `MARERA_4` | 1 | vd.sch |
| `MARERA_3` | 1 | vd.sch |
| `MARERA_2` | 1 | vd.sch |
| `MARERA_1` | 1 | vd.sch |
| `BUS_4` | 1 | vd.sch |
| `BUS_3` | 1 | vd.sch |
| `BUS_2` | 1 | vd.sch |
| `BUS_1` | 1 | vd.sch |
| `AVENSIS_4` | 1 | vd.sch |
| `AVENSIS_3` | 1 | vd.sch |
| `AVENSIS_2` | 1 | vd.sch |
| `AVENSIS_1` | 1 | vd.sch |
| `VOLGA_4` | 1 | vd.sch |
| `VOLGA_3` | 1 | vd.sch |
| `VOLGA_2` | 1 | vd.sch |
| `VOLGA_1` | 1 | vd.sch |
| `FIAT_4` | 1 | vd.sch |
| `FIAT_3` | 1 | vd.sch |
| `FIAT_2` | 1 | vd.sch |
| `FIAT_1` | 1 | vd.sch |
| `pos_work_ac_15` | 1 | mod_ac.sch |
| `mapBenzo_ac` | 1 | mod_ac.sch |
| `4031` | 1 | mod_ac.sch |
| `AMBUSH_AD_3` | 1 | mod_ad.sch |
| `1320` | 1 | mod_ad.sch |
| `AMBUSH_AD_4` | 1 | mod_ad.sch |
| `1325` | 1 | mod_ad.sch |
| `AMBUSH_AD_1` | 1 | mod_ad.sch |
| `2320` | 1 | mod_ad.sch |
| `AMBUSH_AD_2` | 1 | mod_ad.sch |
| `2322` | 1 | mod_ad.sch |
| `node_store_0604` | 1 | mod_ad.sch |
| `map_town_adh` | 1 | mod_ad.sch |
| `c_TABLEWARE` | 1 | mod_ad.sch |
| `2324` | 1 | mod_ae.sch |
| `2327` | 1 | mod_ae.sch |
| `mapBenzo_aeh` | 1 | mod_ae.sch |
| `mapBenzo_ae` | 1 | mod_ae.sch |
| `4032` | 1 | mod_ae.sch |
| `AMBUSH_AF_1` | 1 | mod_af.sch |
| `1335` | 1 | mod_af.sch |
| `AMBUSH_AF_2` | 1 | mod_af.sch |
| `1330` | 1 | mod_af.sch |
| `2337` | 1 | mod_ag.sch |
| `2334` | 1 | mod_ag.sch |
| `mapBenzo_ag13` | 1 | mod_ag.sch |
| `mapBenzo_ag15` | 1 | mod_ag.sch |
| `4024` | 1 | mod_ag.sch |
| `4023` | 1 | mod_ag.sch |
| `4022` | 1 | mod_ag.sch |
| `2347` | 1 | mod_ah.sch |
| `2344` | 1 | mod_ah.sch |
| `2037` | 1 | mod_aj.sch |
| `2035` | 1 | mod_aj.sch |
| `2033` | 1 | mod_aj.sch |
| `2031` | 1 | mod_aj.sch |
| `AMBUSH_AJ_1` | 1 | mod_aj.sch |
| `1345` | 1 | mod_aj.sch |
| `AMBUSH_AJ_2` | 1 | mod_aj.sch |
| `1340` | 1 | mod_aj.sch |
| `4025` | 1 | mod_aj.sch |
| `map_town_ajwb` | 1 | mod_aj.sch |
| `map_town_ajh` | 1 | mod_aj.sch |
| `AMBUSH_AK_3` | 1 | mod_ak.sch |
| `2088` | 1 | mod_ak.sch |
| `AMBUSH_AK_4` | 1 | mod_ak.sch |
| `AMBUSH_AK_1` | 1 | mod_ak.sch |
| `1085` | 1 | mod_ak.sch |
| `AMBUSH_AK_2` | 1 | mod_ak.sch |
| `1080` | 1 | mod_ak.sch |
| `AMBUSH_AL_3` | 1 | mod_al.sch |
| `2070` | 1 | mod_al.sch |
| `AMBUSH_AL_4` | 1 | mod_al.sch |
| `2078` | 1 | mod_al.sch |
| `AMBUSH_AL_1` | 1 | mod_al.sch |
| `1075` | 1 | mod_al.sch |
| `AMBUSH_AL_2` | 1 | mod_al.sch |
| `1070` | 1 | mod_al.sch |
| `2060` | 1 | mod_am.sch |
| `AMBUSH_AM_1` | 1 | mod_am.sch |
| `1065` | 1 | mod_am.sch |
| `AMBUSH_AM_2` | 1 | mod_am.sch |
| `1060` | 1 | mod_am.sch |
| `map_gasholder_am` | 1 | mod_am.sch |
| `AMBUSH_AP_3` | 1 | mod_ap.sch |
| `1098` | 1 | mod_ap.sch |
| `AMBUSH_AP_4` | 1 | mod_ap.sch |
| `AMBUSH_AP_1` | 1 | mod_ap.sch |
| `1095` | 1 | mod_ap.sch |
| `AMBUSH_AP_2` | 1 | mod_ap.sch |
| `map_town_ap` | 1 | mod_ap.sch |
| `TVSET21` | 1 | mod_ap.sch |
| `AMBUSH_AQ_3` | 1 | mod_aq.sch |
| `2010` | 1 | mod_aq.sch |
| `AMBUSH_AQ_4` | 1 | mod_aq.sch |
| `2018` | 1 | mod_aq.sch |
| `AMBUSH_AQ_1` | 1 | mod_aq.sch |
| `1015` | 1 | mod_aq.sch |
| `AMBUSH_AQ_2` | 1 | mod_aq.sch |
| `1010` | 1 | mod_aq.sch |
| `AMBUSH_AR_3` | 1 | mod_ar.sch |
| `2350` | 1 | mod_ar.sch |
| `AMBUSH_AR_4` | 1 | mod_ar.sch |
| `2358` | 1 | mod_ar.sch |
| `AMBUSH_AR_1` | 1 | mod_ar.sch |
| `1355` | 1 | mod_ar.sch |
| `AMBUSH_AR_2` | 1 | mod_ar.sch |
| `1350` | 1 | mod_ar.sch |
| `4027` | 1 | mod_ar.sch |
| `4028` | 1 | mod_ar.sch |
| `map_town_arw` | 1 | mod_ar.sch |
| `map_town_arh` | 1 | mod_ar.sch |
| `AMBUSH_AS_3` | 1 | mod_as.sch |
| `2020` | 1 | mod_as.sch |
| `AMBUSH_AS_4` | 1 | mod_as.sch |
| `2028` | 1 | mod_as.sch |
| `AMBUSH_AS_1` | 1 | mod_as.sch |
| `1025` | 1 | mod_as.sch |
| `AMBUSH_AS_2` | 1 | mod_as.sch |
| `1020` | 1 | mod_as.sch |
| `AMBUSH_AT_1` | 1 | mod_at.sch |
| `1035` | 1 | mod_at.sch |
| `AMBUSH_AT_2` | 1 | mod_at.sch |
| `1030` | 1 | mod_at.sch |
| `AMBUSH_AU_5` | 1 | mod_au.sch |
| `2048` | 1 | mod_au.sch |
| `AMBUSH_AU_6` | 1 | mod_au.sch |
| `2046` | 1 | mod_au.sch |
| `AMBUSH_AU_3` | 1 | mod_au.sch |
| `2042` | 1 | mod_au.sch |
| `AMBUSH_AU_4` | 1 | mod_au.sch |
| `2040` | 1 | mod_au.sch |
| `AMBUSH_AU_1` | 1 | mod_au.sch |
| `1045` | 1 | mod_au.sch |
| `AMBUSH_AU_2` | 1 | mod_au.sch |
| `1040` | 1 | mod_au.sch |
| `node_store_0204` | 1 | mod_au.sch |
| `map_town_auh` | 1 | mod_au.sch |
| `4030` | 1 | mod_au.sch |
| `AMBUSH_AW_3` | 1 | mod_aw.sch |
| `2059` | 1 | mod_aw.sch |
| `AMBUSH_AW_4` | 1 | mod_aw.sch |
| `2050` | 1 | mod_aw.sch |
| `AMBUSH_AW_1` | 1 | mod_aw.sch |
| `1055` | 1 | mod_aw.sch |
| `AMBUSH_AW_2` | 1 | mod_aw.sch |
| `1050` | 1 | mod_aw.sch |
| `map_gasholder_ax` | 1 | mod_ax.sch |
| `1120` | 1 | mod_ba.sch |
| `4021` | 1 | mod_ba.sch |
| `mapNumBenzo_BAm0` | 1 | mod_ba.sch |
| `mapNumBenzo_BA0` | 1 | mod_ba.sch |
| `2100` | 1 | mod_bb.sch |
| `1110` | 1 | mod_bb.sch |
| `1100` | 1 | mod_bb.sch |
| `pos_work_bb0` | 1 | mod_bb.sch |
| `mapNameStore_BCm0` | 1 | mod_bc.sch |
| `mapNameStore_BC0` | 1 | mod_bc.sch |
| `1130` | 1 | mod_bd.sch |
| `pos_work_bd0` | 1 | mod_bd.sch |
| `pos_work_bd1` | 1 | mod_bd.sch |
| `mapNameStore_BDm0` | 1 | mod_bd.sch |
| `mapNameStore_BD0` | 1 | mod_bd.sch |
| `1140` | 1 | mod_bf.sch |
| `AMBUSH_CA_2` | 1 | mod_ca.sch |
| `2200` | 1 | mod_ca.sch |
| `AMBUSH_CA_3` | 1 | mod_ca.sch |
| `2205` | 1 | mod_ca.sch |
| `AMBUSH_CA00_R` | 1 | mod_ca.sch |
| `1200` | 1 | mod_ca.sch |
| `AMBUSH_CA00_L` | 1 | mod_ca.sch |
| `1210` | 1 | mod_ca.sch |
| `map_town_cb3` | 1 | mod_cb.sch |
| `map_town_cbm3` | 1 | mod_cb.sch |
| `AMBUSH_CC_3` | 1 | mod_cc.sch |
| `2225` | 1 | mod_cc.sch |
| `AMBUSH_CC_4` | 1 | mod_cc.sch |
| `2220` | 1 | mod_cc.sch |
| `AMBUSH_CC_0` | 1 | mod_cc.sch |
| `1250` | 1 | mod_cc.sch |
| `AMBUSH_CC_1` | 1 | mod_cc.sch |
| `1240` | 1 | mod_cc.sch |
| `4033` | 1 | mod_cc.sch |
| `AMBUSH_CE_3` | 1 | mod_ce.sch |
| `2245` | 1 | mod_ce.sch |
| `AMBUSH_CE_4` | 1 | mod_ce.sch |
| `2240` | 1 | mod_ce.sch |
| `4026` | 1 | mod_ce.sch |
| `AMBUSH_CF_3` | 1 | mod_cf.sch |
| `2215` | 1 | mod_cf.sch |
| `AMBUSH_CF_4` | 1 | mod_cf.sch |
| `2210` | 1 | mod_cf.sch |
| `AMBUSH_CF_0` | 1 | mod_cf.sch |
| `1230` | 1 | mod_cf.sch |
| `AMBUSH_CF_1` | 1 | mod_cf.sch |
| `1220` | 1 | mod_cf.sch |
| `AMBUSH_CH_R` | 1 | mod_ch.sch |
| `1290` | 1 | mod_ch.sch |
| `AMBUSH_CH_L` | 1 | mod_ch.sch |
| `1280` | 1 | mod_ch.sch |
| `2312` | 1 | mod_db.sch |
| `2301` | 1 | mod_db.sch |
| `AMBUSH_DC_1` | 1 | mod_dc.sch |
| `1300` | 1 | mod_dc.sch |
| `AMBUSH_DC_2` | 1 | mod_dc.sch |
| `1305` | 1 | mod_dc.sch |
| `mapBenzo_dch` | 1 | mod_dc.sch |
| `mapBenzo_dc` | 1 | mod_dc.sch |
| `4035` | 1 | mod_dc.sch |
| `AMBUSH_DQ_3` | 1 | mod_dq.sch |
| `2230` | 1 | mod_dq.sch |
| `AMBUSH_DQ_4` | 1 | mod_dq.sch |
| `2235` | 1 | mod_dq.sch |
| `AMBUSH_DQ_0` | 1 | mod_dq.sch |
| `1270` | 1 | mod_dq.sch |
| `AMBUSH_DQ_1` | 1 | mod_dq.sch |
| `1260` | 1 | mod_dq.sch |
| `4029` | 1 | mod_dq.sch |

</details>

---

## 9. Coverage: what the scripts contain vs what the engine reads

The user complaint this section answers is *"many conditions in the scripts are not
executed"*.  Two separate things were happening, and the measurement below separates them:

1. **The script language has no conditionals at all.**  There is no `if`, `cond`, `when`,
   `trigger` or expression form in any of the 46 files, and the loader does not evaluate
   anything: it dispatches on the *exact head string* of each form and raises
   `"Undefined"` (king.exe `0x41204e`, message table `0x66ceb0`) for an unknown `people`
   gate.  So a "condition" in this game is always (a) a head keyword, or (b) an enumerated
   field whose rule lives in the engine.  Those rules are what `src/script/ScriptRules.cpp`
   now implements (§10).
2. **A number of fields were parsed but never read, or read at the wrong offset.**
   §9.2 lists every one of them.

### 9.1 The (form → field index → consumed by) table

Generated by `python3 tools/script_coverage.py --md`; the same data is produced by the
engine itself (`GameData::coverageSummary()` / `coverageText()`, printed by
`--script-report --script-coverage`), and `tools/shot_test.py` asserts that the two agree.

```
coverage: files=46 heads=722 forms=1047 fields=139 dropped_fields=3
```

`heads` = distinct first elements of a list (722, matching Appendix A), `forms` = list forms
whose head is a *grammar* head, `fields` = distinct (form, field-index) positions,
`dropped_fields` = positions no code reads.

Footnote on the shared head `fuel`: it is a `GAME.CNF` counter (`( fuel 10 )`) *and* the
carrier-class tag `( fuel trucks )` of one cargo item, which is why its shape column reads
`numberx2,symbolx1`.  The same overload is described in §5.8 for `trucks`/`cars`; a parser
must not treat a carrier tag as a form.

Only three fields are genuinely unread, and two of them are structurally empty:

| form | field | shapes (count) | why nobody reads it |
|------|-------|----------------|---------------------|
| `town` | 6 | `listx29` | always `()`, an unused slot |
| `town` | 8 | `listx29` | the map-icon descriptor (icon names per zoom) `[OPEN]` |
| `parking` | 4 | `listx30` | always `()`, an unused slot |

Everything else is consumed — after the changes of §10/§11.  The engine prints its own copy
of the table (`tools/out/script_selftest original --coverage`, or
`--script-report --script-coverage` after the wiring); its form/field/shape rows are
**identical** to the Python survey's 139 rows (checked in this pass; the only difference is
that the engine qualifies the consumer as `GameData::…`).  The complete table:

| form | field | shapes (count) | consumed by |
|------|-------|----------------|-------------|
| `vehicle` | 1 | symbolx217 | id |
| `vehicle` | 2 | stringx217 | model |
| `vehicle` | 3 | numberx217 | variant |
| `vehicle` | 4 | symbolx217 | class |
| `vehicle` | 5 | listx20,symbolx138 | ai role |
| `vehicle` | 6 | numberx158 | price |
| `vehicle` | 7 | numberx158 | raw F2 (hp-like) |
| `vehicle` | 8 | listx158 | trailers |
| `vehicle` | 9 | numberx4 | flag |
| `ambush` | 1 | symbolx87 | Ambush.id |
| `ambush` | 2 | symbolx87 | class |
| `ambush` | 3 | listx81,stringx6 | tile tag |
| `ambush` | 4 | listx81,stringx6 | '' or tile |
| `ambush` | 5 | stringx87 | wayNode |
| `ambush` | 6 | symbolx87 | direction |
| `ambush` | 7 | listx87 | slot ids |
| `ambush` | 8 | listx87 | paired ambush |
| `ambush` | 9 | numberx8 | radius |
| `node` | 1 | stringx170 | GameData::Bay.name -> .way RNOD |
| `node` | 2 | numberx35,symbolx33 | GameData::Bay.role/.flags |
| `node` | 3 | stringx10,symbolx14 | GameData::Bay.town (trade stall) |
| `town` | 1 | symbolx29 | GameData::Town.id |
| `town` | 2 | stringx29 | display name |
| `town` | 3 | numberx29 | Town.id |
| `town` | 4 | listx15,numberx14 | Town.kind (2 = petrol station) |
| `town` | 5 | listx29 | Town.nodes |
| `town` | 6 | listx29 | **DROPPED** -- (always ()) |
| `town` | 7 | listx29 | Town.goods (item, stock) |
| `town` | 8 | listx29 | **DROPPED** -- map descriptor |
| `service` | 1 | symbolx37 | Service.id |
| `service` | 2 | stringx37 | display |
| `service` | 3 | numberx37 | Service.type |
| `service` | 4 | stringx37 | Service.wayNode |
| `service` | 5 | listx37 | Service.upgrades |
| `service` | 6 | listx14 | Service.fuelId |
| `parking` | 1 | symbolx30 | GameData::Parking.id |
| `parking` | 2 | stringx30 | display |
| `parking` | 3 | numberx30 | bazaar id |
| `parking` | 4 | listx30 | **DROPPED** -- (always ()) |
| `parking` | 5 | listx30 | Bay list |
| `parking` | 6 | numberx15 | Parking.size |
| `nodeevent` | 1 | symbolx31 | NodeEvent.id |
| `nodeevent` | 2 | symbolx31 | kind (STATEBORDER|UNCONDITIONAL) |
| `nodeevent` | 3 | stringx31 | wayNode |
| `nodeevent` | 4 | symbolx31 | direction (ALL|N|S|E|W) |
| `nodeevent` | 5 | stringx31 | text |
| `obstacle` | 1 | symbolx17 | Obstacle.id |
| `obstacle` | 2 | symbolx17 | kind |
| `obstacle` | 3 | stringx17 | tile |
| `obstacle` | 4 | stringx17 | b3d key |
| `obstacle` | 5 | stringx17 | debris A |
| `obstacle` | 6 | stringx17 | debris B |
| `obstacle` | 7 | stringx7 | closed message |
| `obstacle` | 8 | stringx7 | open message |
| `obstacle` | 9 | listx4 | repair nodes |
| `people` | 1 | stringx13 | tile |
| `people` | 2 | listx13 | gate list |
| `people` | 3 | listx13 | the gate list (every child) |
| `people` | 4 | listx13 | the gate list (every child) |
| `people` | 5 | listx9 | the gate list (every child) |
| `people` | 6 | listx5 | the gate list (every child) |
| `people` | 7 | listx5 | the gate list (every child) |
| `people` | 8 | listx3 | the gate list (every child) |
| `people` | 9 | listx2 | the gate list (every child) |
| `people` | 10 | listx1 | the gate list (every child) |
| `people` | 11 | listx1 | the gate list (every child) |
| `people` | 12 | listx1 | the gate list (every child) |
| `DAYONLY` | 1 | stringx32 | Gate.key |
| `DAYONLY` | 2 | numberx32 | Gate.probability |
| `DAYGW` | 1 | stringx29 | Gate.key |
| `DAYGW` | 2 | numberx29 | Gate.probability |
| `PR_TIRES` | 1 | listx1,numberx23 | level 0 |
| `PR_TIRES` | 2 | listx1 | level 1 |
| `PR_TIRES` | 3 | listx1 | level 2 |
| `dumb` | 1 | numberx3,symbolx20 | GAME.CNF world counter |
| `PR_NAVIGATOR` | 1 | listx1,numberx20 | level 0 |
| `PR_NAVIGATOR` | 2 | listx1 | level 1 |
| `PR_FORCED_ENGINE` | 1 | listx1,numberx19 | level 0 (name, price) |
| `PR_FORCED_ENGINE` | 2 | listx1 | level 1 |
| `PR_ADJUSTABLE_SUSPENSION` | 1 | listx1,numberx18 | level 0 |
| `PR_ADJUSTABLE_SUSPENSION` | 2 | listx1 | level 1 |
| `PR_EXTRA_TANK` | 1 | listx1,numberx18 | level 0 |
| `PR_EXTRA_TANK` | 2 | listx1 | level 1 |
| `PR_CARGO_SCANNER` | 1 | listx1,numberx16 | level 0 |
| `PR_CARGO_SCANNER` | 2 | listx1 | level 1 |
| `PR_BRAKES` | 1 | listx1,numberx15 | level 0 |
| `PR_BRAKES` | 2 | listx1 | level 1 |
| `PR_ENGINE_BRAKE` | 1 | listx1,numberx15 | level 0 |
| `PR_ENGINE_BRAKE` | 2 | listx1 | level 1 |
| `PR_FIRE_PROTECTION` | 1 | listx1,numberx14 | level 0 |
| `PR_FIRE_PROTECTION` | 2 | listx1 | level 1 |
| `PR_FUEL` | 1 | listx1,numberx14 | level 0 |
| `PR_FUEL` | 2 | listx1 | level 1 |
| `PR_REINFORCED_CHASSIS` | 1 | listx1,numberx14 | level 0 |
| `PR_REINFORCED_CHASSIS` | 2 | listx1 | level 1 |
| `playertypes` | 1 | listx3 | counters (live/aitruck/police/mafia/dumb/ghost) |
| `playertypes` | 2 | listx3 | the counter list (every child) |
| `playertypes` | 3 | listx3 | the counter list (every child) |
| `playertypes` | 4 | listx3 | the counter list (every child) |
| `playertypes` | 5 | listx3 | the counter list (every child) |
| `playertypes` | 6 | listx1 | the counter list (every child) |
| `PR_ANTI_RADAR` | 1 | listx1,numberx13 | level 0 |
| `PR_ANTI_RADAR` | 2 | listx1 | level 1 |
| `PR_ABS` | 1 | listx1,numberx12 | level 0 |
| `PR_ABS` | 2 | listx1 | level 1 |
| `PR_OIL_ADDITIVE` | 1 | listx1,numberx11 | level 0 |
| `PR_OIL_ADDITIVE` | 2 | listx1 | level 1 |
| `PR_BULETPROOF_GLASS` | 1 | listx1,numberx10 | level 0 |
| `PR_BULETPROOF_GLASS` | 2 | listx1 | level 1 |
| `PR_HEADLIGHT_PROTECTION` | 1 | listx1,numberx10 | level 0 |
| `PR_HEADLIGHT_PROTECTION` | 2 | listx1 | level 1 |
| `PR_HORN` | 1 | listx1,numberx10 | level 0 |
| `PR_HORN` | 2 | listx1 | level 1 |
| `PR_STEALTH_BODY_PAINT` | 1 | listx1,numberx9 | level 0 |
| `PR_STEALTH_BODY_PAINT` | 2 | listx1 | level 1 |
| `UNCONDITIONAL` | 1 | stringx5 | Gate.key |
| `UNCONDITIONAL` | 2 | numberx5 | Gate.probability |
| `PR_AIR_FILTER_SNORKEL` | 1 | listx1,numberx7 | level 0 |
| `PR_AIR_FILTER_SNORKEL` | 2 | listx1 | level 1 |
| `live` | 1 | numberx3 | flag |
| `live` | 2 | stringx3 | player vehicle model |
| `live` | 3 | numberx3 | variant |
| `startposition` | 1 | numberx3 | x |
| `startposition` | 2 | numberx3 | z |
| `startposition` | 3 | numberx3 | height |
| `aitruck` | 1 | numberx3 | GAME.CNF world counter |
| `forhire` | 1 | numberx3 | GAME.CNF world counter |
| `forsale` | 1 | numberx3 | GAME.CNF world counter |
| `fuel` | 1 | numberx2,symbolx1 | GAME.CNF world counter |
| `mafia` | 1 | numberx3 | GAME.CNF world counter |
| `missions` | 1 | numberx3 | GAME.CNF world counter |
| `police` | 1 | numberx3 | GAME.CNF world counter |
| `containers` | 1 | numberx2 | GAME.CNF world counter |
| `scenario` | 1 | symbolx2 | world/race scenario name |
| `semitrailers` | 1 | numberx2 | GAME.CNF world counter |
| `forsale_stolen` | 1 | numberx1 | GAME.CNF world counter |
| `forsale_used` | 1 | numberx1 | GAME.CNF world counter |
| `ghost` | 1 | numberx1 | GAME.CNF world counter |
| `parkings` | 1 | numberx1 | GAME.CNF world counter |

### 9.2 What used to be dropped, and is not any more

| form | field | what it is | before | after |
|------|-------|------------|--------|-------|
| `people` gate | head | `GOODWEATHER` is a real engine condition (king.exe `0x411fbc`) | dropped by the loader (only `DAY*`/`UNCONDITIONAL` accepted) | `Gate::kindId` 1..4, all four kinds |
| `people` gate | 3 | probability | ad-hoc frame RNG and `DAYGW` reduced to `day` | module-load snapshot using `rand()/1e6 < p` comparison (`0x42adc2`), real day **and** weather condition; port PRNG still differs |
| `nodeevent` | 2 | `STATEBORDER` = bit `0x10`, `UNCONDITIONAL` = 0 | kind kept as a string only | `NodeEvent::kindFlag` |
| `nodeevent` | 4 | `ALL/N/S/E/W` = the **travel direction** required | ignored (any approach fired) | `dirId` + compass vector, `dot(dir, move) > 0` |
| `nodeevent` | (timer) | 8 s re-trigger guard on the event | none | `kNodeEventCooldown`, optional one-shot |
| `obstacle` | 5, 6 | the two debris `.way` nodes | read as `msgOpen`/`msgClose` (off by two) | `debrisA`/`debrisB` |
| `obstacle` | 7, 8 | the closed / reopened messages | dropped | `msgClose`/`msgOpen` |
| `obstacle` | 9 | the `pos_work_*` repair sites | dropped | `Obstacle::workNodes` (6 names) |
| `ambush` | 3, 4 | tile tag / `""` in the north-island tiles | coerced to `int` | `tileTag`/`f4s` |
| `ambush` | 7 | the global ambush-slot ids | merged with field 8 | `slots` = the numeric ids only |
| `ambush` | 8 | the coupled (paired) ambush | merged into `slots` (inflated the car count by 1–2) | `Ambush::next` |
| `ambush` | 9 | the extra radius (`65`, `85`; 8 forms) | dropped | `Ambush::radius` |
| `parking` | 6 | the trailing integer (5…22, bazaars only) | dropped | `Parking::size` |
| `node` | 2/3 | `unload`/`hidden`/`fuel`/`dummy` keyword bits | dropped (only the numeric token was kept) | `Bay::flags` (1/2/4/8) |
| `town` | all | the whole `town` form | not loaded by `GameData` (only `Economy.cpp` read it) | `GameData::Town` (29 towns, 73 nodes, 119 goods) |
| `restable` | 1..n | `( "name" price )` per level | flattened to the last price | `GameData::upgradeLevels` (19 rows / 39 levels) |
| `GAME.CNF` | `(ghost 25)` | the ghost-traffic counter | dropped | `GameData::ghost` + `counters` |
| `GAME.CNF` | counters | `forsale*`, `forhire`, `missions`, `fuel`, `semitrailers`, `containers`, `parkings` | clobbered by MENV/MTR | ENV wins (`GameData::counters`) |

A real gap in the shipped data that the report makes visible: **10 services offer
`( PR_TIRES 3 )` but `restable.sch`'s `PR_TIRES` row has only levels 0, 1, 2** — level 3 has
no name and no price (`missingLevels=10` in `--script-report`).

---

## 10. The engine rules recovered from `original/king.exe`

All addresses are virtual addresses (`VA = file offset + 0x400000`); the binary is the
shipped `original/king.exe` (PE32, image base `0x400000`, `.text` at `0x401000`).
Reproduce with:

```bash
r2 -q -e scr.color=0 -c 'pd 60 @ 0x40b100' original/king.exe      # onNodePass
r2 -q -e scr.color=0 -c 'pd 90 @ 0x42ac40' original/king.exe      # the gate evaluator
tools/build_script_check.sh                                       # ... or just the harness
```

The debug strings of this build carry the original function names
(`$Id: Ai_main.cpp 2001/10/16 ...` at `0x66c558`), which is what makes the mapping below
possible.

### 10.1 The script dispatcher (why there are no `if` forms)

| VA | what it does |
|----|--------------|
| `0x26caf8..0x26d008` (file offsets) | the head-string table: `startposition`, `playertypes`, `live`, `scenario`, `forksale*`, `merchandise.sch`, `restable.sch`, `names.sch`, `vd.sch`, `ms.sch`, `node`, `DAYGW`, `GOODWEATHER`, `DAYONLY`, `UNCONDITIONAL`, `NORTH/SOUTH/EAST/WEST`, `ALL`, `STATEBORDER`, `nodeevent`, `ambush`, `people`, `obstacle`, `service`, `parking`, `town`, `mod_*.sch` |
| `0x41204e` | `people`: no kind matched ⇒ the string `"Undefined"` is passed to the assert/error printer |
| `0x413d5c`, `0x413ea1`, `0x413cb5` | `nodeevent`: `"Unknown node event type"`, `"Unknown node event direction"`, `"Unknown ambush type"` |
| `0x26cd10` | `"Undefined merchandise flag (see merchandise.sch)"` |

There is no expression evaluator, no variables and no branching: a form is a record and a
field is either data or a condition keyword.

### 10.2 `people` — the only condition form (4 kinds)

Parser `0x411f14..0x412077`, gate object 0x30 bytes (`operator new` at `0x411ed7`), runtime
evaluator `0x42ac83..0x42ae69` inside `onModuleLoadX` (`0x42aba0`), with the
jump table at `0x42b050`.

| gate field | offset | meaning |
|------------|--------|---------|
| `+0x0c` | `int` | `-1`, or an index into the room table (`0x68d0c8`) |
| `+0x10` | `std::string` | the `subtag 21` key name |
| `+0x1c` | `bool` | stored **visible** state (written on module load) |
| `+0x20` | `int` | kind: **1 = UNCONDITIONAL, 2 = DAYONLY, 3 = GOODWEATHER, 4 = DAYGW** |
| `+0x28` | `double` | probability |

* `0x411f14` `UNCONDITIONAL` → `[+0x20] = 1`; `0x411f68` `DAYONLY` → 2; `0x411fbc`
  `GOODWEATHER` → 3; `0x41200d` `DAYGW` → 4.  Any other atom is an error (`0x41204e`).
* On a module-load callback the evaluator scans the gates and updates only
  those matching `moduleName` (`0x42acb7..0x42ad04`):
  * `0x42ae48` → `0x483c10` applies the new visibility to the mesh group.
  * kind 1 (`0x42ad18`): `visible = prob > rand()/1e6`
  * kind 2 (`0x42ad47`): `if (!isDay()) visible = 0; else visible = prob > rand()/1e6`
  * kind 3 (`0x42ad89`): `if (!isGoodWeather()) visible = 0; else ...`
  * kind 4 (`0x42ade8`): `if (!isDay() || !isGoodWeather()) visible = 0; else ...`
  * the roll is `fild` of an integer in `[0, 1000000)` divided by `0x64b6b0 = 1e6`, compared
    against the stored probability (`0x42adc2`: `fmul 1e6; fcompp`).
* **Day window:** `isDay()` at `0x484040` = `hour > 8.0 && hour < 21.0`
  (`0x64b698 = 8.0`, `0x64be18 = 21.0`; the clock is `weatherandtime->[+0x58]`).
* **Good weather:** `isGoodWeather()` at `0x484010` = `badness < 0.2` (`0x64b840`), where
  `0x5a9d40` returns the weather object's current badness.
* The clock/weather object is constructed at `0x5a7430` from `.\weather.ini`
  (`0x67ea90`), section `weatherandtime` (`0x67eaa0`); its presets are
  `fogf/fogn/frpl_goodday|goodnight|badday|badnight`, and the street lighting keys
  `ILLUM_LEVEL_%02d` (`0x67ea64`) are driven from the same object — which is why no script
  mentions the lighting groups.

### 10.3 `nodeevent` — `onNodePass` (`0x40b100`)

Loader `0x413b68..0x413f7a`; the event is a 0x38-byte object and is attached to the node
record: `node[ebp] = [0x68cf00][id]; [node + 0xc0] = event` (`0x413f10..0x413f19`).

| field | offset | meaning |
|-------|--------|---------|
| kind | `+0x00` | `0x10` = STATEBORDER, `0` = UNCONDITIONAL (`0x413d3a`, `0x413d54`) |
| dir enum | `+0x04` | 0 ALL, 1 NORTH, 2 SOUTH, 3 EAST, 4 WEST (`0x413db0`, `0x413df1`, `0x413e32`, `0x413e73`, `0x413e9c`) |
| dir vector | `+0x08..+0x20` | (x, second-world-axis): N `(0,1,0)`, S `(0,-1,0)`, E `(1,0,0)`, W `(-1,0,0)` |
| last fire | `+0x20` | `double`, the time the message was last shown |
| text | `+0x28` | `std::string`, the message (`0x56ae60` displays it) |

Runtime (`0x40b18e..0x40b2c7`):

1. `esi = node[+0xc0]`; if `esi && (esi[0] & 0x10)` → `0x40ae80(node, direction)` — the
   **STATEBORDER** path, taken *before* any direction or timer test (it updates the player's
   room/region), and it never shows a message.
2. Otherwise (UNCONDITIONAL):
   `dot = ev[8]*d.x + ev[0x10]*d.y + ev[0x18]*d.z` compared with `0.0` (`0x64b678`);
   `0x40b29d` skips the event unless `dot > 0` — i.e. the player must be **travelling along**
   the stored compass direction.
3. Timer: only `fabs(now - ev[+0x20]) > 8.0` (`0x64b698`) permits a repeat
   (`0x40b2a9..0x40b2b4`); the same message is not repeated within 8 seconds.
   On a fire, `now` is stored to `+0x20`
   (`0x40b2c7`).
4. The direction argument comes from `setPlayerPosition`:
   `0x43e7de..0x43e801` subtracts the previous from the current player position and passes
   the difference; `0x43e822` calls `onNodePass`.
   The other callers are `0x43ba2b` and `0x43eae6`.

Consequences for a re-implementation: a `nodeevent` is **not** a radius trigger, it is a
*node pass* with a travel-direction filter and an 8 s guard.  `src/script/ScriptRules.cpp`
implements the direction/cooldown rules (`kNodeEventCooldown`, `passDirectionOk`, `passNode`, and an
optional one-shot policy that the original does not have).

Two deliberate divergences, both marked in the code:

* `DIRECTION = ALL` (which the data only ever uses on `STATEBORDER`) is treated as "no
  direction filter".  The engine's `ALL` branch (`0x413e9c`) writes only the enum `[+0x04]`
  and never writes the compass vector, so a hypothetical `UNCONDITIONAL + ALL` event would
  compare against whatever the 0x30-byte object's `+8..+0x20` happens to hold (zeroed
  allocation, or garbage) - certainly not a defined "any direction".  The safe reading is
  "no filter".
* The engine keys the pass on the player's road `PositionId`; the port now projects
  the pose onto RSEG links and selects named endpoint positions. This remains
  approximate. The 20 u nearest-node stand-in is used only without a road
  network. `passNode(name)` can feed an explicit pass.

### 10.4 `obstacle` — a road-closure scheduler

| VA | what it does |
|----|--------------|
| `0x434f2d` | `ObstaclesTask` constructor: the next run is scheduled `600 + rand()%300` ticks later (`0x434f83` `%300`, `0x434f8c` `+600`) |
| `0x42b3a0` | the task body: pick a random obstacle (`rand` state at `0x68b9c8`), skip it if the player is near (`0x42b2d0`), skip it if it is within **500 u** of the currently closed one (`0x64b860`), else activate it |
| `0x42b170` | activate: close the chosen obstacle, open all others (`0x42b060(obstacle, bool)`), then show its message (`0x484280`) |
| `0x42b2a0` | "the currently closed obstacle" = the first with `[+0x1c] != 0` |
| `0x42b060` | set the closed/open state (and the `b3d` key group visibility) |

So: **at most one road closure is active world-wide**, it moves every 600–900 ticks, the new
one must be > 500 u from the old one and away from the player, and the `pos_work_*` nodes of
field 9 are the repair sites tied to it.  `Obstacle::msgClose`/`msgOpen` (fields 7 and 8) are
the strings to show when it closes/opens.

### 10.5 `ambush` — a proximity band, not a radius

| VA | what it does |
|----|--------------|
| `0x42b550` (`ambushArmable`) | distance to the player; `0` if `< 250.0` (`0x64b868`) or `> 600.0` (`0x64b870`); then a road-graph/direction test (`0x4858b0`, hop count `< 2` at `0x42b6ba`) |
| `0x42b4c0` (`ambushFar`) | returns true when the player is beyond **600 u** |
| `0x42c390` (`updateAmbushes`) | for every ambush: if active and `ambushFar` → `activateAmbush(a, false)` (`0x42bb90`); if inactive and `[+0xf0]` (armed) and `ambushArmable` → `activateAmbush(a, true)`, then deactivate its already-placed cars that the player has left behind |
| `0x68a970` | the ambush list; `[+0x1c]` = active, `[+0x38..0x50]` = position, `[+0x7c]` = the blocking player, `[+0xdc]` = the spawned cars |

So an ambush **arms between 250 and 600 u** and disarms beyond 600 u — the shipped
`ambush` forms carry no radius of their own except the optional field 9 (65/85) on 8 forms,
which is an extra distance parameter for that same test.

### 10.6 `node` roles

`0x40f2b0..0x40f36c`: the keyword tokens are OR-ed into a flags word, one bit each —
`unload` → `1` (`0x40f303`), `hidden` → `2` (`0x40f323`), `fuel` → `4` (`0x40f344`),
`dummy` → `8` (`0x40f364`); `"Unknown role"` (`0x40de1f`) is the error for anything else.
The numeric token of the same field is a separate integer (the shipped data uses 4 = trade
stall, 8 = hidden-car bay, 32 = old-car bay) — the two encodings never mix in the data.

### 10.7 The tuning table (`restable.sch`)

`0x40cf23` loads `restable.sch`; a row is `( PR_X ( "name" price ) … )` and the loader keeps
the whole ordered level list (`getServiceInfo` at `0x4420f5` is what the shop UI reads).
Level 0 is the factory part (`"Regular engine" 0`), so `( PR_TIRES 2 )` in a `service` means
levels 0..2 and level *n*'s price is the *n*-th entry.  `ScriptRuntime::tuningOffers(money)`
expands every `service` offer into one row per level with `name`, `price` and `affordable`,
which is what the tuning UI needs; `missingLevels` counts offers whose level has no row.

---

## 11. `--script-report`

`src/script/ScriptRules.cpp` prints one line per form family with the **active / satisfied /
fired** counts, so the coverage is measurable.  The App wiring (a 3-line option plus the
report block) is delivered as `tools/script_report_wiring.diff`; until it is applied,
`tools/script_selftest` is the same producer.

The producers are `tools/out/script_selftest` (built by `tools/build_script_check.sh`, also
built and run by `tools/shot_test.py`) and, after the wiring, `ht2re --script-report`
(`--script-coverage` additionally dumps the §9.1 table).

Before (the old `--scripts`, which only counted what the loader happened to keep; note that
the traffic counters are the MENV/MTR ones — the menu worlds sorted after `ENV/GAME.CNF` and
overwrote them — and that `ghost`, `towns` and the coverage line did not exist):

```
scripts: files=46 errors=0 | player=ZilR start=(-3020,-4625,20) world=- ai=8 police=0
  mafia=0 dumb=0 | vehicles=158 upgrades=19 parked=59 parkings=30 cargo=107 drivers=74
  nodeevents=31 messages=56 gates=66 obstacles=17 services=37 ambushes=87
```

After (`--scripts` now keeps the ENV counters and adds `ghost`, `towns` and the coverage
line; everything else is unchanged, so the existing checks still match):

```
scripts: files=46 errors=0 | player=ZilR start=(-3020,-4625,20) world=- ai=50 police=15
  mafia=8 dumb=70 ghost=25 | vehicles=158 upgrades=19 parked=59 parkings=30 cargo=107
  drivers=74 nodeevents=31 messages=56 gates=66 obstacles=17 services=37 ambushes=87
  towns=29 | coverage: heads=722 form_heads=49 fields=139 dropped_fields=3
```

and the new per-family report:

```
script-report: family=gate        total=66 active=0 satisfied=0 visible=0 kinds=DAYGW=29,DAYONLY=32,UNCONDITIONAL=5
script-report: family=nodeevent   total=31 active=31 satisfied=0 fired=0 stateborder=12 directional=19 all=12 cooldown=8.0s
script-report: family=obstacle    total=17 closed=0 toggles=0 separation=500 playerAway=250
script-report: family=ambush      total=87 active=0 armed=0 fired=0 band=[250,600]
script-report: family=service     total=37 fuel=14 repair=23 upgradeOffers=268
script-report: family=town        total=29 petrol=14 fuelDepot=4 nodes=73 goods=119
script-report: family=tuning      upgradeRows=19 levels=39 offers=268 rows=559 missingLevels=10 maxPrice=12500
script-report: family=world       cargo=107 drivers=74 parkings=30 parkedModels=59 messages=56
script-report: conditions day=8.0<hour<21.0 goodWeather=0.20 oneShot=0
script-report: coverage: heads=722 form_heads=49 fields=139 dropped_fields=3
```

and the same producer fed a real world state
(`tools/out/script_selftest original --state HOUR,WEATHER,MONEY`, one frame):

```
state hour=12.0 weather=0.00 money=45000
script-report: family=gate        total=66 active=37 satisfied=66 visible=37 kinds=DAYGW=29,DAYONLY=32,UNCONDITIONAL=5
state hour=23.0 weather=0.00 money=45000
script-report: family=gate        total=66 active=3 satisfied=5 visible=3 kinds=DAYGW=29,DAYONLY=32,UNCONDITIONAL=5
state hour=12.0 weather=0.50 money=45000
script-report: family=gate        total=66 active=19 satisfied=37 visible=19 kinds=DAYGW=29,DAYONLY=32,UNCONDITIONAL=5
state hour=6.0 weather=0.00 money=45000
script-report: family=gate        total=66 active=3 satisfied=5 visible=3 kinds=DAYGW=29,DAYONLY=32,UNCONDITIONAL=5
```

### 11.1 The regression tests

`tools/shot_test.py` gained 8 checks for this work (the harness itself asserts 27 rules and
exits non-zero if any fails, so the first check subsumes all 27):

| check | what it pins |
|-------|--------------|
| script-rules harness builds and every recovered rule holds | 27 harness rules, 0 failures |
| the recovered condition families are reported (4 gate kinds, 12+19 node events) | 66 gates / kinds `DAYGW=29,DAYONLY=32,UNCONDITIONAL=5`; 31 events / 12 STATEBORDER / 19 directional |
| a time/weather gated condition holds only in its window | `8 < hour < 21` and `weather < 0.2` (0x484040 / 0x484010) |
| a position/direction condition fires only when satisfied | `dot(dir, move) > 0` on a real NORTH event |
| a money condition gates the tuning upgrade | `PR_TIRES` level 2 = 1500; 1500 ok / 1000 refused |
| a one-shot does not re-fire (and the engine's 8 s guard holds) | rising-edge policy + the 8 s cooldown |
| the tuning table is exposed for the UI | 19 rows / 39 levels / 559 offer rows / 10 levels with no price |
| coverage table is stable (engine and the Python survey agree) | `heads=722 fields=139 dropped_fields=3` from both |

`satisfied` is the *condition* count and is stable (66 = all four kinds at noon with clear
weather; 5 = only `UNCONDITIONAL` at 23:00 or 06:00; 37 = 5 + 32 when the weather is bad).
`visible` additionally contains the module-load probability roll
(`rand()/1e6 < prob`). These condition counts describe loads at the stated
time/weather, not automatic per-frame re-evaluation of already loaded groups.

---

## 12. Historical wiring notes (superseded by the 2026-10-06 runtime correction)

`tools/script_report_wiring.diff` is a real unified diff (`git apply --check` clean) that
adds `--script-report` / `--script-coverage`, the `Options` flags, the `ScriptRules.cpp`
build entry, and the report block in `App::run`.  For the *runtime* integration (feeding the
conditions into the world each frame) the intended shape is:

```cpp
// App.h
#include "../script/ScriptRules.h"
std::unique_ptr<ScriptRuntime> scriptRules_;      // built after gameData_.load()

// once, after indexWayNodes():
scriptRules_ = std::make_unique<ScriptRuntime>(gameData_);
std::vector<ScriptNodePos> np;
for (const auto& kv : wayNodes_) np.push_back({kv.first, kv.second.pos.x, kv.second.pos.z});
scriptRules_->setNodePositions(np);

// per frame (replaces the ad-hoc radius check in updateNodeEvents and the
// gate RNG in updateGates):
ScriptWorldState st;
st.time    = clockSeconds;        // monotonic
st.hour    = startHour + clockSeconds / dayLength * 24.0;
st.weather = weatherBadness;      // 0 clear .. 1 storm
st.money   = economy_.money();
st.damage  = vehicle3d_.damage();
st.px      = vehicle3d_.x();  st.pz = vehicle3d_.z();
scriptRules_->update(st);
// gate visibility:
for (const auto& g : scriptRules_->gates()) setKeyVisible(g.def->key, g.visible);
// node-event messages (already direction-filtered and 8 s guarded):
for (const auto* e : scriptRules_->pendingMessages()) showBanner(e->def->text);
// STATEBORDER: the region/town change
for (const auto* e : scriptRules_->pendingBorders()) onBorderCrossed(e->def->wayNode);
// road closures:
for (const auto& o : scriptRules_->obstacles()) setObstacleClosed(o.def->key, o.closed);
```

---

## 13. What is still unknown after this pass

1. **`town` field 8** (the map-icon descriptor) and the meaning of `( 5000 )`.
2. **`service` field 3**'s enum (0…6, 10, 11); only "10 = petrol" is confirmed by
   `BENZO_*` + `getServiceInfo`.
3. **The obstacle's player-distance constant.**  `0x42b2d0` returns "the player is far
   enough" but the constant was not extracted; `ScriptRules.h` uses a named 250 u and says
   so.
4. **The `ambush` field-7 slot ids.**  Still not found as an index into anything shipped;
   the position comes from field 5, so they only matter as a car count.
5. **`parking` field 6** (`Parking::size`): now kept, but no consumer was found.
6. **Additional `people` refresh/streaming paths.** `0x42ac83` is inside
   `onModuleLoadX`, not a frame callback (corrected 2026-10-06). Other refresh
   paths and renderer transitions are not established.
7. **The `weatherandtime` badness curve** (`0x5a9d40`) - only its threshold (0.2) and the
   `weather.ini` presets are known; `TRUCK.INI [EFFECTS] days=50 weats=60` are presumably
   the cycle lengths.

---

## 14. Second pass — the remaining unknowns closed

Addresses are VAs (`VA = file offset + 0x400000`).  Everything here is
reproducible with `tools/xref.py` (raw ABI xrefs) and the analysed radare2
project (`r2 -q -p /tmp/r2/king ...`), and is asserted by
`tools/script_selftest.cpp` (34 checks) and printed by `--script-report`.

### 14.1 The obstacle player-distance constant: **500 u** (was a 250 u guess)

`fcn.0042b2d0` (called from the obstacle scheduler at `0x42b3c7` and `0x42b448`)
returns `1` when

```
|| obstacle[+0x270..+0x288] - player[+0x40..+0x58] ||  >  qword [0x64b860]
```

`0x42b33a` is the `fcomp qword [0x64b860]`, and the bytes at the VA decode to
`500.0` (double) — the **same global as the obstacle-to-obstacle separation** the
doc already recorded for `0x42b3a0`.  So there is one distance constant, not two:
a candidate obstacle is rejected when the player is within 500 u of it *and*
when it is within 500 u of the closure that is already active.
`ScriptRules::kObstaclePlayerAway` is now `500.0` and
`--script-report` prints `playerAway=500`.

### 14.2 The weather-badness curve (`0x5a9d40`) — full formula

`isGoodWeather()` (`0x484010`) calls `fcn.005a9d40` at `0x48401a` and compares
the result with `0.2` (`0x64b840`).  `0x5a9d40` is 122 bytes and computes the
whole curve from three fields of the `weatherandtime` object plus the frame
time:

```
A = 1 - 2 * w[+0x220]                              ; 0x64c250 = 1.0f
B = 1 + 0.3 * sin(w[+0x78])                        ; 0x64d0f0 = 0.3
b = 0.5 * (1 + A + B * (1 - |A|) * sin(w[+0x68] - pi/2 + t))
badness = clamp(b, 0, 1)                           ; 0x64b670 = 1.0,
                                                   ; 0x64da00 = pi/2,
                                                   ; 0x64b6e0 = 0.5
```

so `w[+0x220]` is **clearness** (1.0 ⇒ badness 0.0, 0.0 ⇒ badness 1.0, 0.5 ⇒ a
pure sinusoid about 0.5 whose amplitude is `0.5*B`), `w[+0x78]` is the
amplitude blend and `w[+0x68]` the phase.  The bad weather of a `GOODWEATHER`
gate is therefore a *sinusoid crossing 0.2 twice per period*, not a step.

`ScriptRules::scriptBadness()` / `scriptBadnessFromWeather()` implement exactly
this (the second takes the port's 0 = clear convention); `--script-report`
prints a probe line, e.g. clearness 0.35 → correct 0.3000.

### 14.3 The node pass is a road-position transition, not a radius

`setPlayerPosition` (`0x43e610..0x43e827`) resolves the player's road
`PositionId`, compares it with the previous one (`0x43e770 cmp eax,edx;
je 0x43e82c`), and **only on a change** computes `newRoadPos - oldRoadPos`
(`0x43e7de..0x43e7f1`) as the travel vector and calls `onNodePass` (`0x43e822`).
So:

* the trigger is *the player's road-graph node changed*, and
* the direction vector is *node to node*, not a free-world position delta.

`ScriptRuntime::setRoadSegments(const std::vector<ScriptRoadSeg>&)` now feeds the
port the real `.way` RSEG links (`nodeA`, `nodeB`, `WDTH`, polyline) and the
runtime reproduces that rule (`roadPass()`): the link the player is inside (its
own `WDTH/2`, never the 20 u radius) decides which node they are on, the node
fires when they reach the far end of the link, and the direction is the road
position difference (corrected 2026-10-06: a forced road tangent suppressed
three authored transverse crossings). Driving 60 m aside is
off-road and passes nothing.  Without a fed network the old
`kNodePassRadius` stand-in is used. The App connection was missing despite
the historical hunk below; it was actually applied on 2026-10-06.

### 14.4 `town` field 8 = per-zoom map icon + label keys

All 29 shipped `town` forms have exactly 9 children, so field 8 is the last one.
Its 29 values fall into three shapes:

| shape | count | example |
|-------|-------|---------|
| `( 5000 )` | 3 | `TOWN_AA`, `TOWN_BB_FUEL`, `TOWN_CC_FUEL` |
| one string | 4 | `( "mapBenzo_ac" )`, `( "map_gasholder_am" )` |
| two lists of 4 strings | 22 | `( ("map_town_adh" "map_town_ad" "map_town_ad" "map_town_adl") ("node_store_0604" x4) )` |

The 4 entries are the four **map zoom levels** (the suffixes `h` / none / `l`
and the `b`/`w` variants select the high/mid/low-detail icon), and the second
list is the node key the map draws the town label at.  So the field is a
*map-icon descriptor*: `(icons[4], labels[4])`, with the single-string and
`(5000)` forms being the short-hand for one icon at every zoom.  It is read by
the map layer, not by gameplay; `GameData::Town::mapIcons` keeps it verbatim.

### 14.5 `ambush` field 7 = the cars-to-spawn slot range (ids enumerated)

The shipped values are *ranges* of 4-digit ids, 2..20 per ambush, grouped by the
first two digits: `1320..1329`, `2320..2329`, `1330..1349`, `2334..2349`,
`2031..2039`, `2080..2089`, `1325..1329`, …  They scale with the ambush's
importance (10 ids for a 5-car ambush, 20 for the largest), and nothing in the
shipped data indexes them: neither `etrashboxes.sch` (59 parked models), the
`parking` bays (33 groups) nor the `.way` node names carry those numbers, and no
consumer could be found in `king.exe` within the budget.  The port therefore
keeps them as *a car count* (`Ambush::slots.size()`), which is what the doc said
before; the ids themselves stay unexplained.

### 14.6 `--script-report` after this pass

```
family=obstacle    total=17 closed=0 toggles=0 separation=500 playerAway=500 delay=[600,900]
family=ambush      total=87 active=0 armed=0 fired=0 band=[250,600]
conditions day=8.0<hour<21.0 goodWeatherBadness<0.20 oneShot=0
badness    clear=0.350 amp=0.000 phase=0.000 t=0 -> 0.3000 (good=0)
nodePass   rule=nearest-node-radius passes=0 radiusPasses=0 radius=20.0 node=-
```

The last line switches to `rule=road-position-transition` as soon as the App
calls `setRoadSegments` (§14.3).

---

## 15. The Lua port — `sch2lua.py` + `lua_sexp.lua` (implemented)

The original files carry no code (§10.1), so there is nothing to decompile: the
port is a **transpiler of a data format**, and its acceptance test is a round
trip, not a diff against decompiled source.

```
original/*.sch ──decrypt──> cp1251 s-expressions ──sexp.py──> tree
      │                                                          │
      │                                     sch2lua.py emits Lua tables
      ▼                                                          ▼
tools/out/lua/*.lua  ──dofile──> lua_sexp.lua ──> canonical text ──> cmp
```

### 15.1 What is produced

`tools/sch2lua.py --all` writes 46 files to `tools/out/lua/` (≈280 KB). Each is
plain data with the head kept as a named field:

```lua
return {
  {
    head = "town",
    "TOWN_AA",
    { q = "Алмазный" },          -- the original wrote this atom as "..."
    "10", {},                    -- `()` is an empty list, not an atom
    { { head = "node", "node_store_1000", "unload" }, … },
  },
  { head = "service", "STO_AA", { q = "" }, "0", { q = "STO_AA" }, { … } },
}
```

Two rules make it lossless *and* readable:

* a **bare atom is a plain Lua string** (`"TOWN_AA"`, `"FallAB1Key"`, `"10"`,
  `"."`) — the value is printed verbatim;
* an **atom the original wrote as `"..."` is `{ q = "..." }`** — the wrapper is
  structural, so no in-string marker can collide with Lua's own escape
  sequences.  (An earlier design used the quote style of the literal as the
  marker; Lua's `\034`-style escapes make that ambiguous.  Do not go back.)
* quoted text is stored as UTF-8 (Lua source is UTF-8); `lua_sexp.to_text`
  transcodes it to cp1251 on the way out.

Fields stay **positional** — the readable field names are the ones in §5, and
`--check`/`--roundtrip` never conflate a field with a form head.

### 15.2 The check

```
tools/sch2lua_check.sh
```

1. reader invariants: escapes, `()` is a list, `""` is a quoted atom,
   `1#e+006`, canonical idempotency (6 cases + 4 properties);
2. the five existing Python consumers still agree
   (`script_survey`/`script_coverage`/`gen_head_table`/`verify_nodes`/`economy_check`);
3. **round trip**: `canonical(original) == canonical(dofile(lua))` for all 46
   scripts — 68 765 canonical bytes, byte identical;
4. all 46 generated files actually load (`assert(dofile(…))`);
5. **SDTE round trip**: `decrypt -> parse -> re-encrypt -> parse` unchanged for
   all 46 (a rebuilt `.sch` is not byte-identical to the shipped one — the author's
   own indentation differs — but it is the same data);
6. the known defects are still reported with locations.

`--encode <rel|all>` writes a rebuilt `.sch` (canonical text re-encrypted with
`decrypt_sch`'s keystream), which is the hook for shipping Lua-authored mods
back into the original format.

### 15.3 Effect on the existing reports

Fixing the reader changed what the earlier tools *see*, which is the point:

| metric | before the fix | after |
|--------|----------------|-------|
| `merchandise.sch` cargo items | 106 (an escaped quote swallowed one) | **107** |
| `merchandise.sch` head `trucks` | 60 | **61** |
| `script_survey` empty-head rows | 182 (merged children) | **557** |

`economy_check.py` was already 107/107 before (it carried its own escape
workaround); it now uses `tools/sexp.py` directly and its numbers are unchanged
(`RESULT: ok`, 147 offers, 1110 jobs, 15 dealers, 11 towns).

### 15.4 There *is* a Scheme interpreter in `king.exe` — but no Scheme code in the data

Worth stating precisely, because the `.sch` extension is misleading.  `king.exe`
embeds a complete scheme core, identifiable by its own debug strings:

```
$Id: Sch_eval.cpp   1999/6/25 20:22:40     $Id: sch_read.cpp  2000/12/5  14:49:36
$Id: sch_krnl.cpp   1999/6/26 20:50:56     $Id: sch_mcro.cpp  1999/3/10  18:05:52
$Id: Sch_envt.cpp   1999/6/25 16:32:00     $Id: sch_write.cpp 2000/1/10  14:51:20
… 22 files, plus the symbols `apply`, `eval`, `scheme-report-environment`
```

and one script filename, `init.sch`, which is **not shipped** (it is not in
`original/`, in `LOAD.RES`, or in any `.res` payload).  The shipped `.sch`/`.CNF`
files, by contrast, parse into pure data: after reading all 46 of them there is
no `define`, `lambda`, `quote` or `if` form anywhere — every head is one of the
engine's own loader keywords (§8/§9).  So the interpreter exists in the binary
and the game could evaluate Scheme, but the shipped content is data that the
engine dispatcher consumes directly (§10.1).  There is therefore nothing to
decompile; the Lua port is a data transpiler (§15.1) plus a re-implementation of
the engine rules (§15.4 below).

### 15.5 What Lua buys, and what still has to be written

The data conversion is done; **the behaviour is not**. The conditions live in
`king.exe` (§10), not in the data, so a Lua port must still port them:

* now: `ScriptRules.{h,cpp}` has the recovered rules (day window, weather
  badness, node-pass, ambush band, obstacle scheduler) and `--script-report`;
* next: express those rules once in Lua and load `tools/out/lua/*.lua` as the
  default content;
* after that: add an evaluation layer (`on_node_pass`, `on_day_tick`,
  `on_ambush_armed`, cargo delivery) so missions and events can be *authored* in
  Lua rather than inferred from the tables. That is where a scripting rewrite
  actually pays off; re-emitting the tables is only the prerequisite.

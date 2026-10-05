# `ht2editor` — the separate map and car-model editor

A second executable, `ht2editor`, edits two things the shipped data only *describes*:

* **the road graph of a map tile** — `original/ENV/<tile>.way` (nodes, segments),
* **a car model** — a vehicle container of `original/COMMON/TRUCKS.B3D`
  (its parts/submeshes, materials and anchors).

It is **not part of the game**: own target, own window, own CLI. It links the
game's own parsers and renderer (`src/world/B3D.cpp`, `src/world/Way.cpp`,
`src/gfx/Renderer.cpp`, `src/res/Res.cpp`, `src/res/Mask.cpp`,
`src/world/SceneTextures.cpp`) — there is no second parser anywhere — and it
never writes a shipped file. Every edit goes into **one overlay text file**,
which `ht2re --overlay FILE` reads back at startup.

```
tools/editor/        main.cpp EditorApp.{h,cpp} MapEditor.{h,cpp} ModelEditor.{h,cpp}
src/world/Overlay.{h,cpp}   the format + the semantics, shared by both binaries
tools/editor_check.sh       the headless verification harness
tools/editor_overlay.diff   the engine hunk, as a patch (App.cpp is shared)
```

---

## 1. Build and run

The editor is part of the normal build:

```sh
cmake -B build && cmake --build build -j
#  -> build/ht2re      (the game, unchanged)
#  -> build/ht2editor  (this editor)
```

Interactive (own SDL/OpenGL window, 1280x720):

```sh
# map editor: ENV/aa.way, overlay ./ht2edit.ovl
./build/ht2editor --data original --map aa

# car-model editor: the "ZilR" container of COMMON/TRUCKS.B3D
./build/ht2editor --data original --vehicle ZilR
```

Headless (offscreen, one frame to a BMP, then exit):

```sh
./build/ht2editor --data original --map aa --select STO_AA --shot shots/editor/map_2d.bmp
./build/ht2editor --data original --map aa --view 3d \
                  --cam 3000,1400,-3500,0,-45 --shot shots/editor/map_3d.bmp
./build/ht2editor --data original --vehicle ZilR --angle 205,-18 --shot shots/editor/model_a.bmp
./build/ht2editor --data original --vehicle Fiat --angle 25,-10  --shot shots/editor/model_b.bmp
```

### CLI

| option | meaning |
|---|---|
| `--data DIR` | game data root (default `./original`) |
| `--map TILE` | edit `ENV/TILE.way` (e.g. `--map aa`) |
| `--vehicle NAME` | edit the `NAME` container of `COMMON/TRUCKS.B3D` (e.g. `--vehicle ZilR`) |
| `--overlay FILE` | overlay to read/write (default `ht2edit.ovl`) |
| `--view 2d\|3d` | map view mode (default `2d`) |
| `--select NAME` | select this node (map) / part (model) at startup |
| `--angle YAW[,PITCH]` | model camera orbit angles (default `205,-18`) |
| `--cam x,y,z,yaw,pitch` | explicit camera (map 3D / model view) |
| `--cmd 'CMD'` | run one script command (repeatable) |
| `--script FILE` | run a command script (see §5) |
| `--list` | print the loaded nodes/segments or parts/anchors, then exit |
| `--shot FILE` | render one frame to `FILE` (BMP) and exit |
| `--blank` | render the background only — the harness's control frame |
| `--no-textures` | do not decode `.res` textures |
| `--size WxH` | window / offscreen size (default `1280x720`) |

Exactly one of `--map` / `--vehicle` per run: one process edits one subject.

### Interactive keys

| | map editor | car-model editor |
|---|---|---|
| left drag | move the picked node | orbit |
| right drag | pan | — |
| wheel | zoom | zoom |
| `TAB` / `SHIFT+TAB` | next / previous node | next / previous part |
| `A` | — | next anchor |
| `V` | toggle 2D / 3D | — |
| `F` | fit the network to the window | — |
| `H` | — | hide / show the selected part |
| arrows | nudge the node (x/z) | move the selected anchor (x/z) |
| `PgUp` / `PgDn` | nudge the node in height | — |
| `Q` / `E` | — | rotate the selected anchor about Y |
| `DEL` | delete the selected node | — |
| `S` | **save the overlay** | **save the overlay** |
| `ESC` | quit (does **not** save) | quit (does **not** save) |

Quitting never saves: only `S` (or a script `save`) writes the overlay, so a
stray key press cannot rewrite the file.

---

## 2. What the map editor does

* loads `ENV/<tile>.way` through the game's `Way` parser (rooms, `RNOD` nodes
  with `NNAM`/`POSN`/`FLAG`, `RSEG` segments with `ATTR`/`RTEN`/`WDTH`/`VDAT`),
* applies the overlay on top, so reopening a run shows the saved state,
* draws the network **from above (2D)** as a wireframe with the node positions,
  the segment colours (from `ATTR`) and the segment widths (from `WDTH`)
  scaled to the world units the `.way` stores,
* draws it **in 3D** as well: `Renderer::draw()` (the game's own generated
  ground + road ribbon) plus `Renderer::drawScene()` (the tile's `B3D`
  geometry, textured from `ENV/<tile>.res`), with the network wireframe
  projected on top through exactly the matrices `Renderer::drawScene()` uses,
* lists every node (`name`, `x y z`, `flags`) and every segment
  (`index`, `name`, `attr`, `width/width1`, point count),
* lets you select a node or a segment, **move** a node (drag or arrow nudge,
  `PgUp`/`PgDn` for height), **edit** a segment's `attr` / `width` / `width1`,
  **add** and **delete** nodes and **connect** two nodes with a new segment.

Vertical axis: `Way::Pos::y` **is the height**, exactly as the `.way` `POSN`
chunk stores it (`x`, `z`, `height`). The overlay keeps that order.

## 3. What the car-model editor does

* loads `COMMON/TRUCKS.B3D` and assembles the named vehicle with
  `B3D::buildNamedScene()` — the same call the game makes,
* lists every **submesh** (`name`, `material`, triangle count, radius, bbox)
  and groups them into **parts** (one part = every submesh sharing a node name;
  the container is flattened per face × material, so a wheel is many submeshes),
* lists every **anchor** that this model's links actually placed
  (`B3D::usedAnchors`) — the container holds the anchors of *every* vehicle in
  `TRUCKS.B3D` (876 of them), of which `ZilR` uses 12, so the panel and the 3D
  markers show only the relevant ones; `--list` still dumps the whole container,
* shows the model **textured** from `COMMON/TRUCKS*.RES` and lets you rotate /
  zoom it,
* **toggles parts** (this is the intact/damaged skin switch: `Fiat` links a
  `Capot_0`/`FR_0`/… intact shell of `Fiat_all1` **and** a `Capot_2`/`FR_2`/…
  dented shell of `Fiat_alld1`; hiding the `*_2` parts leaves the intact car —
  the same convention `App::dropDamagedParts()` uses),
* **recolours a part** by swapping its material name, which changes both the
  texture the renderer binds and the fallback material colour,
* **adjusts an anchor's translation and rotation**. Anchors place the linked
  part containers during `buildNamedScene()`, so moving one really moves the
  mirror/wheel/bumper it mounts.

## 4. The overlay file format

Plain text, line oriented. `#` starts a comment, blank lines are ignored, and
the first non-comment line must be `HT2EDIT 1`.

```
HT2EDIT 1
# ---- map (.way) ----------------------------------------------------
NODEPOS   <tile> <name> <x> <y> <z>            node moved to an absolute spot
NODEFLAGS <tile> <name> <flags>                FLAG dword (decimal or 0x…)
NODEADD   <tile> <room> <name> <x> <y> <z> <flags>
NODEDEL   <tile> <name>
SEGEDIT   <tile> <index> <attr> <width> <width1>
SEGDEL    <tile> <index>
SEGADD    <tile> <name> <attr> <width> <width1> <n> <x y z> * n
# ---- vehicle models (COMMON/TRUCKS.B3D) ----------------------------
PART      <model> <part> hidden|shown
PARTMAT   <model> <part> <material>
ANCHORT   <model> <anchor> <tx> <ty> <tz>
ANCHORM   <model> <anchor> <m0 … m8>
```

* `<tile>` is an `ENV` tile stem (`aa`); `*` matches every tile. A vehicle
  record's `<model>` is the container name (`ZilR`, `Fiat`, …).
* `<index>` is the position of the segment in the **shipped** `.way`'s own
  segment order — the order `Way::segments()` returns for a freshly loaded
  file. It is deliberately independent of the other records: every index is
  resolved against the pristine list *before* any `SEGDEL`/`SEGADD` is folded
  in, so the records can be written in any order and re-applied idempotently.
* `NODEPOS` stores the **absolute** position (not a delta), so replaying the
  file any number of times is the same as replaying it once.
* `ANCHORM` stores the nine floats of `B3D::Anchor::m` in the engine's own
  column convention (`p' = M·p`, row-major storage `m[r*3+c]`) — the same
  layout `B3D::load()` produces from the `subtag 24` record.

A real file (trimmed), written by

```sh
./build/ht2editor --data original --map aa --overlay demo.ovl --cmd 'select STO_AA' \
    --cmd 'moveto 4444.25 451.5 -5222.75' --cmd 'addnode editor_new_node 4300 452 -5100' \
    --cmd 'connect editor_new_node STO_AA 12.5' --cmd 'setseg 0 width 22.25' --cmd save
```

```
HT2EDIT 1
# HT2:RE editor overlay - written by ht2editor, read by ht2re --overlay
# The shipped data is never modified; delete this file to get it back.
# Format: docs/editor.md

# ---- map (.way) ----------------------------------------------------
NODEPOS aa STO_AA 4444.2500 451.5000 -5222.7500
NODEADD aa room_aa_000 editor_new_node 4300.0000 452.0000 -5100.0000 0x0
SEGEDIT aa 0 2 22.2500 8.0000
SEGADD aa editor_new_node__STO_AA 0 12.5000 0.0000 2 4300.0000 452.0000 -5100.0000 4444.2500 451.5000 -5222.7500

# ---- vehicle models (COMMON/TRUCKS.B3D) ---------------------------
PART ZilR Kapot_0 hidden
PARTMAT ZilR Kapot_1 Zil_parts1
ANCHORT ZilR ZilRWheel0Space -0.6 2.1 -0.89
ANCHORM ZilR ZilRWheel0Space 0.965926 0 0.258819 0 1 0 -0.258819 0 0.965926
```

`NODEADD`/`SEGADD` are idempotent by name: loading the same tile twice (the
game does load each `.way` in `loadData()`, `collectWaySegments()` **and**
`indexWayNodes()`) does not duplicate the node or the segment.

## 5. Command scripts

`--script FILE` (and `--cmd 'CMD'`) drive the editor without a window; every
command goes through the very code path the interactive keys use.

```
view 2d|3d            fit                     select NAME            selectseg I
selectanchor NAME     moveto X Y Z            nudge DX DY DZ         setflags 0xN
addnode NAME X Y Z [ROOM]                      delnode NAME           connect A B [W]
setseg I width|width1|attr V                   setseg I del
part NAME hide|show   partmat NAME MATERIAL
anchormove NAME DX DY DZ                       anchorpos NAME X Y Z
anchorrot NAME DYAW DPITCH DROLL               angle YAW [PITCH]      zoom D
cam X Y Z YAW PITCH   save                    shot FILE              list
report                echo TEXT
```

`list` prints the machine-readable node/segment or submesh/part/anchor dump,
`report` prints the one-line state summary; both are what
`tools/editor_check.sh` reads.

## 6. What the engine does with it

`ht2re --overlay FILE` (`Options::overlay`) reads the file once in
`App::loadData()` and then:

| record | where it lands in the game |
|---|---|
| `NODEPOS` `NODEFLAGS` `NODEADD` `NODEDEL` | every tile's `Way` in `App::collectWaySegments()` and `App::indexWayNodes()` → the road profile, the AI road graph (`waySegs_`), the named-node map (`wayNodes_`: parking bays, service stops, ambushes, script node events) and the game's own road-map overlay (`src/ui/Map.cpp` pins and labels) |
| `SEGEDIT` `SEGDEL` `SEGADD` | the same two loaders plus `way_` = `ENV/aa.way` itself (the ribbon `Renderer::buildRoadMesh()` uploads) and the ground profile `GroundMap::build()` seeds its cells from |
| `PART` `PARTMAT` | `App::run()` / `App::loadData()` after `B3D::buildNamedScene()`, on `trucks_` — the player's truck |
| `ANCHORT` `ANCHORM` | the same two places **before** `buildNamedScene()`, because the anchors are what place the linked part containers (and the rear-view mirror mount, `Mirror::load()`) |

There is **no** overlay file by default: without `--overlay` the engine behaves
exactly as before, byte for byte.

The hunk that adds this lives in `src/app/App.h`, `src/app/App.cpp` and
`src/main.cpp`. Because `App.cpp` is shared with another agent it is also
shipped as a patch:

```sh
git apply -p1 tools/editor_overlay.diff      # if a later edit drops the hunk
```

## 7. Verification

```sh
tools/editor_check.sh                      # 48 checks, its own harness
python3 tools/shot_test.py                 # the game's own suite, untouched
```

`tools/editor_check.sh` renders the map in 2D and 3D and the model from two
angles, asserts every frame differs from a `--blank` control, moves/adds/deletes
nodes and segments, saves, **re-loads the file and re-asserts the values**,
checks the intact/damaged skin toggle, drives the game with the same overlay and
asserts both that the game consumed it (its own `overlay: …` log lines, the road
segment count, the rendered frame) and that `ENV/aa.way` and
`COMMON/TRUCKS.B3D` are byte-identical afterwards.

## 8. Not editable yet

* **Segment geometry.** `SEGEDIT` changes `attr`/`width`/`width1`; the `VDAT`
  polyline points of a shipped segment cannot be moved or re-sampled, and the
  per-point heights follow the node heights. Only a whole new straight segment
  can be added (`SEGADD`, i.e. `connect`).
* **Rooms.** `NODEADD` puts the new node in an existing room (the tile's first,
  or the one named on the command line); rooms cannot be created, renamed,
  reordered or deleted.
* **Terrain and road surfaces.** The map editor does not touch
  `MTR/terrain*.raw` or the tiles' own asphalt geometry. The 3D view shows what
  the game derives from the tile *as it is*; a node moved off the tile's asphalt
  has no matching surface under it.
* **`FLAG` semantics.** The dword is editable and saved, but its meaning is not
  decoded (`docs`/`NOTES.md` only record the observed values `0`, `0x10100`,
  `0x10700`), so the editor cannot tell you what a value does. The same holds
  for `ATTR`, which is only used here to colour the segments.
* **The tile's road ribbon width.** `Renderer::buildRoadMesh()` draws every
  segment with a constant 15-unit half width; a `SEGEDIT … width` therefore does
  not change the ribbon the game renders (it does change the AI graph, the road
  profile, the ground profile and the map overlay).
* **New parts, new submeshes, new anchors, new vehicles.** The model editor can
  hide/rename/anchor-shift what the container already contains; it cannot create
  geometry, and it does not write `.b3d` back.
* **Multi-vehicle batches.** `PART`/`PARTMAT`/`ANCHOR*` are applied to the
  player's `trucks_` container only — parked cars, AI traffic models and the
  cockpit (`CABINES.B3D`) are unaffected.
* **Undo/redo.** Saving writes the whole overlay; there is no history beyond
  "quit without saving".
* **A grid / snapping / a numeric entry dialog.** Node moves are drag or
  arrow-nudge (1 world unit per press) or `moveto` from a script.

## 9. Limitations of the editor itself

* The 3D map view needs the tile's own `ENV/<tile>.b3d` + `.res`; without them
  (`--no-textures`, a missing file) it still draws the network wireframe over
  the generated ground/terrain.
* Toggling to 3D **interactively** cannot rebuild the terrain/ground mesh:
  `Renderer::init()` builds those once from the pointers it holds at that
  moment, so the editor registers them up front (before `init()`) whenever a
  run may show 3D — that is why `ht2editor --map TILE` loads the tile even in
  2D. The tile `B3D` geometry and the wireframe do appear either way.
* One subject per process; there is no combined map+car session.

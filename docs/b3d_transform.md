# B3D scene tiles → world space (HT2:RE)

Reverse-engineered from `original/king.exe` (PE32, image base `0x400000`) and validated
numerically against the shipped data. Every claim below is marked **[PROVEN]** (king.exe
code and/or a numeric check over the 42 `.b3d` files) or **[ASSUMED]**.

Tooling: `tools/b3d_world.py` (independent parser + assembler + validator + top-down PNG),
images in `tools/out/b3dw_*.png`.

---

## 0. The rule in one line

> A container record (`subtag 5`, or `subtag 4` for instanced objects) is emitted **once per
> anchor that references it**, with

```
p_world = p_local · M + t          (row vector; M is the 4x3 matrix read verbatim
                                    from the anchor record, rows of 3 floats)
```

> The reference comes from a **`subtag 18` link record** `{A → B}` ("place container `B` at
> anchor `A`") and/or from the container's own **`subtag 4` anchor name** (which always names
> the paired child `A_` of the same anchor and is therefore numerically identical).
> Containers nobody references are **not** objects: they inherit the transform of their
> record-tree ancestors (which for room content is identity, because room content is authored
> in world space).

Worked example (`ENV/aa.b3d`): `link_Sto` = `{StoAASpace → STOS}`. `5 'STOS'` is modelled
around its own origin (`x −6.7..6.2, z −10.1..10.1, h 0..6.4`); `24 'StoAASpace'` carries
`M = diag(−1,−1,1)` (180° about the up axis in b3d components) and `t = (4750, −5675, 450)`.
Placed: `x 4743.8..4756.7, z −5685.1..−5664.9, h 450.0..456.4`. The `.way` node `STO_AA`
sits at exactly `(4750, −5675, 450)` → distance from the assembled mesh to that node = **0.00**.

---

## 1. Record parameter layouts (dwords after the subtag)

These differ from `tools/b3d_full.py`'s docstring in two places (`18`/`4` second name is a
full `name32`, and `30` is `4f + name32 + 6f`, not `4f + name32 + 6dw`); both parsers agree
on 100 % byte consumption, this table is what king.exe's readers actually consume.

| sub | node type | params (dwords) | layout | children |
|----|----|----|----|----|
| 24 | `ntSpace` = 6 | 13 | `m9[9f] pos[3f] case[1f]` | `count { child }` — the paired `XSpace_` |
| 18 | link | 20 | `sphere[4f] name32 A name32 B` | leaf |
| 4  | instance | 20 | `sphere[4f] name32 anchor name32 name2` | `count { child }` (the meshes) |
| 5  | group/container | 12 | `sphere[4f] name32` | `count { child }` |
| 19 | room | 0 | — | `count { child }` |
| 30 | portal | 18 | `centre[4f] name32 'tile:room' p0[3f] p1[3f]` | leaf |
| 40 | point object | 32 | `pos[3f] r[1f] … name` | leaf |

`sphere = (cx, cz, h, radius)` in the container's **local** frame — see §4.3.

**[PROVEN]** layouts, from the readers (see §3.3) plus the stream reader at `0x1ed819`:

```
0x1ed819  cmp eax, 0x6f      ; 111 open
0x1ed825  cmp eax, 0x22b     ; 555 close      -> loop
0x1ed835  cmp eax, 0x1bc     ; 444 separator  -> loop
0x1ed83c  cmp eax, 0xde      ; 222 end of file
0x1ed847  cmp eax, 0x14d     ; 333 record
0x1ed85c  call 0x1ef830      ; parse one record
```

`0x1ef830` reads the 32-byte name into the global buffer `0x723a04`, reads the subtag, then
dispatches with a 42-entry jump table at **VA `0x5f0614`** (`jmp dword [eax*4 + 0x5f0614]`,
compared against `0x29`). Table entries (VA) for the interesting subtags: `24 → 0x5efdad`,
`18 → 0x5f0085`, `4 → 0x5efb3d`, `5 → 0x5ef95b`, `19 → 0x5f00e2`, `30 → 0x5f0472`.
Each handler `new`s a node, calls the base constructor `0x1f19b0(this, name, type, capacity)`
and installs a vtable; the common tail `0x1f05ba` then calls `vtable[+0x2c](stream, …)` — the
per-type **payload reader**.

---

## 2. The transform: 4x3 matrix, row-major, **row-vector** application

`24` payload = `m9[9f] pos[3f] case[1f]`, read into the node's `+0x40` block as **four rows of
three floats**:

```
node+0x40:  m0 m1 m2   <- first row of 3 floats from the file
node+0x4c:  m3 m4 m5
node+0x58:  m6 m7 m8
node+0x64:  m9 m10 m11 = translation (the file's pos[3f])
```

**[PROVEN]** `Space::read` at `0x5f2640`:

```
0x5f2648  push edi
0x5f2649  lea  ecx, [esi + 0x40]
0x5f264c  call 0x1aba10        ; reads 4 groups of 3 floats  (0x1ab790 = read 3 floats)
0x5f2651  call 0x1ab140        ; reads the `case` dword -> node+0x38 (0 or 1)
0x5f267f  call 0x1ab140        ; reads the child count -> 0x1f1db0 (child parser)
```

and `0x1aba10`:

```
0x001aba18  push edi / call 0x1ab790   -> dest+0x00
0x001aba1f  lea ecx,[esi+0xc] / call    -> dest+0x0c
0x001aba28  lea ecx,[esi+0x18] / call   -> dest+0x18
0x001aba31  lea ecx,[esi+0x24] / call   -> dest+0x24
```

so the on-disk order *is* the memory order and the first 9 floats are a row-major 3×3.

### 2.1 How the matrix is applied — row vector `p·M + t`

**[PROVEN]** the renderer transforms a point with the 12-float matrix at `0x723a58`
(`+0x24` = translation) at `0x1f2317..0x1f2336`:

```
0x1f2317  fld  dword [0x723a58]    ; m0
0x1f231d  fmul dword [ebp+0x34]    ; * v[0]
0x1f2320  fld  dword [0x723a70]    ; m6
0x1f2326  fmul dword [ebp+0x3c]    ; * v[2]
0x1f2329  faddp st(1)
0x1f232b  fld  dword [0x723a64]    ; m3
0x1f2331  fmul dword [ebp+0x38]    ; * v[1]
0x1f2334  faddp st(1)
0x1f2336  fadd dword [0x723a7c]    ; + t0
```

    p'_0 = v0·m0 + v1·m3 + v2·m6 + t0        (= column 0 of M)

The second output (`0x1f233c..0x1f2361`) is `v0·m1 + v1·m4 + v2·m7 + t1`, the third
(`0x1f2365..0x1f238a`) `v0·m2 + v1·m5 + v2·m8 + t2`. That is exactly

    p' = p · M + t        (row-vector convention, M row-major)

The alternative reading `M·p + t` would need `m1, m2` for the first output — it is not what
the code does.

### 2.2 Composition — `abs = local · parentAbs`

**[PROVEN]** `GetAbsSpace` at `0x1f25c0` (vtable `0x64d7d0[+0x2c]` of `ntSpace`; the wrappers
`GetAbsSpace`/`GetRelSpace`/`ReplaceSpace` are at `0x0e4930/0x0e497a/0x0e48e2`):

```
0x1f25c4  mov  ecx, [esi+0x2c]     ; parent
0x1f25cb  mov  dword [esi+0x38], 2 ; mark dirty when a parent exists
0x1f25e9  push edi                 ; out
0x1f25ea  call 0x1f25c0            ; abs(parent) -> out
0x1f25ef  add  esi, 0x40           ; &local
0x1f25f2  mov  ecx, edi            ; dst = out
0x1f25f7  call 0x1abc10            ; dst = local (*) parentAbs
```

and `0x1abc10` → `0x1aba40(local, src, dst)` is a plain row-major matrix product
`out = src · dst` — verified by expanding its first output
(`0x1aba66..0x1aba7a`: `m0·n0 + m1·n3 + m2·n6` with `m = src`, `n = dst`, i.e.
`(src·dst)[0][0]`). So with row vectors: `p·local·parentAbs` — **the local space is applied
first, then the parent**, which is what nesting `<X>Space` inside its parent means.

### 2.3 Component order: rows are (x, z, height)

**[PROVEN, data + code]** `0x1aba10` copies the file's `pos[3f]` verbatim into the fourth row,
i.e. the translation is `(t0, t1, t2)` in the same component order as the file, and the third
output row (`t2`) is used as the *height* by the scene code. Independent check: for
`ENV/aa.b3d`, `StoAASpace.pos = (4750, −5675, 450)` and the `.way` node `STO_AA` is
`(x 4750, z −5675, h 450)`. Hence the file's component order is `(x, z, height)` — matching
the established data convention (`b3d comp0 = world x, comp1 = world z, comp2 = height`) —
and the rows of `m9` are the (x-row, z-row, height-row). `m9` is **not** an Euler triple and
**not** a quaternion; it is stored as a 3×3 with the third row/column `(0,0,1)` about the up
axis for every anchor in the corpus.

### 2.4 Scale

There is no scale field: all anchors in the corpus are isometries (rows orthonormal to
~1e-3, `m9 ≈ ±` rotation). The `case` float is either `0`, or tiny numerical noise
(`−1.2e−16`), or `1`; it is read into `node+0x38`, the same field `GetAbsSpace` switches on
(0/1/2), so it is a *state*, not a scale. **[PROVEN for the read, ASSUMED for the semantic]**
(nothing in the corpus sets it to 2).

---

## 3. Which record places what

### 3.1 `24 '<Name>Space'` = anchor (node type 6 = `ntSpace`)

Creates a node of type 6 (`0x1efdad`: base ctor `0x1f19b0(name, 6, 1)` → vtable `0x64d7d0`),
`+0x40` = the 4×3 matrix, `+0x38` = case, capacity 1 child (the paired `<Name>Space_`, whose
own matrix is identity in every file, so `abs(NameSpace_) == abs(NameSpace)`).

### 3.2 `18 '<link…>'` = "place B at A" (node type 17)

Reader `0x5f9850` (`vtable 0x64c7dc[+0x2c]`):

```
0x5f985b  lea ecx,[esi+0x14] / call 0x1ac7f0   ; 4 floats = bounding sphere -> node+0x14
0x1f9876  read name32 -> node+0x44             ; A = anchor name
0x1f987f  mov ecx, 0x71e820 / call 0x1d2650    ; intern/lookup in the global name table
0x1f989b  read name32 -> node+0x40             ; B = container name
```

The *apply* function `0x5f98c0` (`vtable[+0x30]`) is called per frame with the scene object:

```
0x1f991d  mov  ecx, [ebx+0x44]                 ; A (anchor node)
0x1f9922  je   0x1f993b                        ; no anchor -> skip transform
0x1f9924  lea  edx, [esp+0x58]
0x1f9929  call 0x1f25c0                        ; GetAbsSpace(anchor) -> local matrix
0x1f9934  mov  ecx, ebp
0x1f9936  call 0x1ae580                        ; apply that matrix to the instance
0x1f993b  lea  ecx, [ebx+0x14]
0x1f9941  call 0x1b4fc0                        ; bounding-sphere cull/LOD, see §4.3
```

`0x1ae580` gets the composed matrix; `0x1ac2e0` (matrix inverse) and `0x1aba40` around
`0x1f99bd..0x1f99cb` build the object's world/view matrix from the anchor's absolute matrix.
**Conclusion:** the anchor's *absolute* matrix is the object's local→world matrix; multiple
links naming the same container produce independent copies (each link is its own record, so
the emit order is the file order of the `18` records).

### 3.3 `4 '<Inst>'` = instanced object with a built-in anchor reference

Reader `0x5f4340` (`vtable 0x652bd0[+0x2c]`), node type 4:

```
0x1f434c  lea ecx,[esi+0x14] / call 0x1ac7f0   ; bounding sphere
0x1f4366  lea ecx,[esi+0x3c] / read name32     ; param name1  = ANCHOR name ('DRZ001Space_')
0x1f438b  lea ecx,[esi+0x38] / read name32     ; param name2  = secondary name ('sharp_DRZ001')
0x1f43ab  read dword -> 0x1f1db0               ; children
```

The type-4 vtable entry `+0x34` (`0x5f43d0`) does

```
0x1f4455  mov  ecx, [ebp+0x3c]     ; the anchor node resolved from param name1
0x1f4458  call 0x1f25c0            ; GetAbsSpace(anchor)
0x1f4469  call 0x1ac2e0 / 0x1aba40 ; compose with the caller's matrix
```

**So `subtag 4` places geometry by itself** — the link is not required. Both mechanisms exist
side by side and are *redundant but consistent*: measured over all 42 `.b3d` there are 990
`subtag 4` nodes whose anchor name resolves, **all 990 are also covered by a `subtag 18`
link**, and 1036 name→matrix comparisons between the two mechanisms agree with **0
disagreements** (the built-in name points at the paired `XSpace_` child, whose local matrix is
identity). Practical consequence for the engine: place via the links (as now) *or* via the
built-in name, but deduplicate by `(container, matrix)` if both are honoured, otherwise every
such object is emitted twice with an identical transform (invisible overdraw, but it doubles
vertex/instance counts).

### 3.4 `5 '<Group>'` = plain container

Payload `0x5f3850` (`vtable 0x64c26c[+0x2c]`): 4 floats bounding sphere + name32. No
transform of its own; it is placed only if a link names it (or if a space ancestor exists in
the record tree). The subclass chosen at load time depends on the *name* (handler
`0x1ef95b`: `strncmp(name, "env", 3)` → one class, else `0x1efa3a`; `0x679a18 = "lamp"`),
which is a collision/rendering detail, not a transform.

### 3.5 `19 '<room…>'` = world-space scenery container

No parameters, no transform; its children are authored **in world coordinates**
(`ENV/aa.b3d room_aa_000`: `x 2050..5010, z −5928..−3101, h 447..583`; `MENV/dr.b3d`'s 45
rooms tile the whole menu circuit). Their accumulated transform is identity. Room content is
where the terrain/roads/`subtag 40` props live, and it is the ground reference used for
validation.

### 3.6 Name tables

The intern table is the global object at `0x71e820`: `0x1d2540` registers a node under the
record's name (called from the record parser tail at `0x1f05df`), `0x1d2650` looks a name up
and stores the node pointer into the destination field. That is how `subtag 18`'s `A` and
`B` and `subtag 4`'s anchor name become node pointers at load time. Because lookup happens
while parsing, a name must already be registered; the files are authored
anchors-first, so this always holds (no forward references were observed: every `A`/`B`
resolved — see the validation below, where the only unresolved link targets are
`AnmObj0201`/`AnmObj0202` in `aa.b3d`, which are animated objects created by the game, not
containers in the file).

---

## 4. Records that are **not** transforms

### 4.1 `30` = room portal (not a transform)

**[PROVEN, code + fit]** Reader `0x5fc360` (`vtable 0x65288c[+0x2c]`, node type 14):
4 floats → `+0x70`; the name is split at `':'` (a *tile* prefix such as `db:` and a room name
such as `room_db_000`); then `0x1ab790` is called twice, reading `p0[3f]` and `p1[3f]`, and the
function then computes cross products from `p1 − p0` to build an orthonormal **door basis**
into the node's own zeroed `+0x3c` matrix block (the file never supplies that matrix).

The four floats are `centre = (p0+p1)/2` and `radius = 1.001 · |p1−p0|/2`. Measured over
**all 2309 `subtag 30` records** in the corpus: max `|centre − (p0+p1)/2|` = 1.2e-4, max
`|radius/(|p1−p0|/2) − 1.001|` = 1.1e-4 (the 0.1 % is a cull-sphere margin, the same
+0.1 % seen between the `18` sphere radius and the container vertices on the larger
containers). Examples:

| file | centre | 4th | p0 | p1 | \|p1−p0\|/2 |
|----|----|----|----|----|----|
| `MENV/dr.b3d room_dr_014` | (100, 325, 50) | 52.255 | (85, 325, 0) | (115, 325, 100) | 52.255 |
| `MENV/dr.b3d room_dr_016` | (125, 400, 50) | 61.299 | (100, 425, 0) | (150, 375, 100) | 61.24 |
| `ENV/aa.b3d → db:room_db_000` | (2500, −3025, 525) | 79.136 | (2475, −3025, 450) | (2525, −3025, 600) | 79.057 |

So `30` is the opening between two rooms (a vertical segment of width `|p1−p0|` spanning
ground→top, in b3d `(x, z, height)`), used for room streaming/teleporting; `aa.b3d` even
points at rooms in *other tiles* (`da:room_da_190`, `db:room_db_000`). The node's matrix is
built from the segment, so it must **not** be applied to any B3D geometry.

### 4.2 `40` = point object (glow, tree, collision marker)

`pos[3f]` world coordinates + a name (`AAGlow33`, `CollisionOfTerrain`, `Tree_12`…). No
transform; drawn/used at `pos`. `Tree_*` meshes live in the room and are already world
positioned (their canopies sit at `h 548..583`, i.e. ~100 units above the `h=447.1` ground —
this is what makes identity-transformed tile renders show "objects in the sky").

### 4.3 The 4 leading floats of `18`, `4`, `5` = local bounding sphere

**[PROVEN, numeric + code]** For every container with a mesh in the corpus the stored radius
equals the largest vertex distance from the stored centre to within 0.21 units:

| container | file | stored `(cx,cz,h,r)` | max vertex distance from centre |
|----|----|----|----|
| `STOS` | aa | (−0.29, −0.04, 3.21, 12.409) | 12.40 |
| `store_aa` | aa | (−1.0, 5.9, 4.175, 46.665) | 46.62 |
| `Combinat` | aa | (2.5, 0.0, 4.0, 123.33) | 123.21 |
| `Tribune` | dr | (−0.108, 11.187, 6.539, 63.426) | 63.36 |
| `lep12` | dq | (287.34, 914.17, 23.33, 65.56) | 65.6 |

…with 1146 containers tested and max radius error 0.211. Radius `0` marks a container with no
renderable mesh: over the corpus, **0** placements have a mesh and radius 0, and only 2 have
no mesh and a nonzero radius (the `treesNN` groups whose meshes live in sibling records).
The consumer is the cull/LOD test `0x1b4fc0`, called from the link applier with
`&node+0x14`:

```
0x1b4ffc  fld  dword [esi+0xc]      ; the radius
0x1b4fff  fcomp qword [0x64b678]    ; == 0.0 ?
0x1b500a  je   0x1b5024
0x1b500c  mov  eax, 0x1f
0x1b5011  mov  dword [edi+0x2050], eax   ; visibility/LOD state = 31
...
0x1b5047  call 0x1ac640             ; transpose + copy the 4x3 view matrix
0x1b509c  fld  dword [esp+0x80]     ; transformed sphere centre components
0x1b50a3  fmul dword [0x64c938]
```

i.e. transform the sphere centre into view space and cull/LOD from it; radius 0 short-circuits
to "hidden/skip". **These floats never influence placement** (the sphere is rotation-invariant
data consumed after the placement matrix is built).

---

## 5. Validation

`python3 tools/b3d_world.py <b3d>` prints per-anchor transforms, per-container world bboxes
and the numbers below; `--all` runs the corpus checks; a top-down PNG plus a zoomed PNG are
written to `tools/out/`.

### 5.1 `ENV/aa.b3d` (the tile that exposed the problem)

```
.way aa.way (MNAM=aa) nodes=20
Combinat  <- link:CombinatAASpace  anchor_dev=0.000  base-anchor_h=+0.00  nearest_way=  1.00 (mapSto_AAXXX)  below_ground=0.0%
STOS      <- link:StoAASpace       anchor_dev=0.000  base-anchor_h=+0.00  nearest_way=  0.00 (STO_AA)         below_ground=0.0%
store_aa  <- link:StoreAASpace     anchor_dev=0.000  base-anchor_h=-0.30  nearest_way=  7.65 (node_store_1002) below_ground=0.0%
tile flat ground level (1st pct of room heights) = 447.06
assembled vertices: 4347 ; below ground: 0 (0.0%) ; below by more than 3 units: 0
.way node STO_AA        -> nearest assembled vertex d=0.00 (h=450.1)
.way node mapSto_AAXXX  -> nearest assembled vertex d=1.00 (h=468.5)   [inside the Combinat]
.way node node_store_*  -> nearest assembled vertex d=7.65..8.24 (h=455.0)  [row of docks in front of the store]
```

The warehouses land exactly on their storage/junction `.way` nodes, their bases sit on the
tile ground (447.1) at `h = 450` — exactly the `.way` node height — and no assembled vertex is
below the ground.

### 5.2 `MENV/dr.b3d` (menu, the strictest test: 90°/45° anchors)

All 80 placements have `anchor_dev = 0.000` and `base-anchor_h = 0.00`; ground clearance
`+3.25…+9.00`; only 0.8 % of assembled vertices are below the ground level. The 4 tribunes /
town blocks land 5–8 units from the `finish1_*`/`finish2_*` `.way` nodes of the menu track,
and the 16 `DRZ*` lamp posts land on the circuit edges — visible in
`tools/out/b3dw_menv_dr.png`, where the assembled lamp posts (cyan, labelled) sit exactly on
the grey world-space room mesh of the circuit.

### 5.3 Corpus (`--all`, 42 `.b3d`: 40 `ENV`, 2 `MENV`, plus `MTR/tr`, `MTB/tb`)

```
files=42  links=2013  sub4=990  placements=1184 (after dedup of identical name+matrix)
sub4 self-anchor == sub18 link anchor: agree=1036 disagree=0
containers referenced by >1 link: 98   (10 of them link the same anchor twice -> duplicate records)
bounding sphere: radius error <= 0.211 ; mesh&r==0 -> 0 ; no-mesh&r>0 -> 2
worst anchor-vs-footprint deviation: 897.9 (ENV/dq.b3d, lep12)
```

The `dq.b3d` outlier is **data, not a rule failure**: `LepDQSpace` sits at `(−4000, −2500, 0)`
and the `lep*` containers are modelled away from their own origin (`lep12` local
`x 241..335, z 865..964` — which is also exactly what its bounding sphere centre `(287,914)`
records). The anchor reproduces the container origin; the geometry is simply offset. The
correct scale-free consistency metric is the bounding-sphere check (§4.3), which holds to
0.75 units mean over 1146 containers.

### 5.4 Images

| file | content |
|----|----|
| `tools/out/b3dw_aa.png` | whole `aa` tile: grey room/road quads, coloured assembled containers boxed and labelled, `.way` nodes, anchors, depth-byte contours |
| `tools/out/b3dw_aa_zoom.png` | zoom: `store_aa` with `node_store_1000..1004` beside it, `STO_AA` on the `STOS` footprint, `Combinat` + `mapSto_AAXXX` |
| `tools/out/b3dw_menv_dr.png` | the menu circuit with the 16 `DRZ*` lamp posts and 4 grandstands assembled onto the track |
| `tools/out/b3dw_menv_dr_zoom.png` | zoom on the start/finish grandstands and the `finish1_*`/`finish2_*` nodes |
| `tools/out/b3dw_dq.png`, `b3dw_dq_zoom.png` | `dq` tile: the multi-anchor `lep*` decorations + `DQZ*` lamp posts on the road, and the 897-unit "anchor at the container origin" case |
| `tools/out/b3dw_ab.png` | a tile with **zero** links: everything is world-space room content (proves rooms need no transform) |
| `tools/out/b3dw_ad.png` | the densest tile (77 links, 546 groups) |

---

## 6. Terrain caveat (**still open**)

`original/MTR/terrain0.raw` is 257×257 little-endian u16 whose high byte equals the low byte
(every sample is `b·257`), i.e. an 8-bit map. The previously recorded model
`worldY(t) = (157.6 − t)·5.136`, `sample = coord/140 + 128.5` is **not reproducible as a
height model**:

* it scores `r ≈ 0.91` only because the vertex set mixes two clusters (world geometry at
  `h≈448` over samples with depth ≈65–107, and untransformed *local* container geometry at
  `h≈0–46` over the sample at the tile origin) — and it "validates" the *untransformed* local
  geometry (median residual −1.0 for `Combinat`), which cannot be a real ground fit;
* against all 762 `.way` node heights of the 40 `ENV` tiles it gives `r ≈ 0.25–0.41` for
  every horizontal scale tried (20…200 units/sample), i.e. no fit;
* the ENV ground is a **flat plane** anyway: the road/ground quads of `ENV/aa.b3d` are flat
  child meshes at exactly `h = 447.059` (one at 447.572), and the buildings/props sit at
  `h = 450`, which is precisely the
  `.way` height. So the tile ground is a constant, not a terrain function of `(x,z)`.

Consequently the validator uses the tile's own world-space room geometry as the ground
reference (1st percentile = flat ground level, plus a local low-surface grid) and only
*reports* the depth-map residual for the record (`+204` median for `aa`, `−101` for `dr`).
**Decisive experiment still open:** render `terrain0.raw` as a heightfield with the camera
looking along the tile plane and compare with the assembled road quads — or find the
`terrain*.raw` consumer in `king.exe` (search for the reader of `MTR/terrain%d.raw` and the
`140`/`256`-sample constants). Until then, treat `terrain*.raw` as a *depth buffer*, not
authoritative heights.

---

## 7. How the C++ engine should apply it (port notes for `src/world/B3D.cpp`)

**Matrix push/pop.** Only three node kinds change the matrix, and all of them push the *same*
kind of matrix (12 floats, rows of 3): a `subtag 24` space pushes its own 4×3; `subtag 4`
instances and `subtag 18`-referenced `subtag 5` groups push the **absolute** matrix of their
anchor (`abs = local · parentAbs`, computed once per space node by walking the record-tree
parent chain, never re-derived per frame). Children of a space inherit it (compose as
`child_abs = child_local · parent_abs`, row-vector order — local first), while `subtag 19`
rooms, `subtag 40` points and any container with no anchor/link get **identity** (room content
is already world space). `subtag 30` portals and the 4-float bounding spheres never push a
matrix: the portal's matrix is derived by the engine from its two endpoints, and the sphere is
culling metadata consumed after placement. The transform application itself must be
`p_world = p · M + t` with `M` the nine floats in file order (row-major) — i.e. `x' = x·m0 +
z·m3 + h·m6 + t0`, `z' = x·m1 + z·m4 + h·m7 + t1`, `h' = x·m2 + z·m5 + h·m8 + t2`; normals get
the same 3×3 without `t`. **The currently shipped convention (`M·p + T`) is the transpose of
this and is numerically identical only for the symmetric anchors of the `aa` tile
(identity/180°); it will mirror chiral objects (lamp posts, signs, gates) at the 90°/45°
anchors used by `dr`, `ac`, `ad`, `dq`, …** A/B-testable with
`tools/b3d_world.py <tile> --convention row|col` and the PNG output.

**Placement list.** Build `links: B → [(A, sphere, file_order)]` from `subtag 18`; for each
container (`subtag 5` or `subtag 4`) emit one instance per link, plus (if you honour the
built-in `subtag 4` anchor name) skip it when the link already produced the same
`(container, matrix)`. Containers with no anchor are drawn with the parent transform. Chain
into the existing hand-written port (no autogenerated pseudocode is worth keeping).

---

## 8. Proven vs assumed

| claim | status | evidence |
|----|----|----|
| record layouts (§1) | **proven** | readers `0x5f2640/0x5f4340/0x5f9850/0x5fc360`, dispatch table `0x5f0614`, 100 % byte consumption in two independent parsers |
| 12 floats = 4 rows of 3, row-major, file order | **proven** | `0x1aba10` reads 4×(3 floats) into `+0x40` |
| `p_world = p·M + t` (row vector) | **proven** | point transform `0x1f2317..0x1f238a`; product `0x1aba40` = `src·dst`; composition `0x1abc10` in `GetAbsSpace` |
| `abs = local · parentAbs` | **proven** | `0x1f25c0` + `0x1abc10`/`0x1aba40` |
| rows are (x, z, height) | **proven** | translation passed verbatim + `.way` match (`STO_AA` = 4750/−5675/450) |
| `24` = anchor, type 6 | **proven** | `0x1efdad`, `0x1f19b0(…, 6, 1)`, `GetAbsSpace` asserts `getType()==6` |
| `18` = "place B at A", applies `GetAbsSpace(A)` | **proven** | `0x5f9850` (names → `+0x40/+0x44`), `0x5f98c0` (`+0x44` → `0x1f25c0` → `0x1ae580`) |
| `4` = self-anchored instance, anchor name at param dword 4 | **proven** | `0x5f4340` (name1 → `+0x3c`), `0x5f43d0` (`+0x3c` → `0x1f25c0`) |
| `4`'s anchor is redundant with the `18` link | **proven (data)** | 990 instances, all covered by links, 1036 comparisons, 0 disagreements |
| links apply to the whole container subtree | **proven** | the matrix is pushed for the instance whose children are the meshes (`0x1f1db0` child parse; `0x5f98c0` per-instance) |
| 4 floats = local bounding sphere, radius 0 ⇒ not rendered | **proven (numeric+code)** | radius = max vertex distance ≤0.21 (1146 containers); `0x1b4fc0` reads `+0xc` as radius and sets the cull state; 0 mesh-with-r0, 2 no-mesh-with-r>0 |
| multi-anchor emit order = file order, each link an independent copy | **proven (data)** | 98 multi-link containers; duplicates (same anchor twice) exist in the data and are pure overdraw |
| `30` = room portal, not a transform | **proven** | `0x5fc360` reads 4f + name + 2×3f and builds a basis; radius = 1.001·\|p1−p0\|/2, centre = (p0+p1)/2 (2309 portals, ≤1.2e-4) |
| `case` float is a state, not a scale | proven read, **assumed** semantic | `0x5f2652` → `+0x38`; `GetAbsSpace` switches on `+0x38` |
| `terrain*.raw` is a heightmap | **assumed / contradicted** | §6 |

---

## 9. Tool

```
python3 tools/b3d_world.py original/ENV/aa.b3d            # report + PNGs
python3 tools/b3d_world.py original/MENV/dr.b3d --convention col   # A/B the convention
python3 tools/b3d_world.py --all                          # corpus verification
python3 tools/b3d_world.py <b3d> --way <way> --terrain <raw> --out tools/out --topview
```

`b3d_world.py` contains its **own** record walker (it does not import `tools/b3d_full.py`), so
the two parsers cross-check each other; it reports the byte-consumption percentage,
per-anchor local+absolute matrices, per-container world bboxes, the anchor/footprint
deviation, nearest `.way` node, ground clearance, the bounding-sphere check and the corpus
adversarial test described above.

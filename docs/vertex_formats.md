# B3D mesh vertex formats (subtag 7 / 37)

Reverse-engineered from `original/king.exe` (PE32, ImageBase `0x400000`) and
checked against every B3D shipped with the GOG release
(39 `original/ENV/*.b3d` tiles + `MENV/dr.b3d`, `MTR/tr.b3d`, `MTB/tb.b3d`,
`COMMON/{TRUCKS,COMMON,CABINES}.B3D`).

> Address convention: the addresses below are **virtual addresses** as `r2`
> prints them for the PE (`r2 -NN -q -e asm.arch=x86 -e asm.bits=32 …`).
> Subtract `0x400000` for the raw file offset used by
> `docs/msk_format.md` / `tools/res_msk.py`.

Reproduce the data side with `python3 tools/b3d_vfmt.py slots` (per-slot
statistics) and `python3 tools/vfmt_uvsets.py` (per-edge UV evidence).

---

## TL;DR — the table

`stride` = floats per vertex. Offsets are 0-based float indices *within* the
vertex record. "UV" means the pair the original engine feeds to texture stage 0
(`u = offset`, `v = offset+1`).

| fmt | stride | meshes | verts | position | normal | uv (set 0) | extra sets | confidence | evidence |
|---|---|---|---|---|---|---|---|---|---|
| `0x00` (sub 37) | 5 | 4011 | 0 | 0 | — | **3** | — | confirmed | loader `0x5f2800`; empty in the shipped data (all `vc == 0`) |
| `0x00` (sub 7, no fmt dword) | 5 | 615 | 20042 | 0 | — | **3** | — | confirmed | subtag-7 handler forces `fmt = 0`; `(f3,f4)` in `[0,1]²` for 70 % of the verts, unit triples nowhere |
| `0x02` | 8 | 35356 | 680044 | 0 | **5** | **3** | — | confirmed | `(f5,f6,f7)` is unit-length in **99.2 %** of 680 k verts (the only remaining 3 floats) |
| `0x03` | 6 | 2307 | 17427 | 0 | `5` (1 float only) | **3** | — | uv confirmed, normal inferred | `f5 ≡ 1.0` for **100 %** of 17 427 verts; `(f3,f4)` has 147/149 distinct values and lives in `[0,1.09]` (truck UVs); a waterfall mesh maps `(f3,f4)` = (0.4998,0.4998)…(0.0002,0.3055) |
| `0x102` | 10 | 1128 | 139950 | 0 | **7** | **3** | set 1 = **5** | confirmed | `(f7,f8,f9)` unit-length in **99.5 %**; loader reads `pos(3) + uv0(2) + 1 pair + normal(3)` |
| `0x202` | 12 | 219 | 31994 | 0 | **9** | **3** | set 1 = **5**, set 2 = **7** | confirmed | `(f9,f10,f11)` unit-length in **100 %** of the 31 994 verts; loader reads 2 pairs |
| `0x203` | 10 | 2 | 42 | 0 | `9` (1 float only) | **3** | set 1 = **5** | uv confirmed | `stride = 10` is the only value that lands on a valid record end (`_ok_end` true only for 10); `f9 ≡ 1.0` |

**Recommended offsets: `position = 0`, `uv = 3` (u) / `4` (v) for *every*
format, `normal = 5` (0x02), `7` (0x102), `9` (0x202), no normal for `0x00`,
one float at `5` (0x03) / `9` (0x203).**

The one *wrong* entry in the current C++ is **fmt `0x03`: it returns 4, the
correct value is 3.** With `uv = 4` the `v` component is the constant `1.0`
(see §4), i.e. the texture collapses to one texel row — the classic
smeared/striped look. This affects 2307 meshes / 17 427 vertices, including
794 of the 28 596 mesh records in `TRUCKS.B3D`.

---

## 1. Where the layout comes from (king.exe)

### 1.1 Dispatcher

* `0x5ef830` — B3D record parser. Token compares at
  `0x5ed819` (`0x6f` open), `0x5ed825` (`0x22b` 555), `0x5ed835` (`0x1bc` 444),
  `0x5ed83c` (`0xde` EOF), `0x5ed847` (`0x14d` 333).
* `0x5ef877` — `cmp eax, 0x29; jmp [eax*4 + 0x5f0614]`: the **subtag jump
  table**. `subtag 37 -> 0x5efc35`, `subtag 7 -> 0x5efbda`, `subtag 35 -> 0x5ef8d9`,
  `subtag 8 -> 0x5ef8a2`, `subtag 40 -> 0x5ef914`, `subtag 19 -> 0x5f00e2`.
* Both the subtag-7 and the subtag-37 handler install the same mesh class
  vtable `0x6515dc`. Its `[+0x2c]` slot (index 11) is the payload parser,
  **`0x5f2800`** (called generically at `0x5f05f9`).
* The subtag-7 handler stores `[obj+0x6c] = 0`; the subtag-37 handler stores
  `[obj+0x6c] = 1` (edi is 1 in that function, `0x5ef855`). That flag is the
  "there is an explicit fmt dword in the stream" flag (see 1.2).

### 1.2 Format decoding — `0x5f2800`

```
0x5f2850  mov eax,[esi+0x6c]      ; fmt-seen flag
0x5f2855  cmp eax,edi (0) / je     ; subtag 7 -> no fmt dword, fmt stays 0
0x5f285a  call 0x5ab140           ; read u32 fmt        <- subtag 37 only
0x5f2862  mov [esi+0x6c],eax
0x5f286b  and eax,0xff            ; LAYOUT  = fmt & 0xff        -> [obj+0x6c]
0x5f2870  sar ebp,8               ; NPAIRS  = fmt >> 8          -> ebp
0x5f2876  call 0x5ab140           ; read u32 vcount             -> [obj+0x38]
0x5f2897  new(28*vcount+4)        ; per-vertex record, 28 bytes -> [obj+0x3c]
0x5f28f3  new(12*vcount+12)       ; 3 floats per vertex         -> [obj+0x40]
0x5f290b  (zero-fill the 3-float array)
```

and then **one iteration per vertex**, in this exact file order:

| step | code | file floats consumed | goes to |
|---|---|---|---|
| 1 | `0x5f2947 call 0x5e56f0` | **3** | vertex `+0x00,+0x04,+0x08` = position |
| 2 | `0x5e56f8 call 0x5ab790` | | |
| 3 | `0x5e56fe/0x5e5704` | **1** | vertex `+0x0c` |
| 4 | `0x5e5707/0x5e570d` | **1** | vertex `+0x10` |
| 5 | `0x5f294e jle` + loop `0x5f2974` | **2 × NPAIRS** | separate array at `vertex+0x14`, count at `+0x18` |
| 6a | `0x5f29ac/0x5f29b4` layout **1 or 2** | **3** | 3-float array `[obj+0x40] + i*12` |
| 6b | `0x5f29b6/0x5f29bb` layout **3** | **1** | `[obj+0x40] + i*12` (only the first float) |
| 6c | layout **0** | 0 | — |

`stride = 3 + 2 + 2*NPAIRS + (3 if layout∈{1,2} else 1 if layout==3 else 0)`,
which reproduces every stride observed in the data **exactly**:
`0x00 -> 5`, `0x02 -> 8`, `0x03 -> 6`, `0x102 -> 10`, `0x202 -> 12`,
`0x203 -> 10`.

The sub-7 reader `0x5e56f0` is a fixed 3 + 2 float reader, which is why subtag-7
vertices are `pos[3] + uv[2]` even though the format id is implicit.

### 1.3 What the two "extra" blocks are

* **Step 5** (`vertex+0x14`, 8 bytes per pair) is the u/v pair list for the
  *extra* texture coordinate sets: pair *n* = `(float[3+2+2n], float[4+2+2n])`.
* **Step 6** (`[obj+0x40]`, 12 bytes per vertex, zero-initialised for every
  vertex) is the **normal** array. For layout 1/2 all three components come
  from the file; for layout 3 only `normal.x` is stored and `y = z = 0`. The
  proof that it is a normal is statistical: the 3 floats it consumes are
  unit-length in 99.2 % / 99.5 % / 100 % of the vertices of fmt
  `0x02` / `0x102` / `0x202` respectively, and that is the only per-vertex data
  left in those records.

### 1.4 Confirmation from the consumer side

* `0x5f26a0` (mesh vtable slot `[+0x30]`) copies
  `mesh+0x3c` (vertex array) to `drawctx+0x203c`, `mesh+0x40` (normals) to
  `drawctx+0x2040`, `mesh+0x38` (vcount) to `0x723a48`, layout `mesh+0x6c` to
  `0x723a50`.
* `0x5af9e0` (per-vertex emitter) walks the **28-byte** records
  (`0x5b007f add eax,0x1c`) and for the static case reads the texcoords from
  `[vertex + i*28 + 0xc]` and `[+0x10]` (`0x5afb45 fld [eax+ecx+0x10]`,
  `0x5afb4f fld [eax+0xc]`) — i.e. **file floats 3 and 4** — and writes them
  into the destination vertex at byte `+0x1c` / `+0x18` (`0x5b0049`,
  `0x5b0050`) with a destination stride of **32 bytes** (`0x5b0074
  add esi,0x20`). 32 bytes = XYZ(12) + NORMAL(12) + TEX1(8), i.e. consistent with FVF
  `0x112`, and the texcoord slot is bytes 24..31.
* The u/v order inside the pair: `u = float 3`, `v = float 4`. In the same
  emitter the material's `move` vector is added component-wise as
  `float3 += [mat+0x50]`, `float4 += [mat+0x54]` (`0x5afba7`–`0x5afbbe`), and
  the material text parser stores `move <a> <b>` into `[mat+0x50]` then
  `[mat+0x54]` in that order (`0x5ebe63`–`0x5ebe8f`). The material syntax is
  `move <u> <v>` (`mat_water tex 14 move 0.02 0 att 52 "wave"`), therefore
  float 3 is `u`.

### 1.5 Texture coordinate *sets* and the material `coord N` keyword

* `0x5b04e0`–`0x5b04fb`: `SetTextureStageState(stage, D3DTSS_TEXCOORDINDEX(11),
  stage)`: **stage N uses texcoord set N** (stage 0 -> set 0, stage 1 -> set 1).
  The device also gets `SetTexture(2, NULL)`/`SetTexture(4, NULL)` cleanup at
  `0x5aeab8`-`0x5aeb00`, but no `TEXCOORDINDEX` is ever set for stage ≥ 2.
* The `.res` `MATERIALS` section is a **text list**
  (`<name> <type> <index> [options]`), e.g. from `ENV/aa.res`:
  ```
  mat_refl_water tex 14  env 0.07 0.07  notile  coord 3
  mat_bump_water tex 15  coord 2  att 55
  mat_water      tex 16  att 56  "wave"
  mat_road4      tex 22  col 5  usecol  att 46
  ```
* `coord N` is parsed at `0x5ebd14`-`0x5ebd33`:
  ```
  0x5ebd29  call 0x61fc9d     ; atoi(token)
  0x5ebd32  dec eax           ; 1-based -> 0-based
  0x5ebd33  mov [ebp+0x30],eax ; material->texcoordSet
  ```
  so `coord 1/2/3` = set **0/1/2**, and materials **without `coord` use set 0**
  (the renderer falls back to 0 when it does not take the value from the
  material: `0x5b0895 xor edi,edi`, then `0x5b08fc push edi; 0x5b08fd push 0xb;
  0x5b08ff push esi; … SetTextureStageState(stage=esi, type=11, value=edi)`).
* Consequence: every `mat_road*`, `mat_grass*`, `mat_zebra*`, `mat_gravel*`,
  `mat_water` (no `coord`) uses **set 0 = floats 3,4**. Only
  `mat_bump_water` (`coord 2` -> set 1) and `mat_refl_water` (`coord 3` ->
  set 2) use the extra sets, which is exactly why the water meshes carry 2
  (`0x102`) or 3 (`0x202`) pairs.

---

## 2. Census over the shipped data

45 B3D files, 43 638 mesh records, 889 499 vertices.

| subtag | fmt | stride | meshes | verts | files |
|---|---|---|---|---|---|
| 7 | `0x000` (implicit) | 5 | 615 | 20 042 | 33 |
| 37 | `0x000` | 5 | 4 011 | 0 | 20 |
| 37 | `0x002` | 8 | 35 356 | 680 044 | 45 |
| 37 | `0x003` | 6 | 2 307 | 17 427 | 38 |
| 37 | `0x102` | 10 | 1 128 | 139 950 | 38 |
| 37 | `0x202` | 12 | 219 | 31 994 | 17 |
| 37 | `0x203` | 10 | 2 | 42 | 1 |

39 ENV tiles only:

| subtag | fmt | stride | meshes | verts |
|---|---|---|---|---|
| 7 | `0x000` | 5 | 563 | 18 761 |
| 37 | `0x000` | 5 | 91 | 0 |
| 37 | `0x002` | 8 | 10 683 | 376 325 |
| 37 | `0x003` | 6 | 1 414 | 10 407 |
| 37 | `0x102` | 10 | 1 070 | 130 383 |
| 37 | `0x202` | 12 | 169 | 21 666 |
| 37 | `0x203` | 10 | 2 | 42 |

`0x203` appears only in `ENV/ch.b3d` (`refer_TextureSizeKey`, 2 meshes × 21
verts) and is also referenced by `mat_river_0_waterfall_14`. No other format id
exists in the shipped data. Note that `nfm()`'s `strideOf()` fallback finds
`10` for `0x203` by luck (only `s = 10` lands on a valid record end), but it
should be listed explicitly.

### Material vs. format (why the offsets must be chosen by format)

The same material is used with several formats, e.g.

| material | fmt `0x2` | fmt `0x102` | fmt `0x0` |
|---|---|---|---|
| `mat_grass1` | 25 183 polys | 6 171 polys | — |
| `mat_roadgrn` | 1 279 | 52 | 256 (sub 7) |
| `mat_zebra` | 8 | 76 | 3 (sub 7) |
| `mat_water` | — (`mat_water1..4`, 32 each) | 3 615 | — |
| `mat_road*` | — | 52 637 | — |

Everything with `road` in the name is fmt `0x102` except `mat_roadgrn`
(also `0x2` and sub-7 `0x0`); the road *markings* are `mat_zebra`/`mat_zebrabw`
(`0x2` and `0x102`).

---

## 3. Ground truth from the data

### 3.1 A real road mesh (`ENV/bg.b3d`, `/room_bg00/obj_bg00/show_bg00/`, fmt `0x102`, `vc=1407`, materials `mat_road4` / `mat_store_*`)

```
pos(800.00000, 3406.50000, 2.00000)  f3=0.99800 f4=0.00000  f5=-5.00000 f6=-3.37500  f7..f9=(0,0,1)
pos(809.69293, 3406.24536, 2.02437)  f3=0.99800 f4=1.00000  f5=-2.57677 f6=-3.43866  f7..f9=(0,0,1)
pos(800.00000, 3412.50000, 2.00000)  f3=0.00200 f4=0.00000  f5=-5.00000 f6=-1.87500  f7..f9=(0,0,1)
pos(819.30426, 3405.49951, 2.09626)  f3=0.99800 f4=2.00000  f5=-0.17393 f6=-3.62512  f7..f9=(0,0,1)
```

* `f3` runs 0.002…0.998 **across** the road (one texture repeat over the road
  width), `f4` counts 0,1,2,… **along** the road (~9.7 world units per repeat).
  This is the authored "road ribbon" mapping and it is the pair at offsets 3,4.
* `f5,f6` is an independent uniform planar projection: `f5 = 0.25·(x-820)`,
  `f6 = 0.25·(y-3420)`, i.e. **4.000 world units per UV unit** (measured
  `median(|dPos|/|dUV|) = 4.000`, `p90/p10 = 1.0` over 57 601 road edges). It is
  texcoord set 1 and is *not* the material's set (no `coord` in `mat_road*`).
* `f7,f8,f9` is the normal.

### 3.2 fmt `0x102` with `mat_grass1`

`median(|dPos|/|dUV|)` for pair `(5,6)` is undefined with `dead = 0.997`
(99.7 % of 17 318 edges have `|Δuv| < 1e-6` while `|Δpos| > 0.5`): the second
pair of those records is constant/zero. Reading `(5,6)` there would smear the
texture completely — a concrete example of why the pair must be selected by
material if you want sets other than 0.

### 3.3 fmt `0x202` = water (3 sets)

The 169 ENV meshes of fmt `0x202` are dominated by
`mat_water`/`mat_river*` (only 12 stray polys use `mat_grass1/3` with this
format); per-edge scale over 49 956 water edges:

| pair | world units per UV unit | spread `p90/p10` |
|---|---|---|
| `(3,4)` set 0 | 6.667 | 1.0 |
| `(5,6)` set 1 | 40.000 | 10.0 |
| `(7,8)` set 2 | 2.000 | 1.0 |

`mat_water` has no `coord` → set 0 (`0x102` water meshes use 2 sets, `0x202`
water meshes use 3 sets), `mat_bump_water` `coord 2` → set 1,
`mat_refl_water` `coord 3` → set 2. The three sets are the same projection at
three scales (measured on `/room_aa_000///`: `Δset1 = 0.05·Δset2`,
`Δset0 = 0.3·Δset2`), which is what a base/bump/reflection multitexture needs.

### 3.4 fmt `0x03` is `pos + uv + 1.0` (the current C++ is wrong)

* `f5 ≡ 1.000` for **all** 17 427 fmt-`0x03` vertices (slot 5 has exactly one
  distinct value in `TRUCKS.B3D`, `ENV`, `COMMON`).
* `(f3,f4)` is a real UV pair:
  `ENV/ch.b3d` `mat_waterfall_14`: `(0.4998,0.4998) (0.4998,0.3055) (0.2500,0.3055) (0.2500,0.1456) …`
  `COMMON/TRUCKS.B3D`: 6 407 verts, slot 3 and slot 4 have 147/149 distinct
  values inside `[0,1.09]`.
* With the current `uvOffsetOf` (`0x3 -> 4`) the sampled `v` is `f5` = 1.0 for
  every vertex, i.e. every texel is taken from one row of the texture — the
  exact "smeared" failure mode.

### 3.5 fmt `0x00` (subtag 7) and fmt `0x02`

Subtag 7 (e.g. `mat_lit_window`, 8856 polys): `(f3,f4)` is in `[0,1]²` for
69.7 % of the sub-7 vertices and `|f3|,|f4| ≤ 1` for 84 %, 88 %; there is no
room for a normal (stride 5). fmt `0x02`: `(f3,f4)` = UV, `(f5,f6,f7)` =
normal. On `mat_grass1` the UV scale is 22.22 world units per repeat
(`p90/p10 = 1.9`), on `mat_grass` 20.02, on `mat_fence1` 2.41, etc.

---

## 4. Recommended code change (`src/world/B3D.cpp`)

```cpp
// floats per vertex for subtag 37 vertex formats (0x0 -> 5)
static uint32_t nfm(uint32_t fmt) {
    switch (fmt) {
        case 0x0:   return 5;
        case 0x2:   return 8;
        case 0x3:   return 6;
        case 0x102: return 10;
        case 0x202: return 12;
        case 0x203: return 10;   // NEW: refer_TextureSizeKey / waterfall objects
        default:    return 0;
    }
}

// The primary (stage-0) texture coordinates are the two floats directly after
// the position in EVERY format: pos[3] | uv[2] | [n x uv[2]] | normal[3] or w.
static int uvOffsetOf(uint32_t sub, uint32_t fmt, uint32_t stride) {
    (void)sub; (void)fmt;
    return stride >= 5 ? 3 : -1;          // fmt 0x3: was 4, must be 3
}

// Extra texture coordinate sets, in engine order (material `coord N`, 1-based).
static int uvSetOffset(uint32_t fmt, uint32_t set) {
    switch (fmt) {
        case 0x0: case 0x2: case 0x3: return set == 0 ? 3 : -1;
        case 0x102: return set <= 1 ? 3 + 2 * (int)set : -1;  // 3, 5
        case 0x202: return set <= 2 ? 3 + 2 * (int)set : -1;  // 3, 5, 7
        case 0x203: return set <= 1 ? 3 + 2 * (int)set : -1;  // 3, 5
        default:    return -1;
    }
}

// Stored normals.  The C++ currently derives smooth normals from the geometry
// (B3D.cpp "per-mesh smooth normals"), which is fine; this is only needed if
// you want the authored ones.
static int normalOffsetOf(uint32_t fmt, uint32_t stride) {
    switch (fmt) {
        case 0x2:   return stride >= 8  ? 5 : -1;
        case 0x102: return stride >= 10 ? 7 : -1;
        case 0x202: return stride >= 12 ? 9 : -1;
        // 0x3 / 0x203 store a single float which the loader puts in normal.x
        // (normal.y = normal.z = 0); in the shipped data it is always 1.0.
        // 0x0 stores no normal at all.
        default:    return -1;
    }
}
```

Practical notes:

* **Do not select the offset by material** — the same material appears with
  several formats (§2). Always select by `fmt` (and `sub == 7` ⇒ `fmt = 0`).
* If the renderer only carries one UV set (as the current emitter does), set 0
  at float 3 is what the engine itself uses for every material without `coord`
  — including all road, grass, gravel, zebra, fence and building materials.
* Supporting `coord` is optional and only matters for water: parse the tile's
  `.res` `MATERIALS` text section, take `coord N` (1-based, default set 0) and
  feed `uvSetOffset(fmt, N-1)` to the second/third sampler.

---

## 5. Ambiguities and how to handle them

1. **`0x03` / `0x203`: what is the trailing single float?** It is definitely
   *not* a UV (constant `1.0` in 100 % of the shipped vertices). The loader
   writes it into `normal[0]` of a zero-filled 3-float array, i.e. the engine
   sees the normal `(1,0,0)`; it might also be a homogeneous `w = 1` that the
   loader happens to park in that array. The offset (5 for `0x03`, 9 for
   `0x203`) and the fact that it is one float are *confirmed*; its meaning is
   *inferred*. Recommendation: ignore it, keep the computed geometric normals,
   and never use it as a texture coordinate.
2. **Which UV set a mesh actually renders with** cannot be decided from a B3D
   alone: the choice lives in the `.res` MATERIALS text (`coord N`, 1-based,
   default 0). For set 0 there is no ambiguity; for sets 1/2 the mapping is
   confirmed by `mat_bump_water coord 2` / `mat_refl_water coord 3` and by the
   water meshes carrying exactly 2/3 pairs.
3. **I did not find a destination vertex buffer with more than one texcoord.**
   The emitter I traced (`0x5af9e0`) always uses a 32-byte / `XYZ|NORMAL|TEX1`
   destination and writes set 0 only. Whether sets 1/2 reach D3D through a
   different (non-traced) path or are consumed by a custom lighting/water
   routine is unresolved. This does not affect any of the offsets above.
4. **Layout id 1 is treated exactly like layout id 2** by the loader
   (3 floats into the normal array) but never occurs in the shipped data, so no
   distinction can be observed. If an unknown format id shows up, decompose it
   as `layout = fmt & 0xff`, `pairs = fmt >> 8`,
   `stride = 5 + 2*pairs + (3 if layout in (1,2) else 1 if layout == 3 else 0)`.
5. **`0x00` in subtag 37** always has `vc == 0` in the shipped data (placeholder
   records). `okEnd(voff)` is false for those records, but the C++'s
   `strideOf()` returns 0 and `if (alt) nf = alt;` keeps the table value 5, so
   parsing continues correctly. Consider skipping `vc == 0` meshes explicitly
   anyway (no vertex data is read for them either way).
6. **Polygon records sometimes carry per-corner attributes** (see §7). The
   vertex-format table above is unaffected, but the *polygon* decoder is a
   separate (and currently not fully correct) piece of this format.

---

## 6. If road textures still look smeared after this fix

The current `uvOffsetOf` already returns 3 for fmt `0x102`, which is exactly
what the original engine feeds to texture stage 0 for `mat_road*`
(`coord` absent ⇒ set 0). So for the *roads* the pair selection is not the bug.
Things worth checking next, in order:

1. **Polygon payload stride (§7).** With the current
   `s = (dw % cnt == 0) ? dw/cnt : 1` rule, **15 905 of 428 865 polygons**
   decode to out-of-range vertex indices and are then dropped/degenerated by
   `emit()`. All of them are on subtag-7 / fmt `0x0` meshes, and the materials
   involved include road/ground-ish ones: `mat_roadgrn` (222 polys, stride 4),
   `mat_troad4` (186, s4), `mat_gravel` (185, s6), `mat_slope1/2` (s4),
   `mat_stog` (734, s4), `stos_doorl` (s4/s6), `mat_lit_window` (s2/s3/s4).
   The corrected rule in §7 makes that count **0**. Reproduce with
   `python3 tools/vfmt_polys.py`.
2. **fmt `0x03` meshes** (2307 records, all trucks/windows/waterfalls/road
   props): the current UV offset 4 makes `v ≡ 1.0`, which looks like smearing
   and is a real bug — fixed by the change in §4. Same for `0x203`.
3. **UV set / texture stage**: the engine sets
   `TEXCOORDINDEX = stage` and uses `SetTexture(0/1/2/4, …)`; every material
   without `coord` (all `mat_road*`, `mat_grass*`, `mat_zebra*`, `mat_gravel*`)
   uses set 0 = floats `3,4`. If the re-implementation binds the asphalt to
   stage 1 (where the engine would put set 1 = the 4 m/repeat planar
   projection), the road *will* look wrong even with correct offsets.
4. **Wrap/addressing and `att N`**: `mat_road4` is
   `mat_road4 tex 22 col 5 usecol att 46`. The `att NN` attribute index is not
   decoded yet; it plausibly selects the sampler/wrap/scale behaviour of the
   road material. Compare against an `att`-table dump before blaming the UVs.
5. **`v[5],v[6]` are not universally valid**: on fmt `0x102` `mat_grass1`
   meshes 99.7 % of the edges have constant `(5,6)` (measured), so a renderer
   that blindly prefers set 1 would smear those meshes completely.

---

## 7. Follow-up: the polygon payload stride (a real decoder bug)

Found while validating the UV offsets. It does **not** change any entry of the
vertex-format table, but it does change which *vertices* (and therefore which
UVs) a polygon uses, so it must be fixed before blaming the vertex layout for
anything.

A polygon record is `{ flags, 1.0f, 0x7fff, material, cnt, payload }`. The
payload is `cnt` **interleaved per-corner entries**; the entry stride is 1, 2,
3, 4 or 6 dwords, and the entries with stride > 1 carry per-corner data after
the index:

| stride | entry layout | polygons in the shipped data |
|---|---|---|
| 1 | `{ idx }` | 407 340 |
| 2 | `{ idx, w }` — the second dword is bit-identical to `0x3f800000` (1.0) in every entry | 2 625 |
| 3 | `{ idx, u, v }` — a clean example is `ENV/aj.b3d` `mat_h5`: `{24,0,0} {23,0,1} {25,1,1} {26,1,0}` (a quad with UV corners) | 7 140 |
| 4 | `{ idx, a, b, c }` — looks like a normal or texgen tuple, not decoded | 9 654 |
| 6 | `{ idx, u, v, nx, ny, nz }` — e.g. `{66, 9.0, 0.1914, 1, 0, 0}` | 2 106 |

Because the record boundary is found by scanning for the next `{1.0f, 0x7fff}`
pair, the dword count measured between two headers is `cnt*s + 1` for every
polygon except the last one of a face, where it is exactly `cnt*s`. Hence:

```cpp
uint32_t s;
if (cnt && (dw - 1) % cnt == 0 && (dw - 1) / cnt >= 1)      s = (dw - 1) / cnt; // + trailing dword
else if (cnt && dw % cnt == 0 && dw / cnt >= 1)             s = dw / cnt;       // last poly of a face
else                                                        s = 1;
// corner k's vertex index = u32(payload + 4*s*k)
```

Evidence that this is right and the current rule is wrong:

* with this rule **0 of 428 865** polygons produce a vertex index `>= vcount`;
* with the current `s = (dw % cnt == 0) ? dw/cnt : 1` rule **15 905** polygons
  produce out-of-range indices (all of them subtag 7 / fmt `0x0`), e.g.
  `(dw=13, cnt=3)` 6 956 polys, `(dw=10, cnt=3)` 4 436, `(dw=7, cnt=3)` 2 054.
  `emit()` then drops those triangles (`if (vi >= m.vcount) continue`) or
  degenerates them, so both the geometry and the UVs of those polygons are
  wrong.

Every stride>1 polygon belongs to a **subtag-7 / fmt `0x0`** mesh, so no
`0x2/0x3/0x102/0x202/0x203` mesh's indices are affected by the bug itself. But
the materials carried by those sub-7 polygons include road/ground-ish ones:
`mat_roadgrn` (222 polys, stride 4), `mat_troad4` (186, s4), `mat_gravel` (185,
s6), `mat_slope1/2` (s4), `mat_stog` (734, s4), `stos_doorl` (s4/s6),
`mat_lit_window` (2 254/3 474/2 968 for s2/s3/s4), `mat_h*`, `mat_store_*`,
`tab_*`, `combinat_*`. If those meshes are rendered with textures, the
mis-decoded indices are a plausible source of smeared/stretched surfaces.

Reproduce with `python3 tools/vfmt_polys.py` (prints the stride histogram and
the out-of-range index count for both rules).

Open question: for stride 3/6 the entry's `{u,v}` disagrees with the
vertex-record UV for most corners (47.6 % agreement for stride 6, 0.8 % for
stride 3 under the corrected boundary rule), so there are two candidate UV
sources for those polygons. Which one the engine uses needs a dedicated look at
the face-class parse/emit code.

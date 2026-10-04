# Texture transparency in *Hard Truck 2: King of the Road* (`king.exe`, 2003)

**Answer in one line.** A `.txr` texture carries **no** transparency information at all.
The engine puts **one 16-bit colour key** on the DirectDraw surface of a texture, and that
colour is looked up from the shared palette using the colour index written in the `.res`
`MATERIALS` table:

```
material value "ttx <N> [col <C>]"   →   texture #N of that .res is colour-keyed
                                          key = RGB565( PALT[C-1] )   (C defaults to 1 → black)
```

The key is applied with `IDirectDrawSurface7::SetColorKey(DDCKEY_SRCBLT, {k,k})` where

```
k = ((R & 0xF8) << 8) | ((G & 0xFC) << 3) | (B >> 3)      ; king.exe @ 0x1db551..0x1db56b
```

and `PALT` is the first loaded palette file (`COMMON.RES` → `common.plm`, 256 × RGB bytes).

Concrete example, verified numerically: `ENV/aa.res` → `tree0 ttx 3 col 256`, and
`common.plm` entry 255 = `(50,59,18)` (bytes `32 3B 12`) → `k = 0x31C2`. `txr\tree0.txr` is
filled with exactly `0x31C2` around the foliage (1382 of 4096 pixels = 33.7 %). Rendering it
with `0x31C2` → alpha 0 gives a clean tree cut-out (see *How to verify*).

All file offsets below are `king.exe` file offsets; the runtime VA is `offset + 0x400000`
(image base, `.text` is mapped 1:1 at RVA 0x1000). The radare2 command used was
`r2 -q -e scr.color=0 -n -e asm.arch=x86 -e asm.bits=32 -c 'pd N @ 0x…' original/king.exe`.

---

## 1. The texture loader — what it does and does **not** do

### 1.1 Blob variants

| kind | who | header | pixels |
|---|---|---|---|
| `txr/LOFF` | all `ENV/*.res`, `COMMON.RES`, `MENV/*.res` (12057 blobs) | `u16 12, u16 2, u16 0, u16 0x1000, u32 0, u16 w, u16 h, u16 0x2010`, then the chunk `"LOFF"` | 16 bpp RGB565, no alpha |
| `txr/8bpp` | the 172 `LOAD.RES` menu bitmaps | `u16 0x0100, u16 0x0001, …, u16 0x2008` + 256 BGR triples | 8 bpp indices, index 0 = transparent *by convention* |

The LOFF-header fields are constants in the shipped data (only `w`/`h` vary), so nothing in
the header is a key: I dumped all of them for the tree/glow/fence/gate families and they are
byte-identical (`0c 00 02 00 00 00 00 10 00 00 00 00 <w> <h> 10 20 "LOFF"`).

### 1.2 The LOFF/chunk reader (`0x1e7930`)

Reached from `0x1e78e0` ("build a texture object from a file"), which opens the blob and calls
the chunk walker. Verified inside `0x1e7930`:

```
0x1e7a6b  call 0x21d6f0 + push 0x683484      ; "LOFF"
0x1e7b01  …                  push 0x683458   ; "LVMP"
0x1e7b3a  …                  push 0x683450   ; "PFRM"
0x1e7cf1  …                  push 0x683428   ; "ENDR"
0x1e7b83  (PFRM matched) reads 4 u32 masks via 0x1ab140:
0x1e7b93  1st = R mask, 0x1e7b9e 2nd = G mask, 0x1e7ba6 3rd = B mask, 0x1e7bae 4th = A mask
0x1e7e08  mov word [edi+0x14], bp     ; texture->width
0x1e7e0c  mov word [edi+0x16], si     ; texture->height
0x1e7e19  mov word [edi+0x18], ax     ; level/format class
0x1e7e46  inc word [edi+0x1a]         ; log2(number of mip levels)
0x1e7e6f  mov dword [edi+0x24], ecx   ; total byte size
0x1e7b57  mov dword [edi+0x44], eax   ; mip level count
```

After the `PFRM` tag four `u32` are read with `0x1ab140` (`0x1e7b93`, `0x1e7b9e`, `0x1e7ba6`,
`0x1e7bae`); they land in `ebx` (2nd), `ebp` (3rd) and `eax` (4th), the 1st in a stack slot:
they are the **R, G, B, A masks in file order** (independently corroborated by
`tools/res_msk.py`, where `masks[0]=R`, and by the shipped data: `F800 / 07E0 / 001F / 0000`).
They only select a **pixel-format code** stored at `texture+0x40`:

```
0x1e7be4  cmp ebp, 0x1F,  0x1e7be9 cmp ebx, 0x3E0,  0x1e7bf1 cmp [esp+0x5c], 0x7C00
0x1e7bff  test eax, eax          ; A mask == 0 required
0x1e7c01  jne 0x1e7c93           ; A mask != 0  →  not 555/565  →  " - unsupported texture format"
0x1e7c07  mov dword [edi+0x40], 3 ; 555   (R=0x7C00 G=0x3E0 B=0x1F A=0)
0x1e7c13  cmp ebx, 0x7E0 … cmp [esp+0x5c], 0xF800 ; 0x1e7c29 mov [edi+0x40], 2   ← RGB565
0x1e7c53  mov dword [edi+0x40], 5 ; 4444  (0xF00/0xF0/0xF/0xF000)
0x1e7c7a  mov dword [edi+0x40], 6 ; XRGB888     0x1e7c8a  mov [edi+0x40], 7 ; ARGB8888
0x1e7ca7  mov dword [edi+0x40], 8 ; 8 bpp / palettised
```

(`2 = RGB565` is what every shipped ENV/COMMON blob is; the renderer later branches on
`code == 8`, `0x1db503`.)

So a 16 bpp RGB555/565 texture **with a non-zero alpha mask is not supported** — the two
16 bpp classifier branches require the 4th mask to be 0, otherwise the loader falls through
to `" - unsupported texture format"` (string at `0x683430`) and leaves the format code unset.
Every shipped ENV/COMMON texture has A mask 0 and RGB565 masks. **The loader never calls
`SetColorKey`, and it never stores a key colour anywhere.**

### 1.3 No DirectDraw creation-time colour key

Texture surfaces are created in `0x1bae30`:

* `dwSize = 0x7c` (`DDSURFACEDESC2`), `dwFlags = 0x1007 | 0x20000`
  (`DDSD_CAPS|HEIGHT|WIDTH|PIXELFORMAT`, plus `DDSD_MIPMAPCOUNT` when the global `0x7208c0`
  is non-zero; the mip count goes to `dwMipMapCount`), `ddsCaps = DDSCAPS_TEXTURE (0x1000) |
  DDSCAPS_VIDEOMEMORY (0x4000)` or `| DDSCAPS_SYSTEMMEMORY (0x800)`, plus
  `DDSCAPS_COMPLEX (0x8)` in the video-memory case (`0x1baf0e or eax, 0x400008`).
* `DDSD_CKSRCBLT` (`0x00010000`) is **never** used, `ddckCKSrcBlt` in the descriptor is never
  filled, and the `DDPIXELFORMAT` is taken from a runtime-built table (`0x720128/0x720148/
  0x720728/0x720870`, in BSS) — RGB, no `DDPF_ALPHAPIXELS`.

The only key-related code in that function is a *reset to black*:

```
0x1bb01a  test byte [ebp+0x34], 8   ; texture->flags bit 3 = "has colour key"
0x1bb01e  je   0x1bb03b
0x1bb020  xor  ecx, ecx
0x1bb022  mov  [esp+0x14], ecx      ; DDCOLORKEY.low  = 0
0x1bb02a  mov  [esp+0x18], ecx      ; DDCOLORKEY.high = 0
0x1bb03e  push 8                    ; DDCKEY_SRCBLT
0x1bb040  push eax                  ; surface
0x1bb041  call dword [edx+0x74]     ; SetColorKey
```

(the no-key branch pushes a NULL `DDCOLORKEY`). This runs **before** the material-driven call
below, so the material colour wins.

---

## 2. The actual mechanism: `MATERIALS` → colour index → palette → colour key

### 2.1 The material keyword parser (`0x1eb7a0`)

The material slots themselves are created by the **B3D loader**, not by the `.res`: the
`.b3d` header carries the material count (`u32` at 0x18; e.g. 114 for `aa.b3d`) followed by
that many 32-byte material names starting at 0x1c. For `aa`, `bc`, `ap`, `cb`, `dc` the
`.b3d` name table and the `.res` `MATERIALS` list are **identical in name and order**, and the
`.res` loader (`0x1ecfb4`…`0x1ed15b`) only attaches/overrides properties, matching slots by
name (`0x1ed04d`). That is why the same `.txr` blob can be keyed in one tile and opaque in
another.

Each `MATERIALS` line is `<name> <keyword> <arg> <keyword> <arg> …`. The parser is called from
the `.res` loader at `0x1ed0c7` (`call 0x1eb7a0` with the value string and the 0x7c-byte
material slot). The interesting keywords (string table at `0x683654…0x6836c0`):

| keyword | effect | address |
|---|---|---|
| `col C` | `material->colorIndex (slot+0x28) = C - 1` | `0x1eb815` |
| `tex N` | `slot+0x20 \|= 1`, `slot+0x38 = N-1` | `0x1eb869` |
| `ttx N` | `slot+0x20 \|= 3`, `slot+0x38 = N-1` | `0x1eb8c2` |
| `itx N` | `slot+0x20 \|= 0x41`, `slot+0x38 = N-1` | `0x1eb906`/`0x1eb915` |
| `transp X` | `slot+0x20 \|= 0x20`, `slot+0x58 = atof(X)` | `0x1eb9aa` |

```
0x1eb815  3bc3           cmp  eax, ebx            ; strcmp(token,"col") == 0 ?
0x1eb817  751c           jne  0x1eb835
0x1eb819  8d442414       lea  eax, [esp+0x14]
0x1eb81d  50             push eax
0x1eb81e  e88df9fbff     call 0x1ab1b0            ; next token
0x1eb823  50             push eax
0x1eb824  e874440300     call 0x21fc9d            ; atoi
0x1eb829  83c408         add  esp, 8
0x1eb82c  48             dec  eax
0x1eb82d  894528         mov  dword [ebp+0x28], eax   ; ★ material->colorIndex = C-1

0x1eb8c2  8d442414       lea  eax, [esp+0x14]     ; "ttx"
0x1eb8c7  e8e4f8fbff     call 0x1ab1b0
0x1eb8cc  8b7d20         mov  edi, [ebp+0x20]
0x1eb8cf  83cf03         or   edi, 3              ; flags |= 1|2
0x1eb8d2  eba5           jmp  0x1eb879            ; …mov [ebp+0x38], atoi(tok)-1
```

### 2.2 "ttx" → the texture object gets flag bit 3

While preparing a module (`0x1db340`…), for every material slot:

```
0x1db380  f6402002       test byte [eax+0x20], 2    ; material flag bit 1 (= "ttx")
0x1db384  740d           je   0x1db393
0x1db386  8b4838         mov  ecx, [eax+0x38]       ; material->texIndex
0x1db389  8b4754         mov  eax, [edi+0x54]       ; texture-pointer array
0x1db38c  8b0488         mov  eax, [eax+ecx*4]
0x1db38f  83483408       or   dword [eax+0x34], 8  ; ★ texture->flags |= 8
```

This is the **only** instruction in the whole binary that sets bit 3 of a texture's flags
(searched for every `or dword [reg+0x34], 8` / `or dword [reg+0x34], 0x8` form: exactly one hit).

### 2.3 The key colour is computed and applied (`0x1db529`)

```
0x1db529  f6473408       test byte [edi+0x34], 8   ; has colour key?
0x1db52d  7470           je   0x1db59f
0x1db52f  8b4628         mov  eax, [esi+0x28]      ; ★ material->colorIndex  (= C-1)
0x1db532  8b93d4010000   mov  edx, [ebx+0x1d4]     ; texture/palette manager
0x1db538  83c008         add  eax, 8               ; skip the palette object's 24-byte header
0x1db53b  8d0c40         lea  ecx, [eax+eax*2]     ; *3 bytes per colour
0x1db53e  8b4264         mov  eax, [edx+0x64]      ; array of palette objects
0x1db541  8b10           mov  edx, [eax]           ; first loaded palette file
0x1db543  33c0           xor  eax, eax
0x1db545  03ca           add  ecx, edx             ; &PALT[C-1]
0x1db547  33d2           xor  edx, edx
0x1db549  89542410       mov  [esp+0x10], edx      ; DDCOLORKEY.low  = 0
0x1db54d  89542414       mov  [esp+0x14], edx      ; DDCOLORKEY.high = 0
0x1db551  8a01           mov  al, [ecx]            ; R
0x1db553  8a5101         mov  dl, [ecx+1]          ; G
0x1db556  24f8           and  al, 0xf8
0x1db558  c1e005         shl  eax, 5
0x1db55b  83e2fc         and  edx, 0xfffffffc
0x1db55e  0bc2           or   eax, edx
0x1db560  33d2           xor  edx, edx
0x1db562  8a5102         mov  dl, [ecx+2]          ; B
0x1db565  c1e003         shl  eax, 3
0x1db568  c1ea03         shr  edx, 3
0x1db56b  0bc2           or   eax, edx             ; k = RGB565 truncating
0x1db571  89442410       mov  [esp+0x10], eax      ; ck.low  = k
0x1db575  89442414       mov  [esp+0x14], eax      ; ck.high = k
0x1db579  8b4748         mov  eax, [edi+0x48]      ; IDirectDrawSurface7*
0x1db57c  52             push edx                  ; &ck
0x1db57d  6a08           push 8                    ; DDCKEY_SRCBLT
0x1db57f  50             push eax
0x1db580  8b08           mov  ecx, [eax]
0x1db582  ff5174         call dword [ecx+0x74]     ; SetColorKey
```

`0x74/4 = index 29 = IDirectDrawSurface7::SetColorKey` (for `IDirectDrawSurface`…`7` this
index is the same). Failures are reported through `0x1d4c10`.

### 2.4 Why `add eax, 8` — the palette object layout

The palette file loader `0x1e6e90` (dispatched from the `PALETTEFILES` branch of the `.res`
loader) checks the chunk tag `"PLM"` (`0x683424`), allocates **`0x330` = 816 bytes**
(`0x1e6f1b push 0x330`), then for the `"PALT"` chunk (`0x68341c`):

```
0x1e7000  mov eax, 0x55555556 ; imul ebx   ; len / 3 colours
0x1e7016  lea edi, [esi+0x19]              ; first write lands on [edi-1] = object+0x18
0x1e701b… writes R,G,B and does `add edi,3`
```

so **RGB triples start at `object+0x18` (= 24 = 8 entries × 3 B)**, exactly the `add eax,8`
above. Independently confirmed by the post-load pass at `0x1ed59f`:

```
0x1ed59f  mov eax, [ebp+0x64]     ; palette array
0x1ed5a6  mov eax, [eax]          ; first palette object
0x1ed5ac  lea edi, [eax+0x18]     ; ★ palette colour base
0x1ed5f1  mov esi, [edx+eax+0x28] ; material->colorIndex, used directly as the entry index
0x1ed5ff  mov bl, [esi+eax]       ; byte at base + 3*index  (blue channel → 0xBBBBBB in +0x44)
```

So `material->colorIndex` **is** the palette entry index (`col 256` → entry 255).

Palette used: the first entry of the palette array (`[eax]` = `array[0]`). The world/ENV data
loads no palettes of its own; the only `PALT` in the shared data is `COMMON.RES` →
`COMMON/common{,1,2}.res` → `common.plm` (all three byte-identical, entry 0 = `(0,0,0)`,
entry 3 = `(191,191,191)`, entry 244 = `(255,247,207)`, entry 255 = `(50,59,18)`).
`LOAD.RES` has a different one (`menu_.plm`), but its entry 0 is black as well.

### 2.5 What about `MASKFILES`?

Masks are a separate, explicit-alpha mechanism: `MSK*` blobs (`0x308` header + RLE, see
`docs/msk_format.md`) are loaded by `0x1e94d0`/`0x1e9910`, and the mask surface is created with
`SetColorKey(DDCKEY_SRCBLT, {0,0})` at `0x1bc135` (mask palettes index 0 = transparent) and
`0x1e9239`. The material parser binds a mask with the `att`/mask keywords. **No `ENV/*.res`
has `MASKFILES`** (count 0 in all 117 files), so this path is irrelevant for the world textures
— and it is not what makes the trees/fences transparent.

The `COLORS 256` section is parsed and **thrown away**: the loader (`0x1ec2c4`…`0x1ec32a`)
reads the count and then just consumes that many tokens with `0x1ab1b0` without storing them.
The floats there are not the key source; the `.plm` byte palette is.

---

## 3. Empirical validation over every shipped `.res`

Re-generated with `python3 tools/txr_key.py --rules` (118 `.res` = `ENV/*.res` + `COMMON.RES`,
6788 (file, texture) pairs; 2137 engine-keyed pairs).

* Key colours in use in the whole world data — only **four**:

| `col C` | palette index | key (RGB565) | colour | pairs | distinct textures |
|---|---|---|---|---|---|
| absent (`ttx N`) | 0 | `0x0000` | black | 1255 | 95 |
| `col 256` | 255 | `0x31C2` | (50,59,18) olive | 858 | 85 |
| `col 245` | 244 | `0xFFB9` | (255,247,207) pale haze | 15 | 5 (`mount*.txr`) |
| `col 4` | 3 | `0xBDF7` | (191,191,191) grey | 9 | 3 (`wf_sp`, `wf_ver12*`, `fonar`) |

* **The key colour is also the most frequent colour of the texture in 876 of 882 explicit-`col`
  pairs (99.3 %)**; in the other 6 the key colour does not occur at all (nothing is hidden):

  | share bucket | explicit `col` (882) | default index 0 (1255) |
  |---|---|---|
  | 0 % | 3 | 315 |
  | < 1 % | 0 | 92 |
  | 1–5 % | 12 | 90 |
  | 5–10 % | 97 | 40 |
  | 10–25 % | 120 | 330 |
  | 25–50 % | 606 | 299 |
  | ≥ 50 % | 44 | 89 |

* Texture families (full lists printed by the tool):
  * `0x31C2` — the whole foliage group (`tree*`, `trees*`, `forest*`, `Tree*ca`, `kust`,
    `aa2`, `aa3`, `fence0`, `fence06`, `Ograjdenie3`, `swamp1`, `side1/2`, `Mount3_slope_`,
    `fronton`, `mount1`…).
  * `0x0000` — fences/walls (`fence*`, `FENCE2`, `rfence`, `Airport_RoofFence`,
    `construction01_/03_`, `wall*`, `combinat_wire/gates/wall`, `vent`, `ventgrid`, `stolb`,
    `rama`, `zont`, `bridge_*`, `power`, `tarelka`, `store_r0`, `stos_ceil*`, `furgon*`,
    the whole `Z*` sign family, `back*`, `benzo_v`, `h_0*`, `m_expl`, `Gold*`).
  * `0xBDF7` — three wire/glow textures; `0xFFB9` — five mountain textures.

### Counter-examples / caveats (important for an implementation)

1. **`fronton.txr`** (`cb.res`, `cb1.res`, `cb2.res`, material `mat_cottage1_fronton
   ttx … col 256`): the key colour `0x31C2` is *absent* from the bitmap (0 pixels), so the
   engine's key hides nothing. A decoder using the rule is right (nothing changes); a decoder
   *ignoring* the rule is also visually identical. The art simply does not use the key.
2. **`ttx` without `col` is common and often a no-op.** In `default0` above, 315+92 pairs have
   no black at all (e.g. `stos_ceil.txr` top = `0xFFFF` 33 %, `Zpark.txr` top = `0x0AD4` 77 %,
   `combinat_gates.txr` 0 black pixels, `store_r0.txr` 0.9 % black). The material says
   "transparent" but the art contains no key colour → the surface is effectively opaque.
   Conversely, the default of index 0 is clearly intentional where it matters: MENV cut-outs
   (`br_5` 22 %, `Zzigzag` 44 %, `back7` 42 %, `Zclosed` 46 % black) and 60 % of the ENV
   `ttx`-without-`col` textures are dominated by black (`benzo_v.txr` 75 %,
   `Airport_RoofFence.txr` 70 %, `vent.txr` 65 %, `fencew.txr` 64 %, `construction01_` 63 %…).
   A single oddity is `MENV/dr.res` `mat_wall → wall.txr`, which is a black wedge on a *white*
   background — hiding black leaves the white square. It is the only case in the shipped data
   where the default-black reading looks visually questionable.
3. **The same `.txr` blob can be keyed in one tile and opaque in another.** Example:
   `h_06.txr` → `aj.res` has `mat_h6 tex 30` (opaque) while `aj.res`'s neighbour-materials and
   other tiles have `mat_h6 ttx …`. `aj.res` also has `mat_h5 ttx 29`. So the key must be
   resolved **per (`.res`, texture)**, not from a global name→key table.
4. Textures that are *not* referenced by any `ttx` material (4871 pairs) get **no** colour key
   at all, even when they are almost all black.

### 3.1 Naive rules and their false-positive risk

Measured on the 6661 LOFF textures of `ENV/*.res` + `COMMON.RES` (2114 engine-keyed):

| rule | textures keyed | engine-keyed missed | **wrongly keyed** |
|---|---|---|---|
| most frequent colour ≥ 5 % | 5356 | 98 | 3340 |
| most frequent colour ≥ 10 % | 3901 | 381 | 2168 |
| most frequent colour ≥ 25 % | 2691 | 728 | 1305 |
| most frequent colour ≥ 50 % | 978 | 1804 | 668 |
| key = black | 1679 | 1035 | 600 |
| key = `0x31C2` | 1086 | 1259 | 231 |
| key = black or `0x31C2` | 2528 | 340 | 754 |
| key = black or `0x31C2`, each ≥ 5 % of pixels | 1892 | 538 | 316 |
| **engine rule (MATERIALS `ttx`/`col`)** | **2114** | **0** | **0** |

The "most frequent colour" rules are right about *which* colour is the key (the key is the top
colour in 876/882 explicit-`col` cases) but they cannot tell *whether* a texture is keyed at
all — they key two thirds of the opaque textures. Worst false positives of `black|0x31C2`
(pixels that would wrongly disappear):

```
common1.res crpart1..4.txr                                                   57–71 % black
ap*.res     Airport_planes.txr                                                    56 % black
cc*.res     wall_side1.txr                                                    54–55 % black
common1.res shadtree1.txr                                                         52 % black
aj.res      h_06.txr, ar.res h_05.txr                                             50 % black   (opaque in those tiles!)
ENV/*.res   stos_lrepair.txr                                                      46 % black
```

(`LOAD.RES` is even worse — `backRcrd`/`backCtrl`/`backGraf`/… are 61–70 % black full-screen
menu art — but those are the 8 bpp variant and are not part of the 16 bpp world data.)

---

## 4. Recommended minimal implementation rule

For a C++ decoder that reads a `.res` (as `src/res` already does) and wants an RGBA texture:

```
1. Read the .res sections in this order:
     TEXTUREFILES  → ordered list tex[0..n-1]   (NAME: drop the directory and any
                                                 " <number>"/" noload" suffix)
     MATERIALS     → list of "name keyword arg …" lines
2. Build the texture→key map:
     for each line L in MATERIALS:
         tok = tokens(L)                          ; token 0 = material name, rest = value
         N = None; C = None; keyed = false
         for i in 0..len(tok)-1:
             if tok[i] in {"ttx","tex","itx"} and i+1 < len:  N = int(tok[i+1]) - 1
                                                              keyed = (tok[i] == "ttx")
             if tok[i] == "col" and i+1 < len:                C = int(tok[i+1]) - 1
         if keyed and N is not None and 0 <= N < len(tex):
             keyIndex[tex[N]] = (C if C is not None else 0)
3. Palette: PALT[256] = the RGB triples of the first PALETTEFILES entry of
   COMMON.RES (COMMON/common.plm, or common1.res/common2.res — all identical).
   If you have no palette, use PALT = {0:(0,0,0)} (default index 0 = black).
4. Conversion (exactly as the engine, truncating):
     k = ((R & 0xF8) << 8) | ((G & 0xFC) << 3) | (B >> 3)
5. Alpha:
     if a texture has a key index:  alpha = (raw16 == k) ? 0 : 255
     else:                          alpha = 255
   (the 8 bpp `LOAD.RES` menu bitmaps have their own 256-colour palette, 186 masks and no
   `MATERIALS` section at all — they are a separate UI path; `tools/res_msk.py` assumes
   index 0 = transparent there, which is the usual convention but was not traced here.)
```

Minimal condition, stated exactly:

> **A 16 bpp texel is transparent iff its RGB565 value equals
> `((PALT[C-1].R & 0xF8) << 8) | ((PALT[C-1].G & 0xFC) << 3) | (PALT[C-1].B >> 3)`,
> where `C` is the argument of the `col` keyword of a `ttx` material of the same `.res`
> whose texture index is this texture (`C = 1` → `PALT[0]` = black when `col` is absent).**

Notes:
* Do not key anything for a texture that no `ttx` material of its own `.res` references
  (that alone removes 3340 of the naive false positives).
* `fronton.txr`-style cases are harmless: if the key colour is absent, nothing changes.
* If you cannot parse `MATERIALS` at all, the least harmful fallback is: key only pure black
  (`0x0000`) **and** only for textures that are billboards/sprites by name (`tree*`, `forest*`,
  `fence*`, `FENCE*`, `Z*`, `back*`, `Airport_*`, `stolb`, `vent*`, `rama`, …) — never
  globally, because `backRcrd/backCtrl/crpart*/Airport_planes` are 50–70 % black and opaque.
* Do not be tempted by a per-texture "first pixel is the key" or "most frequent colour" rule;
  neither is the engine rule and both destroy opaque art.

---

## 5. How to verify

1. **Numbers (no rendering).**
   `python3 tools/txr_key.py --rules --max 20` re-derives every key from the data and prints,
   per (`.res`, texture): material, `col`, key, number/share of keyed pixels, and the
   false-positive lists of the naive rules. Expected highlights:
   `aa.res tree0.txr` key `0x31C2`, 1382/4096 = 33.74 %;
   `ap.res Airport_RoofFence.txr` key `0x0000`, 2854/4096 = 69.68 %;
   `bc.res wf_sp.txr` key `0xBDF7`, 94.53 %;
   `cb.res fronton.txr` key `0x31C2`, 0 pixels (documented no-op).
2. **Visual, per texture.** Decode with `tools/res_msk.decode_texture()` and paint the keyed
   pixels magenta (255,0,255) before writing a PNG. What to look for:
   * `ENV/aa.res tree0.txr` — magenta must trace the *outside* silhouette of the foliage only,
     never the interior (see the render in this session: a clean cut-out).
   * `ENV/ap.res Airport_RoofFence.txr` — magenta must fill the fence openings, leaving the
     frame; the result must be a frame, not a solid quad.
   * `ENV/aa.res rama.txr` (frame), `MENV/dr.res Zzigzag.txr` / `Zclosed.txr` (road signs) —
     magenta everywhere outside the sign shape.
   * Negative controls (must stay fully opaque, no magenta at all): `ENV/ap.res
     Airport_planes.txr` (56 % black), `ENV/cc.res wall_side1.txr` (55 % black),
     `COMMON/common1.res crpart3.txr` (71 % black), `ENV/aj.res h_06.txr` (50 % black, listed
     as `tex 30` there). If your build punches holes in these, you applied a global black rule
     instead of the material rule.
3. **In the re-implementation.** Load a tile that mixes all four key colours — `ENV/aa.res`
   (many families) or `ENV/bc.res` (`0x31C2` trees + `0xBDF7` wire fence + black fences) —
   and render it. Checklist:
   * tree billboards have clean edges, no olive halo and no black box around them;
   * `Airport_RoofFence` / `fence` textures are see-through frames;
   * `Z*` road signs are triangles/diamonds, not squares;
   * lamp/`fonar` glow quads show `0xBDF7`-keyed grey background removed (where present);
   * walls, roads, terrain and menu backdrops are unchanged (alpha 255), in particular no
     holes in `stos_lrepair`, `wall_side1`, `h_05/h_06` in tiles where they are listed as `tex`.
4. **Regression check.** For every texture the tool prints with a key, verify the decoder's
   transparent-pixel count equals the printed one; for every texture with no keyed material,
   verify alpha is 255 everywhere.

---

## 6. Address index (file offsets; VA = +0x400000)

| offset | what |
|---|---|
| `0x1e78e0` | load a `.txr` blob from file → texture object |
| `0x1e7930` | chunk walker: `LOFF`/`PFRM`/`LVMP`/`LVLS`/`ENDR` (+ `DATA/DIMS/LEVL/LVLH` for other users) |
| `0x1e6e90` | `.plm` palette loader (`PLM`/`PALT`), object = 0x330 B, RGB triples at `+0x18` |
| `0x1ec28e` / `0x1ec2c4` | `COLORS` section: parsed and discarded |
| `0x1ecf7c` | `MATERIALS` section dispatch |
| `0x1eb7a0` | material keyword parser (called from `0x1ed0c7`) |
| `0x1eb815` | `col C` → `material->colorIndex = C-1` |
| `0x1eb8c2` | `ttx N` → flags \|= 3, texIndex = N-1 |
| `0x1ed59f` | post-load pass: resolves material→texture/mask pointers, `+0x44 = 0xBBBBBB` |
| `0x1db380` | `ttx` → `texture->flags \|= 8` (the only writer) |
| `0x1db529` | compute the key from palette[colorIndex] and call `SetColorKey` |
| `0x1db582` | `call [vtbl+0x74]` = `IDirectDrawSurface7::SetColorKey` |
| `0x1bae30` | texture surface creation (`DDSURFACEDESC2`, no `DDSD_CKSRCBLT`) |
| `0x1bb01a` | pre-set `SetColorKey(SRCBLT, {0,0})` for flag-8 textures |
| `0x1bc135`, `0x1e9239` | mask surface `SetColorKey(SRCBLT, {0,0})` |

Related tools: `tools/res_msk.py` (`.res`/MSK/TXR decoding), `tools/txr_key.py`
(this analysis, `--rules` for the comparison tables).

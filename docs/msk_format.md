# `.msk` (MASK) and `.txr` (TEXTURE) pixel storage inside `.RES` containers

Derived from `original/king.exe` (PE32 x86, ImageBase `0x400000`) and verified
against every mask and texture of every `.res` file in the GOG release.

**Verification result (reproduce with `tools/res_msk.py --all --verify`):**

| what | count | byte-exact |
|---|---|---|
| masks (`MASK` 8-bpp + `MS16` 16-bpp) over 141 `.res` files | 1056 | **1056 / 1056** |
| textures (`.txr` chunked + 8-bpp paletted) over the same files | 12229 | **12229 / 12229** |

"Byte-exact" means: decoding stops after exactly `width * height` pixels, the
source cursor has advanced by exactly the number of payload bytes the blob
declares, and every one of the `height` rows sums to exactly `width` pixels.

> Address convention in this document: addresses are **file offsets**, i.e. the
> values you type into `r2` raw mode (`r2 -q -n -e asm.arch=x86 -e asm.bits=32
> -c 'pd 40 @ 0x1bc1a9' original/king.exe`). Add `0x400000` for the virtual
> address that appears inside the binary.

---

## TL;DR / the two answers that were blocking

1. **The 768-byte "constant region" is a 256-entry RGB palette**, not image data,
   not a dither matrix, not a Huffman table, and the decoder never reads it from
   the stream — it occupies a fixed slot in the header. The "16-byte table at
   payload[6:22]" is that palette's grayscale ramp *misaligned by one byte*
   (the analysis measured from the payload = blob+4, the palette starts at
   blob+8). See §3.4.
2. **The pixel data is a trivial run-length code with an explicit transparent
   run**, starting at **blob offset 776 (0x308)** — exactly where `0x1e9690`
   fseeks to. Token = one byte `c`, `n = c & 0x7f`; `c & 0x80` set → `n`
   transparent pixels, clear → `n` literal pixels stored as `n * depth` raw
   bytes. See §3.3.

---

## 1. `.RES` container framing

Text directory, NUL-separated, then the payloads:

```
COLORS <n>\0        <n> float strings
SOUNDFILES <n>\0    <n> entries
SOUNDS <n>\0
MASKFILES <n>\0     <n> entries
TEXTUREFILES <n>\0  <n> entries
PALETTEFILES <n>\0  <n> entries
BACKFILES <n>\0     <n> entries
MATERIALS <n>\0     ...
```

Each entry: `<name> <flags>\0` + `<u32 size>` + `size` bytes.
The next entry starts at `entry_start + 4 + size`. Sections may appear in any
order (`LOAD.RES`: COLORS, SOUNDFILES, SOUNDS, MASKFILES, TEXTUREFILES,
PALETTEFILES, BACKFILES; `MENV/dr.res`: PALETTEFILES, SOUNDFILES, BACKFILES,
MASKFILES, TEXTUREFILES, COLORS, MATERIALS).

`PALETTEFILES` entries are `.plm` files holding the **global** palette:

```
"PLM\0" u32 total_size "PALT" u32 0x300 <768 bytes of RGB triples>
```

`LOAD.RES/menu_.plm`: `00 00 00 | 40 40 40 | 80 80 80 | c0 c0 c0 | ff ff ff |
ff 00 00 | 8c ff aa | f5 ff 9b | ...` — i.e. **RGB** order (entry 5 is red).

## 2. The two kind of mask blobs

`0x1e94d0` is the mask header parser. It `fread`s a 4-byte tag and string-compares
it against four literals:

```
0x1e94f5  mov esi, 0x683524   ; "MASK"
0x1e9538  mov esi, 0x68351c   ; "MSKR"
0x1e9583  mov esi, 0x683514   ; "MS16"
0x1e95c5  mov esi, 0x68350c   ; "MSK8"
```

| tag | `word[mask+8]` (depth) | `dword[mask+0x1c] |= 0x80` | ships in the data |
|---|---|---|---|
| `MASK` | 1 | no | 186 (all of `LOAD.RES`) |
| `MSKR` | 1 | yes | never |
| `MSK8` | 1 | yes | never |
| `MS16` | 2 | no | 870 |

Then it reads width/height and computes the payload size:

```
0x1e960f  push ebp
0x1e9610  call 0x1ab0f0        ; = fread(f, buf, 2, 1); return (int16)buf  -> WIDTH
0x1e9616  mov word [esi+4], ax
0x1e961a  call 0x1ab0f0        ; HEIGHT
0x1e961f  mov word [esi+6], ax
0x1e9623  mov eax, [esp+0x28]  ; arg3 = the entry's <u32 size>
0x1e9627  add eax, 0xfffffcf8  ; -0x308  (-776)
0x1e962f  mov dword [esi+0xc], eax   ; payload byte count
```

and `0x1e9690` reads exactly those bytes:

```
0x1e97a7  push ebp / call 0x1ab0f0   ; width  (re-read, discarded)
0x1e97ad  push ebp / call 0x1ab0f0   ; height (re-read, discarded)
0x1e97b3  add ebx, 0x308             ; <-- DATA STARTS AT blob + 776
0x1e97b9  ... call 0x21e6a3          ; fseek(f, ebx, SEEK_SET)
0x1e97c2  mov eax, [edi+0xc]         ; size
0x1e97c5  mov ecx, [edi+0x10]        ; destination
0x1e97cd  call 0x21e20f              ; fread(dst, 1, size, f)
0x1e97d8  or byte [edi+0x1c], 1
```

`0x308 = 776 = 4 (tag) + 4 (w,h) + 768 (palette)`. Bytes 8..775 are **never
looked at by the loader** — it seeks straight past them. That is why their
content looked like an unexplained constant region.

## 3. `.msk` layout and the exact RLE

### 3.1 Field-by-field

```
offset  size  field
------  ----  ---------------------------------------------------------------
0x000   4     tag: "MASK" | "MSKR" | "MSK8" | "MS16"
0x004   u16   width
0x006   u16   height
0x008   768   256 x 3 bytes RGB palette.
              "MASK"/"MSKR"/"MSK8": real palette (identical to the global
              `.plm` palette -> the art shares one palette).
              "MS16": all 768 bytes are ZERO (unused slot; direct colour).
0x308   ...   RLE stream, exactly (blob_size - 776) bytes for MASK/MSKR/MSK8
0x308+N ..    MS16 only: "PFRM" u32 len + len bytes + "ENDR" u32 0  (44 bytes)
```

Nothing else. `blob_size` comes from the RES entry's `<u32 size>`.

### 3.2 The RLE, verbatim from the binary

The decoder is at **`0x1bc1a9`**, inside the surface-locking routine that starts
at `0x1bc060` (it `Lock`s the DirectDraw surface via `call dword [edx+0x18]` and
decodes straight into the locked pixels, `ebx` = source, `edi` = destination,
`ebp` = the mask object):

```
0x1bc1a9  mov al, byte [ebx]      ; c = *src
0x1bc1ab  mov cl, al
0x1bc1ad  and ecx, 0x7f           ; n = c & 0x7f
0x1bc1b0  test al, 0x80
0x1bc1b2  mov edx, ecx            ; edx = n
0x1bc1b4  je 0x1bc1d2             ; bit7 clear -> literal run
          ; ---- transparent run -------------------------------------------------
0x1bc1b6  xor ecx, ecx
0x1bc1b8  xor eax, eax            ; value = 0
0x1bc1ba  mov cx, word [ebp+8]    ; depth (1 or 2 bytes per pixel)
0x1bc1be  inc ebx                 ; src++
0x1bc1bf  imul ecx, edx           ; ecx = depth * n
0x1bc1c7  rep stosd ... 0x1bc1ce  rep stosb   ; zero-fill depth*n bytes
          ; ---- literal run -----------------------------------------------------
0x1bc1d2  xor ecx, ecx
0x1bc1d4  inc ebx                 ; src++
0x1bc1d5  mov cx, word [ebp+8]    ; depth
0x1bc1d9  mov esi, ebx
0x1bc1db  imul ecx, edx           ; ecx = depth * n
0x1bc1e3  rep movsd ... 0x1bc1ea  rep movsb   ; copy depth*n raw bytes
0x1bc1f5  add ebx, ecx            ; src += depth * n
          ; ---- advance the pixel cursor ----------------------------------------
0x1bc1f7  xor eax, eax
0x1bc1fd  mov ax, word [ebp+8]    ; depth
0x1bc203  imul eax, edx
0x1bc206  mov cx, word [ebp+4]    ; width
0x1bc20a  add edi, eax            ; dst += depth*n
0x1bc214  add eax, edx            ; x += n
0x1bc216  cmp eax, ecx            ; while (x < width)
0x1bc21c  jl 0x1bc1a5
```

Pseudocode (this is exactly what `tools/res_msk.py: rle_expand` does):

```python
for y in range(height):
    x = 0
    while x < width:
        c = src[i]; i += 1
        n = c & 0x7F
        if c & 0x80:                 # transparent run of n pixels
            out += b"\0" * (n * depth)
        else:                        # literal run of n pixels
            out += src[i:i + n * depth]; i += n * depth
        x += n
    # a row ends as soon as x >= width; in the shipped data x lands on width
    # exactly, so runs never straddle row boundaries
```

**Transparency is therefore "value 0"**, not a separate alpha plane: the
transparent branch physically writes `depth * n` **zero** bytes
(`xor eax,eax` → `rep stosd`). For 8-bpp masks that is palette index 0; for
`MS16` it is the 16-bit value `0x0000`, which is also alpha 0 under every
`PFRM` format used for MS16.

Cross-check over all 186 `LOAD.RES` masks: index 0 occurs 916 760 times, which
is **exactly** the number of pixels emitted by transparent runs, and literal
runs contain index 0 **0** times. Indices 1..7 never occur at all (they are the
reserved UI part of the palette); indices 8..255 are all used.

### 3.3 Byte-exactness

`tools/res_msk.py` computes, for every blob:

```
expected = blob_size - 776            (MASK / MSKR / MSK8)
expected = blob_size - 776 - 44       (MS16; 44 = PFRM chunk + ENDR chunk)
```

and checks `consumed == expected`, that the last run of the last row ends on
`width`, and (for MS16) that the trailer really is
`"PFRM" u32 28 <7 dwords> "ENDR" u32 0`.

Example, `abot_a.msk` (29x29): blob `0x59c = 1436` bytes, palette 768,
RLE `660 / 660` bytes consumed, 841 pixels, all 29 rows sum to 29.

### 3.4 What the "16-byte table" and the "750-byte constant region" really are

The earlier analysis measured `payload = bytes after the 4-byte tag`, so
`payload[k] == blob[k+4]`. The palette occupies `blob[8:776]`, i.e.
`payload[4:772]`:

* `payload[0:2]` = width, `payload[2:4]` = height   (blob 4..7)
* `payload[4]` = first byte of palette entry 0      (blob 8)
* `payload[6:22]` = 16 bytes = `blob[10:26]`
  = `entry0[2]` + `entry1` + `entry2` + `entry3` + `entry4` + `entry5`
  = `00 | 404040 | 808080 | c0c0c0 | ffffff | ff0000`
  = `00 40 40 40 80 80 80 c0 c0 c0 ff ff ff ff 00 00`

  That is exactly the reported "identical 16-byte table". It is the palette's
  black/gray/white ramp, shifted by one byte. **There is no separate table.**
* `payload[22:772]` = 750 bytes = `blob[26:776]` = palette entries 6..255
  (250 entries x 3 bytes = 750). Identical in all 186 masks because the art
  shares one global palette (`menu_.plm`); entries 0..5 are the fixed ramp
  black/gray/white/red.
* `payload[772:]` = `blob[776:]` = the RLE stream — the first per-image byte.

Why the *texture* copy of the same region looked like a "permutation" of the
mask's: for the 8-bpp paletted textures the palette is stored **BGR**, i.e. the
exact triple-reverse of the global palette. Verified: all **172/172** palettes
embedded in `LOAD.RES` textures equal `reverse_each_triple(menu_.plm)`.

Decisive proof that the 768 bytes are a palette slot and not data: all **870**
`MS16` blobs have that whole region zero-filled (a true-colour image does not
need a palette), and their pixel data still decodes byte-exactly.

### 3.5 MS16 (16-bpp) masks

Same RLE, `depth = 2`, no palette, plus a 44-byte trailer that states the pixel
format:

```
"PFRM" u32 28  u32 rmask  u32 gmask  u32 bmask  u32 amask  u32 0 u32 0 u32 0
"ENDR" u32 0
```

Observed `PFRM` masks (file offsets `0x1e7b77` parses `PFRM`, `0x1e7ce5` handles
`ENDR`; the tag strings are `0x683450`/`0x683428`):

* `0x7C00 / 0x03E0 / 0x001F / 0`  → RGB555, opaque
  (e.g. `COMMON/esc15.RES m15\pause.msk` 1600x50 = the "ПАУЗА" bar)
* `0x0F00 / 0x00F0 / 0x000F / 0xF000` → ARGB4444 (e.g. `CABINES.RES txr\glass_1.msk`)

For an `MS16` mask whose `amask` is 0 this implementation treats 16-bit value 0
as the colour key (**assumed**, see §6).

## 4. Textures

Two unrelated on-disk formats, both found under `TEXTUREFILES`.

### 4.1 `LOFF` chunked true-colour textures — 12057 blobs

Used by every `MENV/*.res` and `COMMON/*.res` texture.

```
0x00 u16 12        0x02 u16 2
0x04 u16 0         0x06 u16 0x1000
0x08 u32 0
0x0c u16 width     0x0e u16 height
0x10 u16 0x2010    (bit-depth/flags; 0x2010 = 16 bpp, 0x2008 = 8 bpp)
0x12 "LOFF"        0x16 u32 4
0x1a u32 offset_of_next_chunk        (= 0x1e + 2*width*height)
0x1e width*height u16 pixels, little-endian, base mip level
     "PFRM" ...            pixel format, see §3.5
     ("LVMP"/"LVLS" ...)   reduced mip levels, optional
     "ENDR" u32 0
```

* Loader function starts at `0x1e7930` (SEH prologue); it compares the chunk
  tags `"LOFF"` (`0x683484`), `"LVLS"` (`0x68347c`), `"LVMP"` (`0x683458`),
  `"PFRM"` (`0x683450`), `"ENDR"` (`0x683428`) via `memcmp` at `0x21d6f0`
  (call sites `0x1e7a6b`, `0x1e7b01`, `0x1e7b3a`, `0x1e7b83`, `0x1e7cf1`), and
  rejects other formats with the message at `0x683430`
  (`" - unsupported texture format"`).
* Rendered and visually checked: `MENV/dr.res txr\tree0.txr` (64x64, RGB565
  5:6:5 → a tree), `COMMON.RES txr\smoke_00.txr` (32x32, ARGB4444 → a smoke
  puff), `COMMON.RES txr\m_expl.txr` (256x256, RGB565 → a 3x3 fire/explosion
  sprite sheet).
* **Mip levels (`LVMP`)**: 281 chunks in the shipped data. Layout:
  `u32 levels, u32 first_w, u32 first_h, u32 bytes_per_px` then the packed
  reduced levels. Checked against the length field, e.g. `ENV/aa.res
  txr\ter000.txr` (base 256x256) → `len = 43706 = 16 + 2*(128²+64²+32²+16²+8²+4²+2²+1²)`
  and `COMMON.RES txr\benzo_r2.txr` (base 128x128) →
  `len = 10938 = 16 + 2*(64²+32²+16²+8²+4²+2²+1²)`.
  Every one of the 281 chunks is followed by **2 zero bytes not covered by the
  length field** (purpose unknown). The base level is unaffected.

### 4.2 8-bpp paletted bitmaps — the 172 `LOAD.RES` textures

```
0x00 u16 0x0100   (256 = palette entries)
0x02 u16 0x0001
0x04 u32 0x18010000 as stored (bytes 00 00 01 18)
0x08 u32 0
0x0c u16 width    0x0e u16 height
0x10 u16 0x2008   (8 bpp)
0x12 768 bytes    palette, 256 x B,G,R  (byte-reversed vs the global palette)
0x312 width*height raw 8-bit indices, index 0 = transparent
```

`blob_size == 786 + width*height` exactly for all 172 entries. These are the
only textures with an inline palette; they are uncompressed.
Rendered and checked: `c\backCtrl.txr` (800x600 truck-front background) and
`c\backCtrlOpt.txr` (400x330).

## 5. Do masks and textures share the compression?

Only partially:

* `MASK` (8-bpp) and `MS16` (16-bpp) masks **do** use the §3.2 RLE.
* `LOFF` textures are **not** compressed at all — raw `u16` per pixel, with all
  `2*w*h` bytes present (there is no run-length code on that path). The
  `<u16> 0x2010` header flag and the `PFRM` chunk are the texture-side
  equivalents of the mask's tag/`PFRM` trailer.
* The 8-bpp `LOAD.RES` textures are raw too (one byte per pixel).
* What both families **do** share is the header idea: 4-byte magic + u16 w/h +
  a 768-byte palette slot (used for 8-bpp, zeroed/unused for true colour), and a
  trailing `PFRM` chunk for true-colour data. Only the masks additionally RLE the
  pixels.

## 6. Proven vs. assumed vs. unknown

**Proven (disassembly + byte budget + rendered PNG):**

* RES framing, `.plm` palette framing, and the 768-byte palette slot.
* Mask header: tag, w, h, `payload = size - 776`, data at blob+776.
* The RLE token semantics and the transparent-run zero fill (`0x1bc1a9`).
* Transparency = value 0 for 8-bpp masks (index histogram + decoder zero fill).
* `PFRM`/`ENDR` trailer of `MS16`, its channel masks, and the 44-byte accounting.
* `LOFF` texture header, chunk walk, base-level size, `PFRM` channel masks,
  `LVMP` length formula (16-byte sub-header + packed mips) + the 2 extra bytes.
* 8-bpp texture format, its BGR palette (172/172 = exact triple-reverse of the
  global palette), raw index data, `786 + w*h` size.
* 1056/1056 masks and 12229/12229 textures consume their blobs exactly.

**Assumed:**

* For `MS16` masks whose `PFRM` alpha mask is 0 (RGB555), 16-bit value `0x0000`
  is treated as the transparent key. The RLE decoder does zero-fill transparent
  runs, so 0 must be the "nothing here" value, but the RGB555 blitter path was
  not traced.
* For the 8-bpp paletted textures, index 0 is treated as transparent. Evidence:
  it forms 45% of those images, the first row of `backCtrl.txr` is entirely
  index 0, only 0.08% of index-0 pixels are isolated (i.e. it is a background,
  not interior artwork), and rendering it as transparent cuts the truck out
  cleanly.

**Unknown / not needed, but flagged:**

* `MSKR` / `MSK8` masks: the header parser accepts them and sets object flag
  `0x80`, which makes the 8→16-bpp expander at `0x1bbf00` bail (`test byte
  [ebp+0x1c], 0xa0`), implying a second, compressed code path. **No shipped file
  uses either tag**, so their payload layout is not proven.
* `MS16`/`LOFF` mip levels below the base level (`LVMP`) are located and their
  length formula is verified, but the per-level headers are not decoded.
* The 2 zero bytes after every `LVMP` chunk.
* `0x1bbf00` indexes its palette argument as `palette[(v+8)*3]`, i.e. its
  pointer sits 24 bytes before the first RGB triple (the runtime palette object
  has a header). This does not change the file layout: the artwork uses index
  255, so indexing the file's 768-byte block with `v+8` would read past it;
  `block[v*3]` is correct and is what produces the verified renders.

## 7. Tool

```
python3 tools/res_msk.py <res-file> [--list] [--mask NAME] [--txr NAME]
                        [--out DIR] [--all] [--verify]
```

* masks → `<out>/<name>.png` (RGB + alpha) and `<out>/<name>.gray.png`
  (Rec.601 8-bit grayscale + alpha).
* textures → `<out>/<name>.png` (RGBA; BGR is swapped for the paletted variant).
* every blob prints `consumed=X of Y RLE bytes [EXACT]` or `[MISMATCH]`;
  `--verify` decodes without writing PNGs; `--all` does the whole container and
  the exit status is non-zero if anything does not consume exactly.

Example output:

```
$ python3 tools/res_msk.py original/LOAD.RES --mask 'm\singl_a.msk'
original/LOAD.RES: 186 masks, 172 textures
  m\singl_a.msk noload: 125x125 depth=1 consumed=12438 of 12438 RLE bytes [EXACT]
1/1 blobs consumed exactly
```

## 8. Rendered PNGs that were opened and checked

All under `tools/out/` (`*_zoom.png` are nearest-neighbour enlargements made for
inspection only):

| file | what it shows |
|---|---|
| `abot_a.msk.png`, `abot_a.msk_zoom.png` | 29x29 green round button with a dark triangle |
| `cursor.msk.png`, `cursor.msk_zoom.png` | 14x16 cursor bitmap (diagonal, red highlight) |
| `m_singl_a.msk.png` | 125x125 menu button "ИГРА" ("Play") |
| `m_exit_a.msk.png` | 125x125 menu button "ВЫХОД ИЗ ИГРЫ" ("Exit game") |
| `d_photo01.msk.png` | 116x116 driver photo, correct skin tones (proves RGB palette order) |
| `common.msk.png` | 800x600 truck-front background with the transparent centre |
| `m15_pause.msk.png` | 1600x50 `MS16` RGB555 "ПАУЗА" bar |
| `m15_pager_a.msk.png` | 21x19 `MS16` RGB555 warning light |
| `txr_tree0.txr.png` | `MENV/dr.res` 64x64 RGB565 tree |
| `txr_smoke_00.txr.png` | `COMMON.RES` 32x32 ARGB4444 smoke puff |
| `txr_m_expl.txr.png` | `COMMON.RES` 256x256 RGB565 explosion sprite sheet |
| `c_backCtrl.txr.png` | `LOAD.RES` 800x600 8-bpp paletted truck grille |

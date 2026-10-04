# HT2:RE — the `.FON` / `.SFT` bitmap font format

Reverse engineered from `original/king.exe` (PE32, ImageBase `0x400000`, so
`VA = file_offset + 0x400000`; radare2 raw mode works directly on file
offsets).  Everything marked **PROVEN** below is backed by (a) the loader
disassembly, (b) an exact byte budget over the shipped data files, and (c) a
PNG of the decoded glyphs that was opened and read.  Anything else is marked
**INFERRED** or **OPEN**.

Confidence legend: **[P]** proven, **[I]** inferred (strong evidence, one step
short of proof), **[O]** still open.

---

## 1. Where the loader lives

The single entry point for both extensions is `FUN_005e8c60` (file offset
`0x1E8C60`).  It matches the file's extension against the literal `".fon"`
(string VA `0x6834AC`) and dispatches:

```
0x001e8cb3  mov esi, 0x6834ac          ; ".fon"
0x001e8cb8  add eax, edi               ; eax -> last '.' of the filename
0x001e8cba..0x1e8ce4  (inline strcmp)
0x001e8ce7  test eax, eax
0x001e8ce9  push ebp
0x001e8cea  jne 0x1e8cf3
0x001e8cec  call 0x1e8e20              ; -> .FON loader
0x001e8cf1  jmp 0x1e8cf8
0x001e8cf3  call 0x1e8d10              ; -> .SFT loader   (.sft is NOT .fon)
```

File names are handed to this function from the string table
(`0x676220 "menu.fon"`, `0x67622C "large.fon"`, `0x6768F4 "menublk.fon"`,
`0x676900 "matrix2.fon"`, `0x67690C "matrix1.fon"`, `0x67B890 "icon.fon"`,
`0x67B89C "matrix.fon"`, `0x682ED4 "menu.sft"`).  The current menu code tries
`menu.sft` first and falls back to `menu.fon`:

```
0x001ddd38  push 0x682ed4              ; "menu.sft"
0x001ddd3d  call 0x1e8c60
0x001ddd49  jne 0x1ddd6b
0x001ddd4b  push 0x676220              ; "menu.fon"
0x001ddd50  call 0x1e8c60
0x001ddd5a  jne 0x1ddd6b
0x001ddd5e  push 0x682eb8              ; "menu.sft(fon) is absent."
```

The loaded font object is stored at `[menu_object + 0xB30]`.

---

## 2. The `.FON` container

Both `.FON` layouts found on disk are *one texture plus a fixed header*.  The
`… is absent.` / `.fon` string table also names the four 4-char chunk tags the
LOFF parser looks for (VA `0x68347C`): `LVLS LOFF DATA DIMS LEVL LVLH`.

### 2.1 Variant A — "plain" (no `LOFF` tag)

`MENU.FON` (14610 B) and `LARGE.FON` (24082 B).

```
off  size  field
0x00  2    0x0100                  (unknown, constant in both files)
0x02  2    1                       (unknown, constant)
0x04  2    0                       (unknown)
0x06  2    0x1801                  (unknown, constant)
0x08  4    0                       (unknown)
0x0C  2    width  in pixels        MENU 96, LARGE 112
0x0E  2    height in pixels        MENU 144, LARGE 208
0x10  1    bits per pixel          MENU 8, LARGE 8  (16 in the LOFF variant)
0x11  1    orientation flag        MENU 0x00, LARGE 0x20
0x12  1    0                       (unknown)
0x13  1    see LARGE
0x14  ..   small level/greyscale table:
             MENU : 00 04 04 04 fc fc fc 00 00 00 00
             LARGE: 00 04 04 04 fc fc fc 10 04 04
0x312 ..   pixel data, width*height*bytes_per_pixel bytes
```

**Byte budget [P]:** for both files the pixel block is exactly the last
`width*height*(bits/8)` bytes:

| file | size | w*h | bpp | size − w*h*bpp |
|---|---|---|---|---|
| MENU.FON  | 14610 | 96·144 = 13824  | 1 | **786** |
| LARGE.FON | 24082 | 112·208 = 23296 | 1 | **786** |

so the header is a fixed **786 bytes** (`0x312`).  The first non-zero byte
after `0x20` is exactly at `0x312` in both files, and the rest of the 786-byte
prefix is zero.  The split "18-byte head + 768-byte (256·3) palette area" is
**[I]**; the *size* 786 is **[P]**.

Only two distinct pixel values occur in the atlas: `{0x00, 0x02}` for
MENU.FON and `{0x00, 0x01, 0x02, 0x03}` for LARGE.FON — i.e. the 8-bit values
are palette/level *indices*, not grey levels.

### 2.2 Variant B — `LOFF` / TXR wrapped

`menublk.fon`, `matrix.fon`, `matrix1.fon`, `matrix2.fon`, `icon.fon`.

```
off  size  field
0x00  2    12                      (container type?)
0x02  2    2
0x04  4    0x10000000
0x08  4    0
0x0C  2    width  in pixels
0x0E  2    height in pixels
0x10  1    bits per pixel (16)
0x11  1    orientation flag (0x20 = top-down in all five files)
0x12  4    "LOFF"
0x16  4    4
0x1A  4    chunk size n
0x1E  ..   pixel data, width*height*2 bytes     <-- pixels start at 0x1E
```

**Byte budget [P]:** for all three sizes the payload length `n` reported at
`0x1A` equals `2*w*h + 30`, and the pixel data starts at file offset **30**
(`0x1E`), which is exactly `0x1A + 4`:

| file | w·h | 2·w·h | n | 30 + 2·w·h | file size |
|---|---|---|---|---|---|
| menublk.fon | 96·144 = 13824  | 27648 | 27678 | 27678 | 27722 |
| matrix.fon  | 112·208 = 23296 | 46592 | 46622 | 46622 | 46666 |
| icon.fon    | 400·496 = 198400| 396800| 396830| 396830| 396874 |

(file size − (30 + 2·w·h) = 44 = the trailing chunk header, constant for all
three.)  **This was checked empirically**: scanning every pixel-data start
offset `p ∈ [20,120]` and every row offset, `p = 30` gives an **IoU of 1.00**
against LARGE.FON's `A` for `matrix.fon` and against MENU.FON's `A` for
`menublk.fon`; `p = 44` (the naive "skip the 44-byte prefix" reading) does
not.

The parser for this variant is `FUN_001e7930`; it compares the tag against
`"LOFF"` (VA `0x683484`):

```
0x001e7a5b  push 4
0x001e7a5d  lea ecx, [esp+0x6c]
0x001e7a61  push 0x683484              ; "LOFF"
0x001e7a66  push ecx
0x001e7a6b  call 0x21d6f0              ; memcmp
0x001e7a73  test eax, eax
0x001e7a75  jne 0x1e7cf9               ; not LOFF -> seek past and continue
```

and it only accepts 16 bpp or 24 bpp pixel formats:

```
0x001e7d1d  mov eax, [esp+0x40]        ; bytes/pixel*8?
0x001e7d21  cmp eax, 0x10
0x001e7d24  je 0x1e7de7                ; 16 bpp
0x001e7d2a  cmp eax, 0x18
0x001e7d2d  je 0x1e7d44                ; 24 bpp -> [esp+0x5c] = 3
```

`icon.fon`, despite being 396 kB and starting like a TXR blob, is therefore
**not** a different container — it is the same LOFF-wrapped `.fon` as
`matrix.fon`, just with 400×496 pixels.  **[P]**

### 2.3 Orientation flag (header byte `0x11`)

| file | 0x11 | correct reading |
|---|---|---|
| MENU.FON | `0x00` | rows must be **flipped** (bottom-up / DIB order) |
| LARGE.FON, menublk, matrix*, icon | `0x20` | rows are top-down, no flip |

**[I]**: the correlation is exact over all seven files and `0x20` is a
plausible "origin top-left" flag, but no code path that *tests* this bit was
located.  Practically: `vis = flipud(pixel_block)` when `0x11 == 0`, else
`vis = pixel_block`.

This is measurable, not cosmetic: sampling MENU.FON without the flip yields
`0x20 → 'B'-like`, `0x41 → blank`; with the flip `0x20` is empty and
`0x41` is `A`.

---

## 3. Glyph geometry — the `.FON` is a 16×16 grid of cells

`.FON` has **no glyph directory**.  Every `.FON` file is one texture that is
cut into a 16×16 grid of character cells; the character code *is* the cell
index.  This is the font builder in `FUN_005e8e20`:

```
; ---- cell metrics ----------------------------------------------------
0x001e8e73  mov edi, eax               ; edi = parsed texture
0x001e8e7e  mov cx, word [edi+0x14]    ; texture WIDTH
0x001e8e86  shr ecx, 4
0x001e8e89  mov [ebx+0x18], ecx        ; F1 = width  >> 4  = cell width
0x001e8e8c  mov dx, word [edi+0x16]    ; texture HEIGHT
0x001e8e90  shr edx, 4
0x001e8e93  dec edx
0x001e8e97  mov [ebx+0x14], edx        ; F2 = (height >> 4) - 1 = glyph rows

; ---- per-code tables + packed 1-bpp bitmap --------------------------
0x001e8e79  push 0x100
0x001e8e9a  call 0x232643              ; malloc(256)
0x001e8ea4  mov [ebx+0x0c], eax        ; B[c] : "width"  table
0x001e8e9f  push 0x100
0x001e8ea7  call 0x232643
0x001e8eac  mov [ebx+0x10], eax        ; A[c] : "left"   table

0x001e8eaf  mov eax, [ebx+0x18]        ; cell width in pixels
0x001e8eb2  dec eax
0x001e8eb4  and edx, 7
0x001e8eb9  sar eax, 3
0x001e8ebc  inc eax                    ; bytes per packed row = (cols-1)/8+1
0x001e8ebd  mov [esp+0x4c], eax
0x001e8ec1  imul eax, [ebx+0x14]       ; * glyph rows
0x001e8ec9  shl eax, 8                 ; * 256 characters
0x001e8ecd  call 0x232643              ; malloc(...)
0x001e8ed5  mov [ebx+0x1c], eax        ; packed 1-bpp glyph store

; ---- glyph base address for code c ----------------------------------
0x001e8ee2  mov ebp, [esp+0x10]        ; c = 0..255
0x001e8ef0  mov esi, [ebx+0x14]        ; F2 = (H>>4)-1
0x001e8ef9  inc esi                    ; esi = H>>4   (strip height in rows)
0x001e8eff  sar eax, 4                 ; eax = c >> 4 (strip index)
0x001e8f02  imul esi, eax
0x001e8f07  mov ax, word [edi+0x14]    ; texture width = row stride
0x001e8f0b  imul esi, eax              ; esi = (H>>4)*(c>>4)*W
0x001e8f0e  and ebp, 0x8000000f        ; ebp = c & 15 (column)
0x001e8f1b  imul ebp, ecx              ; ecx = F1 = W>>4  (cell width)
0x001e8f1e  add esi, ebp
0x001e8f20  mov ebp, [edi+8]           ; pixel data pointer
0x001e8f23  add esi, ebp               ; -> pixel origin of code c
```

So, with `C` = code (0..255):

```
strip(C)  = C >> 4                 # 0..15, top-down in *visual* order
column(C) = C & 15
cell_w    = width  / 16
cell_h    = height / 16
glyph_h   = cell_h - 1             # rows the loader copies
origin(C) = strip(C) * cell_h * width + column(C) * cell_w   [pixels]
glyph row r (0 = top) = visual row  strip(C)*cell_h + 1 + r
```

**Cell row 0 is padding** — the loader starts at `+1` (see the `0x1E8F20`
region and the copy loop at `0x1E906D`), and in every shipped file the ink
lives in rows `1 .. cell_h-1`.  Verified for every font: `LARGE.FON` cell
`A` = rows 53..64, not 52..63.

### Measured cell sizes

| file | atlas | cell | glyph bitmap |
|---|---|---|---|
| MENU.FON | 96×144 | 6×9 | 6×8 |
| LARGE.FON, matrix*.fon | 112×208 | 7×13 | 7×12 |
| menublk.fon | 96×144 | 6×9 | 6×8 |
| icon.fon | 400×496 | 25×31 | 25×30 |

### Per-glyph metrics

The loader derives, for every code, a *left* value `A[c]` and a *width* value
`B[c]` by scanning **one** scanline of the cell (`[esp+0x18]` is the cell's
row 0 in buffer order):

```
0x001e8f25  xor eax, eax
0x001e8f27  test ecx, ecx              ; ecx = cell width in pixels
0x001e8f2f  cmp byte [eax+esi], 0      ; scan for first inked pixel
0x001e8f33  jne 0x1e8f3a
0x001e8f35  inc eax
0x001e8f38  jl 0x1e8f2f
0x001e8f3a  cmp eax, ecx
0x001e8f3c  mov edx, eax               ; edx = left edge
0x001e8f3e  jge 0x1e8f4f               ; nothing inked
0x001e8f40  cmp byte [eax+esi], 0      ; scan for first blank pixel
0x001e8f48  je 0x1e8f4f
0x001e8f4a  inc eax
0x001e8f4d  jl 0x1e8f40

0x001e904b  mov ecx, [ebx+0x10]
0x001e9050  mov byte [ecx+ebp], dl     ; A[c] = left       (dl = edx)
0x001e9055  inc eax
0x001e9056  mov edx, [ebx+0x0c]
0x001e9065  mov byte [edx+ebp], al     ; B[c] = right end + 1
```

Two consequences worth knowing:

* `A[c]`/`B[c]` are **single-byte** tables of 256 entries each, stored in the
  in-memory font object at `+0x10` / `+0x0C`; they are *derived at load time*
  and are **not present in the file**.
* Because only the cell's row 0 (in buffer order) is scanned, these values are
  not a reliable ink bounding box for the whole glyph — especially for
  `MENU.FON`, whose row 0 in buffer order is the glyph's *bottom* row.  A
  renderer should use `cell_w` as the advance (the atlas is a uniform grid) or
  an ink bbox computed over the whole glyph.  `tools/fon_probe.py` prints the
  ink bbox and uses `cell_w` as the advance.

The loader also copies the glyph into a packed 1-bpp bitmap
(`[font+0x1C]`, indexed `char * bytesPerRow * glyph_h`, MSB = leftmost
column):

```
0x001e906d  mov ax, word [edi+0x18]    ; bytes per pixel
0x001e907d  mov ax, word [edi+0x14]    ; ... row advance = texture width
0x001e9128  mov edx, 0x80
0x001e912f  sar edx, cl                ; cl = column & 7
0x001e9131  or byte [eax+esi], dl      ; set bit
0x001e9152  mov edi, [esp+0x14]
0x001e9170  mov eax, [esp+0x10]
0x001e917c  inc eax
0x001e917f  cmp eax, 0x100             ; for c = 0..255
0x001e9184  mov [esp+0x10], eax
```

The font object layout (shared with `.SFT`) is:

```
+0x00  vtable / flags
+0x04  first character code
+0x08  last  character code
+0x0C  B[c]  width table   (count bytes)
+0x10  A[c]  left  table   (count bytes)
+0x14  glyph height in rows
+0x18  glyph width  in pixels (F1)
+0x1C  packed 1-bpp glyph store
+0x20  -1
+0x28  DDraw surface / texture
```

`.FON` files use the whole range: `first = 0x00`, `last = 0xFF` (the loader
`malloc`s 256 entries unconditionally), so **code → glyph is the identity**.

Line height and baseline: line height is `cell_h` (9 px for MENU, 13 px for
LARGE); the baseline is the last row of the glyph box (`cell_h - 2` counting
the cell's padding row 0, i.e. row 7 / row 11 of the 8 / 12 row glyph).  The
observed ink band is rows 1..7 (MENU) and 1..9 plus descender rows (LARGE).

---

## 4. Character encoding

For every `.FON` file the code index is used directly, so the file's byte
values are the text encoding.  Decoding all 256 cells and reading them gives
**CP1251** — this is **[P]** for `MENU.FON` and `LARGE.FON` (they are two
sizes of the same character set):

* `0x20` space, `0x21..0x7E` = ASCII `!"#$…{|}~` (all **94/94** cells inked in
  both MENU.FON and LARGE.FON), `0x7F` = `⌂`
* `0x80..0xBF` = CP1251 symbols: `← ↑ → ↓`, `█`, `«`, `»`, `°`, `·`, `©`, …
* `0xC0..0xDF` = `А Б В Г Д Е Ж З И Й К Л М Н О П Р С Т У Ф Х Ц Ч Ш Щ Ъ Ы Ь Э Ю Я`
* `0xE0..0xFF` = `а б в г д е ж з и й к л м н о п р с т у ф х ц ч ш щ ъ ы ь э ю я`

`tools/out/fon_menu_cyr_text.png` (rendered from `MENU.FON` with
`--text "НОВАЯ ИГРА ЗАГРУЗИТЬ ВЫХОД"`) reads exactly that; the atlas PNGs read
`!"#$%&'()*+,-./`, `0123456789:;<=>?`, `@ABCDEFGHIJKLMNO`,
`PQRSTUVWXYZ[\]^_`, `` `abcdefghijklmno ``, `pqrstuvwxyz{|}~` and the Cyrillic
blocks in the expected CP1251 positions.

---

## 5. The `.SFT` container (different format, same font object)

`.SFT` is *not* a `.FON`.  It is a **1-bpp, fixed-size, glyph-major** bitmap
with an explicit character range.  Loader `FUN_005e8d10`:

```
0x001e8d49  push edi                   ; 0
0x001e8d4a  push 0x10
0x001e8d4c  push ebp                   ; file
0x001e8d4d  call 0x21e6a3              ; fseek(f, 0x10, SEEK_SET)
0x001e8d53  call 0x21fd8c              ; v1 -> ebx  = glyph count
0x001e8d5b  call 0x21fd8c              ; v2
0x001e8d60  add eax, 0x20
0x001e8d64  mov [esi+4], eax           ; first code = v2 + 0x20
0x001e8d67  lea eax, [eax+ebx-1]
0x001e8d6b  mov [esi+8], eax           ; last  code = first + count - 1
0x001e8d74  call 0x21fd8c              ; v3 (unused)
0x001e8d7a  mov [esi+0x18], eax        ; v4 = bits per row  (glyph width)
0x001e8d89  mov [esi+0x14], eax        ; v5 = rows           (glyph height)

0x001e8d83  push 0xfa
0x001e8d93  call 0x21e6a3              ; fseek(f, 0xfa, SEEK_SET)
0x001e8d98  call 0x232643              ; malloc(count)
0x001e8d9f  mov [esi+0x0c], eax        ; A[c] read from 0xfa (all zero here)
0x001e8dc6  call 0x21e20f              ; fread(table, count, 1, f)

0x001e8ddc  mov eax, [esi+0x18]        ; bits
0x001e8df0  sar edi, 3
0x001e8df3  inc edi                    ; bytes per glyph row
0x001e8df4  imul edi, [esi+0x14]       ; * rows
0x001e8dfd  imul edi, ebx              ; * count
0x001e8e00  push edi
0x001e8de2  push 0x158                 ; <- fixed bitmap offset
0x001e8df8  call 0x21e6a3              ; fseek(f, 0x158, SEEK_SET)
0x001e8e01  call 0x232643              ; malloc
0x001e8e0e  call 0x21e20f              ; fread(bitmap, size, 1, f)
```

Layout (byte budget verified against both shipped files):

```
0x00  1    0x10                      header size / magic
0x01 13    NUL-padded name           "SMALL.EFT", "D1.SFT"
0x0E  2    ?                         0x0ABA (MENU.SFT) / 0x06BA (D1.SFT)
0x10  1    glyph count               94 in both files
0x11  1    first code − 0x20         1  -> first code 0x21
0x12  1    0
0x13  1    bits per row              8
0x14  1    rows (glyph height)       16 (MENU.SFT) / 10 (D1.SFT)
0xFA  94   per-glyph table           all zero in both shipped files
0x158 ..   1-bpp bitmap, glyph-major, 1 byte per row, MSB = leftmost pixel
```

| file | size | header | bitmap | expected | check |
|---|---|---|---|---|---|
| MENU.SFT | 1848 | 344 | 1504 | 1·16·94 | 344+1504 = 1848 ✔ |
| D1.SFT   | 1284 | 344 |  940 | 1·10·94 | 344+940  = 1284 ✔ |

Code mapping: `code = 0x21 + glyph_index`, i.e. **94 glyphs covering
`0x21..0x7E`** — pure ASCII, no Cyrillic.  Verified by decoding all 94 and
reading them (`!` at index 0, `A` at 0x41, `a` at 0x61, `~` at 0x7E).

Line height = `0x14` (16 px for MENU.SFT); the ink band is rows 4..10, so the
baseline is row 10 and rows 0..3 / 11..15 are leading / descender space.

Because `menu.sft` is tried before `menu.fon`, **`MENU.SFT` is the font the
current start-menu code prefers**, and it is Latin-only.  Any Russian string
must therefore go through `MENU.FON` (CP1251).

---

## 6. `icon.fon`

`icon.fon` uses the *same* LOFF container and the *same* 16×16 grid rule as
`matrix.fon` — [P].  Its 400×496 atlas gives 25×30 pixel cells.  It is not a
text font: opening `tools/out/fon_icon_atlas.png` shows two identical blocks
of dashboard icons — digits `0`-`9` in two weights, `:`, left/straight/right
turn arrows, box-truck and truck+trailer silhouettes, "empty tank" bar
indicators, an `(!)` warning lamp, and large triangular arrowheads — plus
`(!)` / fuel-warning telltales in the right-hand cells.  Only 57 of the 94
ASCII cells are inked and their shapes are not letterforms.  So the answer to
the "is `icon.fon` a genuinely different container?" question is **no** — it
is a TXR/LOFF-wrapped `.FON` like `menublk.fon`; only *variant B* exists for
it, *variant A* (the plain 786-byte header) is used solely by `MENU.FON` and
`LARGE.FON`.

---

## 7. Tool

```
python3 tools/fon_probe.py <font-file> [--text "SINGLE PLAYER"] [--out DIR]
                           [--text-out NAME] [--proportional] [--encoding CP]
```

Prints the container, atlas size, bpp, orientation, cell/glyph geometry,
glyph count, line height, baseline, ink band and a per-glyph table
(`code char left ink_width advance`), then writes
`<out>/fon_<stem>_atlas.png` (all glyphs in a labelled grid) and
`<out>/fon_<stem>_text.png` (the `--text` sample).  `--text` is encoded with
the font's own code page (`cp1251` for `.FON`, `latin-1` for `.SFT`).

Proof images (all opened and read during decoding):

| PNG | what it shows |
|---|---|
| `tools/out/fon_menu_atlas.png` | MENU.FON, all 256 cells: ASCII 0x21-0x7E, CP1251 symbols at 0x80-0xBF, `А…Я` at 0xC0-0xDF, `а…я` at 0xE0-0xFF |
| `tools/out/fon_menu_text.png` | MENU.FON rendering of `SINGLE PLAYER` |
| `tools/out/fon_menu_cyr_text.png` | MENU.FON rendering of `НОВАЯ ИГРА ЗАГРУЗИТЬ ВЫХОД` |
| `tools/out/fon_large_atlas.png` | LARGE.FON, all 256 cells (same CP1251 set, bold 7×12) |
| `tools/out/fon_large_cyr_text.png` | LARGE.FON rendering of `НОВАЯ ИГРА / SINGLE PLAYER` |
| `tools/out/fon_menublk_atlas.png`, `fon_matrix_atlas.png` | the 16-bpp LOFF twins of MENU/LARGE |
| `tools/out/fon_menu_sft_atlas.png` | MENU.SFT's 94 ASCII glyphs (8×16) |
| `tools/out/fon_icon_atlas.png` | icon.fon's dashboard icons |

---

## 8. Proven vs inferred vs open

**[P] Proven**

1. `.FON` is one texture cut into a **16×16 grid of cells**; `cell_w = W>>4`,
   `glyph_h = (H>>4)-1`, `strip = code>>4`, `column = code & 15`,
   `origin = strip*cell_h*W + column*cell_w`.  (loader `0x1E8E7E`–`0x1E8F23`)
2. Cell row 0 is padding; glyph rows are `cell_h-1` rows starting at `+1`.
3. `code → glyph` is the **identity** for `.FON` (256-entry tables,
   `first = 0`, `last = 0xFF`).
4. Plain `.FON` header is **786 bytes**; pixel data is `W*H*bpp` bytes at the
   end.  MENU.FON is 8 bpp, LARGE.FON is 8 bpp.
5. LOFF `.FON` pixels start at file offset **30**, 16 bpp, `n = 2WH+30`.
   `icon.fon` is the same container as `matrix.fon`, not a new one.
6. `MENU.FON` and `LARGE.FON` are **CP1251** fonts; the code→glyph table above
   is exact for every one of the 256 codes.
7. `MENU.FON` needs a vertical flip (bottom-up rows) and `LARGE.FON` does not.
8. `.SFT` layout: count@0x10, first−0x20@0x11, bits@0x13, rows@0x14,
   94-byte table@0xFA, 1-bpp glyph-major bitmap@0x158; MENU.SFT = 8×16 ASCII
   `0x21..0x7E`, D1.SFT = 8×10 ASCII `0x21..0x7E`.

**[I] Inferred**

1. Header byte `0x11` is an orientation flag (`0x00` = bottom-up,
   `0x20` = top-down).  Exact correlation over 7 files, but the code that
   tests the bit was not located — the loaders (`0x1E8E20`, `0x1E8D10`) never
   read it, so the flip is applied by the surface/parser layer.
2. The 786-byte prefix is `18-byte head + 768-byte (256·3) palette area`; the
   `00 04 04 04 fc fc fc` table is the level/greyscale ramp for the 8-bit
   indices.
3. The 94-byte table at `0xFA` in `.SFT` is a per-glyph width table (it is
   read by the loader into `[font+0x0C]`) — zero in both shipped files, so
   `.SFT` behaves as a fixed-pitch font in practice.
4. `matrix.fon`, `matrix1.fon` and `matrix2.fon` share the same header,
   dimensions, cell size and ASCII glyphs (all three decode `0x41` to the
   identical bold `A`), but they are **not** byte-identical: `matrix.fon` and
   `matrix2.fon` differ in 12844 of 46666 bytes, i.e. they are three
   different pictures in the same container.  Which parts differ (ASCII block
   vs. high block) was not mapped.

**[O] Open / not verified**

1. The meaning of the plain-`.FON` header fields `0x00..0x0B`
   (`0x0100, 1, 0, 0x1801, 0`) and of byte `0x13` is unknown; they are
   constant across MENU.FON / LARGE.FON and are not read by the located
   loader entry point.
2. `tools/fon_probe.py` renders with a `cell_w` monospace advance.  The game's
   *actual* advance algorithm — and in particular whether the renderer blits
   the DDraw surface or re-expands the packed 1-bpp store built at
   `0x1E906D`–`0x1E9152` — was not traced.  The single-scanline `A[c]/B[c]`
   tables look unsuitable as proportional metrics, which is the one loose end.
   **Concrete experiment that would settle it:** set a breakpoint / read the
   text-drawing routine that consumes `[font+0xB30]` — find the caller that
   indexes `[font+0x0C]`/`[font+0x10]` and blits a sub-rectangle of
   `[font+0x28]`; two or three such sites exist in the `0x501000`–`0x530000`
   range and reading the one that takes a `char*` argument gives the advance
   rule directly.
3. Whether the *game* actually uses `MENU.FON`'s Cyrillic or only `MENU.SFT`'s
   Latin (the two `menu.fon` call sites at `0x11129A` / `0x1280C5` versus the
   `menu.sft`-first path at `0x1DDD38`) was not resolved — both loaders work,
   and both files decode cleanly.

#!/usr/bin/env python3
"""
fon_probe.py -- decoder for the HT2:RE (Hard Truck 2: King of the Road) bitmap
fonts used by the game:  *.FON  and  *.SFT.

Format was reverse engineered from king.exe (see docs/fon_format.md for the
full derivation and the quoted disassembly).  Summary of what the loader does
(font loader at VA 0x5E8E20 / file offset 0x1E8E20):

    cols   = texture_width  >> 4          ; cell width in pixels
    rows   = (texture_height >> 4) - 1    ; glyph height in pixels
    stride = texture_width                ; bytes per scanline (bpp=1) 
    cell(c)   -> strip = c >> 4, column = c & 15
    origin(c) -> strip * (height>>4) * width + column * (width>>4)

All *.FON files are a single 16:16 grid of character cells; each cell is
(width/16) x (height/16) pixels.  The glyph itself occupies rows 1 .. cell_h-1
of the cell; row 0 is padding.

Three containers were found:
  * plain      : 786 byte header + w*h bytes of 8bpp pixels   (MENU.FON, LARGE.FON)
  * LOFF/TXR   : 30 byte header + 2*w*h bytes of 16bpp pixels (menublk/matrix*/icon.fon)
  * SFT        : 1bpp, 8 x N pixel glyphs, ASCII 0x21..0x7E   (MENU.SFT, D1.SFT)

Header byte 0x11 is an orientation flag: 0x20 -> rows stored top-down,
0x00 -> rows stored bottom-up (DIB order).  MENU.FON uses 0x00.

Usage:
    python3 tools/fon_probe.py <font-file> [--text "SINGLE PLAYER"] [--out DIR]
"""

import argparse
import os
import struct
import sys
import zlib

# --------------------------------------------------------------------------
# minimal PNG writer (stdlib only: zlib + struct)
# --------------------------------------------------------------------------


def write_png(path, width, height, pixels):
    """pixels: flat list/bytearray of width*height gray levels (0..255)."""

    def chunk(tag, payload):
        body = tag + payload
        return (
            struct.pack(">I", len(payload))
            + body
            + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
        )

    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter type 0
        raw += bytes(pixels[y * width : (y + 1) * width])
    out = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )
    with open(path, "wb") as fh:
        fh.write(out)


# --------------------------------------------------------------------------
# tiny 3x5 label font, so atlases can be annotated without PIL
# --------------------------------------------------------------------------
_DIGITS = {
    "0": (0b111, 0b101, 0b101, 0b101, 0b111),
    "1": (0b010, 0b110, 0b010, 0b010, 0b111),
    "2": (0b111, 0b001, 0b111, 0b100, 0b111),
    "3": (0b111, 0b001, 0b111, 0b001, 0b111),
    "4": (0b101, 0b101, 0b111, 0b001, 0b001),
    "5": (0b111, 0b100, 0b111, 0b001, 0b111),
    "6": (0b111, 0b100, 0b111, 0b101, 0b111),
    "7": (0b111, 0b001, 0b001, 0b001, 0b001),
    "8": (0b111, 0b101, 0b111, 0b101, 0b111),
    "9": (0b111, 0b101, 0b111, 0b001, 0b111),
    "A": (0b111, 0b101, 0b111, 0b101, 0b101),
    "B": (0b110, 0b101, 0b110, 0b101, 0b110),
    "C": (0b111, 0b100, 0b100, 0b100, 0b111),
    "D": (0b110, 0b101, 0b101, 0b101, 0b110),
    "E": (0b111, 0b100, 0b110, 0b100, 0b111),
    "F": (0b111, 0b100, 0b110, 0b100, 0b100),
    "X": (0b101, 0b101, 0b010, 0b101, 0b101),
    "-": (0b000, 0b000, 0b111, 0b000, 0b000),
    ".": (0b000, 0b000, 0b000, 0b000, 0b010),
    "?": (0b111, 0b001, 0b011, 0b000, 0b010),
    " ": (0, 0, 0, 0, 0),
}


class Canvas:
    """Trivial 8-bit grayscale canvas."""

    def __init__(self, w, h, bg=24):
        self.w = w
        self.h = h
        self.px = bytearray([bg]) * (w * h)

    def set(self, x, y, v):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y * self.w + x] = v

    def rect(self, x0, y0, x1, y1, v):
        for y in range(max(0, y0), min(self.h, y1)):
            base = y * self.w
            for x in range(max(0, x0), min(self.w, x1)):
                self.px[base + x] = v

    def text(self, x, y, s, v=200, scale=1):
        cx = x
        for ch in s.upper():
            pat = _DIGITS.get(ch, _DIGITS["?"])
            for r in range(5):
                for c in range(3):
                    if pat[r] >> (2 - c) & 1:
                        self.rect(
                            cx + c * scale,
                            y + r * scale,
                            cx + (c + 1) * scale,
                            y + (r + 1) * scale,
                            v,
                        )
            cx += 4 * scale

    def save(self, path):
        write_png(path, self.w, self.h, self.px)


# --------------------------------------------------------------------------
# container decoders
# --------------------------------------------------------------------------


class Font:
    def __init__(self):
        self.path = ""
        self.container = ""
        self.width = 0        # atlas width  (pixels)
        self.height = 0       # atlas height (pixels)
        self.bits = 0         # bits per pixel of the atlas
        self.flag = 0         # header byte 0x11
        self.top_down = True
        self.pixel_offset = 0
        self.cell_w = 0
        self.cell_h = 0
        self.glyph_h = 0      # rows the loader copies (cell_h - 1)
        self.n_glyphs = 0
        self.first_code = 0
        self.rows = {}        # code -> list of rows, each row list of 0/1
        self.encoding = "cp1251"   # byte encoding the code indices refer to
        self.is_sft = False
        self.sft_row_bytes = 0
        self.sft_bitmap_off = 0
        self.widths = {}      # code -> advance derived from ink bbox

    # ---- geometry helpers -------------------------------------------------
    @property
    def advance_h(self):
        return self.cell_h if not self.is_sft else self.cell_h

    def ink_bbox(self, code):
        g = self.rows.get(code)
        if not g:
            return None
        xs = [x for row in g for x, v in enumerate(row) if v]
        if not xs:
            return None
        return min(xs), max(xs)

    def advance(self, code, proportional=False):
        if not proportional:
            # the game's atlas is a uniform grid of cells, so the natural
            # advance is one cell; this reproduces the original spacing.
            return self.cell_w
        bb = self.ink_bbox(code)
        if bb is None:
            return max(1, self.cell_w // 3)
        return bb[1] + 2   # ink extent + one blank column


def _deinterleave(data, width, height, bits, pixel_offset, top_down):
    """Return rows[y][x] -> 0/1 for the atlas."""
    stride = width * (bits // 8)
    n = stride * height
    blob = data[pixel_offset : pixel_offset + n]
    if len(blob) < n:
        raise ValueError("pixel block truncated")
    rows = []
    bpp = bits // 8
    for y in range(height):
        base = y * stride
        line = [0] * width
        for x in range(width):
            if bpp == 1:
                line[x] = 1 if blob[base + x] else 0
            else:
                v = blob[base + x * 2] | (blob[base + x * 2 + 1] << 8)
                line[x] = 1 if v else 0
        rows.append(line)
    if not top_down:
        rows.reverse()
    return rows


def parse_fon(data, path):
    f = Font()
    f.path = path
    f.width = struct.unpack_from("<H", data, 0x0C)[0]
    f.height = struct.unpack_from("<H", data, 0x0E)[0]
    f.bits = data[0x10]
    f.flag = data[0x11]

    if data[0x12:0x16] == b"LOFF":
        # TXR/LOFF wrapped: <14 byte head> LOFF <u32 4> <u32 chunk size> <16bpp pixels>
        f.container = "LOFF/TXR wrapped"
        f.pixel_offset = 30
    else:
        # plain: 786 byte header (18 byte head + 768 byte palette area, all zero)
        f.container = "plain .FON"
        f.pixel_offset = len(data) - f.width * f.height * (f.bits // 8)

    f.top_down = bool(f.flag & 0x20)
    f.cell_w = f.width // 16
    f.cell_h = f.height // 16
    f.glyph_h = f.cell_h - 1
    f.n_glyphs = 256
    f.first_code = 0

    rows = _deinterleave(
        data, f.width, f.height, f.bits, f.pixel_offset, f.top_down
    )
    for c in range(256):
        s, k = c >> 4, c & 15
        y0 = f.cell_h * s + 1          # row 0 of the cell is padding
        x0 = f.cell_w * k
        f.rows[c] = [
            rows[y0 + r][x0 : x0 + f.cell_w] for r in range(f.glyph_h)
        ]
    return f


def parse_sft(data, path):
    f = Font()
    f.path = path
    f.is_sft = True
    f.container = ".SFT"
    count = data[0x10]
    f.first_code = data[0x11] + 0x20       # loader: lea [v2 + 0x20]
    f.bits = data[0x13]
    f.cell_h = data[0x14]
    f.sft_row_bytes = (f.bits + 7) // 8
    f.cell_w = f.sft_row_bytes * 8
    f.glyph_h = f.cell_h
    f.sft_bitmap_off = 0x158
    f.n_glyphs = count
    f.encoding = "latin-1"
    f.width = count
    f.height = f.cell_h
    f.top_down = True
    for i in range(count):
        base = f.sft_bitmap_off + i * f.sft_row_bytes * f.cell_h
        g = []
        for r in range(f.cell_h):
            b = data[base + r * f.sft_row_bytes]
            g.append([(b >> (7 - x)) & 1 for x in range(f.cell_w)])
        f.rows[f.first_code + i] = g
    return f


def load_font(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if len(data) >= 0x16 and data[0x12:0x16] == b"LOFF":
        return parse_fon(data, path)
    if len(data) >= 0x160 and data[0x00] == 0x10 and data[0x01:2].isalpha():
        # .SFT: byte0 = 0x10, then a NUL padded name, bitmap at 0x158
        return parse_sft(data, path)
    return parse_fon(data, path)


# --------------------------------------------------------------------------
# reporting
# --------------------------------------------------------------------------


def ink_band(f):
    """Lowest/highest row that carries ink in any glyph of the font."""
    lo, hi = None, None
    for g in f.rows.values():
        for r, row in enumerate(g):
            if any(row):
                lo = r if lo is None else min(lo, r)
                hi = r if hi is None else max(hi, r)
    return lo, hi


def describe(f, verbose=True):
    print("file            : %s" % f.path)
    print("container       : %s" % f.container)
    if f.is_sft:
        print("glyphs          : %d  (codes 0x%02X..0x%02X)"
              % (f.n_glyphs, f.first_code, f.first_code + f.n_glyphs - 1))
        print("glyph size      : %d x %d pixels (1 bpp, 1 byte/row, MSB left)"
              % (f.cell_w, f.glyph_h))
        print("bitmap offset   : 0x%X, %d bytes"
              % (f.sft_bitmap_off, f.n_glyphs * f.cell_h * f.sft_row_bytes))
        print("line height     : %d px" % f.cell_h)
        print("baseline        : row %d of the glyph cell" % (f.cell_h - 1))
    else:
        print("atlas size      : %d x %d pixels, %d bpp, pixel data @ 0x%X"
              % (f.width, f.height, f.bits, f.pixel_offset))
        print("header bits     : byte 0x10 = %d (bits/px), byte 0x11 = 0x%02X"
              % (f.bits, f.flag))
        print("row orientation : %s" % ("top-down" if f.top_down else
                                         "bottom-up (DIB order, flipped)"))
        print("grid            : 16 x 16 cells of %d x %d pixels"
              % (f.cell_w, f.cell_h))
        print("glyph bitmap    : %d x %d pixels (cell row 0 is padding)"
              % (f.cell_w, f.glyph_h))
        print("glyph count     : %d (codes 0x00..0xFF, loader uses identity)"
              % f.n_glyphs)
        print("line height     : %d px" % f.cell_h)
        print("baseline        : row %d of the glyph cell" % (f.glyph_h - 1))
    # prefix width table
    lo, hi = ink_band(f)
    print("ink band        : rows %s..%s of the glyph box (baseline row %s)"
          % (lo, hi, hi))
    codes = sorted(f.rows)
    print("per-glyph widths (code char  left  ink_w  advance):")
    line = []
    for c in codes:
        bb = f.ink_bbox(c)
        if bb is None:
            if f.is_sft and c in (f.first_code + 0x20 - 0x41,):
                pass
            left, w = ".", 0
        else:
            left, w = bb[0], bb[1] - bb[0] + 1
        ch = chr(c) if 33 <= c <= 126 else "."
        line.append("%02X %s %2s %2s %2d" % (c, ch, left, w, f.advance(c)))
        if len(line) == 6:
            print("   " + " | ".join(line))
            line = []
    if line:
        print("   " + " | ".join(line))
    return f


# --------------------------------------------------------------------------
# rendering
# --------------------------------------------------------------------------


def save_atlas(f, out_dir):
    cols, scale, gap, lab = 16, 4, 8, 8
    if f.is_sft:
        cw, ch = f.cell_w, f.glyph_h
        rows = (f.n_glyphs + cols - 1) // cols
        codes = list(range(f.first_code, f.first_code + f.n_glyphs))
    else:
        cw, ch = f.cell_w, f.glyph_h
        rows = 16
        codes = list(range(256))
    cellw = cw * scale
    cellh = ch * scale
    W = gap + cols * (cellw + gap)
    H = gap + rows * (cellh + lab + gap)
    cv = Canvas(W, H, 24)
    for i, c in enumerate(codes):
        gx, gy = i % cols, i // cols
        x0 = gap + gx * (cellw + gap)
        y0 = gap + gy * (cellh + lab + gap) + lab
        g = f.rows.get(c)
        if g:
            for r, row in enumerate(g):
                for x, v in enumerate(row):
                    if v:
                        cv.rect(x0 + x * scale, y0 + r * scale,
                                x0 + (x + 1) * scale, y0 + (r + 1) * scale, 255)
        cv.text(x0, y0 - lab + 1, "%02X" % c, 170, 1)
        if 33 <= c <= 126:
            cv.text(x0 + 20, y0 - lab + 1, chr(c), 110, 1)
    name = os.path.join(out_dir, "fon_%s_atlas.png" % _slug(f))
    cv.save(name)
    return name


def save_text(f, text, out_dir, fname=None, proportional=False):
    scale = 6
    margin = 16

    # encode the sample into the font's own 8-bit code page
    raw = text.encode(f.encoding, errors="replace")

    widths = []
    for c in raw:
        widths.append(f.advance(c, proportional) if c in f.rows
                      else f.cell_w // 2)
    line_w = sum(widths) + 1
    W = margin * 2 + max(1, line_w) * scale
    H = margin * 2 + f.glyph_h * scale
    cv = Canvas(W, H, 24)
    x = margin
    for c, adv in zip(raw, widths):
        g = f.rows.get(c)
        if g:
            for r, row in enumerate(g):
                for xx, v in enumerate(row):
                    if v:
                        cv.rect(x + xx * scale, margin + r * scale,
                                x + (xx + 1) * scale, margin + (r + 1) * scale,
                                255)
        x += adv * scale
    name = os.path.join(out_dir, fname or ("fon_%s_text.png" % _slug(f)))
    cv.save(name)
    return name


def _slug(f):
    stem = os.path.splitext(os.path.basename(f.path))[0].lower()
    return stem + "_sft" if f.is_sft else stem


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("font", help="path to a .FON or .SFT file")
    ap.add_argument("--text", default="SINGLE PLAYER", help="sample text")
    ap.add_argument("--out", default="tools/out", help="output directory for PNGs")
    ap.add_argument("--quiet", action="store_true", help="only print the header")
    ap.add_argument("--text-out", default=None,
                    help="override the sample-text PNG file name")
    ap.add_argument("--proportional", action="store_true",
                    help="space glyphs by ink width instead of the cell grid")
    ap.add_argument("--encoding", default=None,
                    help="8-bit code page of the sample text (default: the "
                         "font's own, cp1251 for .FON / latin-1 for .SFT)")
    args = ap.parse_args(argv)

    f = load_font(args.font)
    if args.encoding:
        f.encoding = args.encoding
    describe(f, verbose=not args.quiet)

    os.makedirs(args.out, exist_ok=True)
    atlas = save_atlas(f, args.out)
    text = save_text(f, args.text, args.out, args.text_out, args.proportional)
    print("text encoding   : %s" % f.encoding)
    print("atlas PNG       : %s" % atlas)
    print("text  PNG       : %s   (%r)" % (text, args.text))
    return 0


if __name__ == "__main__":
    sys.exit(main())

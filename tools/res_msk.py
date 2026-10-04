#!/usr/bin/env python3
"""
HT2:RE - reader/decoder for Hard Truck 2: King of the Road
`.RES` containers and the `.msk` (MASK) / `.txr` (TEXTURE) blobs inside them.

Everything here was derived from the original PE32 binary `original/king.exe`
(ImageBase 0x400000, so file_offset + 0x400000 == virtual address) and then
checked against a byte-exact consumption budget over every mask and texture of
every `.res` file shipped with the GOG release.  See docs/msk_format.md for the
disassembly the format was derived from.

Key king.exe addresses (as file offsets, i.e. `r2 -c 'pd N @ 0x...'`):

  0x1e94d0  mask header parser (tag / w / h / data-size, size = arg3-0x308)
  0x1e9690  mask blob loader    (fseek to blob+0x308 then fread)
  0x1bc1a9  the RLE *decoder*   (token = <bit7|count7>)
  0x1bbf00  8bpp -> 16bpp palette expander, uses the same tokens
  0x1e9910  file-based mask loader (same header layout)
  0x1e7930  .txr chunk loader ("LOFF"/"LVLS"/"LVMP"/"PFRM"/"ENDR")

CLI:
  python3 tools/res_msk.py <res-file> [--list] [--mask NAME] [--txr NAME]
                          [--out DIR] [--all] [--verify]

  (--mask/--txr may be repeated.)
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
import zlib

# --------------------------------------------------------------------------- #
# constants derived from the binary
# --------------------------------------------------------------------------- #

MASK_HEADER_BYTES = 0x308        # 776 : 4 tag + 4 w/h + 768 palette bytes
PALETTE_BYTES = 768              # 256 * 3
TRAILER_PFRM = b"PFRM"
TRAILER_ENDR = b"ENDR"

# tag -> bit depth (word[mask+8] in the original objects)
MASK_TAGS = {b"MASK": 1, b"MSKR": 1, b"MSK8": 1, b"MS16": 2}

# .txr wrapper magic (u16 at offset 0, u16 at offset 2)
TXR_MAGIC = (12, 2)
# 18-byte paletted-bitmap texture header used by the LOAD.RES art
RAWTEX_MAGIC = (0x0100, 0x0001)

TRANSPARENT_INDEX = 0            # 8bpp masks: palette entry 0 is the key


# --------------------------------------------------------------------------- #
# PNG writer (stdlib only)
# --------------------------------------------------------------------------- #

def write_png(path: str, width: int, height: int, rgba: bytes | bytearray) -> None:
    """Write an 8-bit RGBA PNG."""
    if len(rgba) != width * height * 4:
        raise ValueError("pixel buffer size mismatch")

    def chunk(tag: bytes, payload: bytes) -> bytes:
        body = tag + payload
        return (struct.pack(">I", len(payload)) + body +
                struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)                                    # filter: none
        raw += rgba[y * stride:(y + 1) * stride]

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    data = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(data)


# --------------------------------------------------------------------------- #
# colour helpers
# --------------------------------------------------------------------------- #

def channel_shift_bits(mask: int) -> tuple[int, int]:
    """(shift, width) of a channel mask such as 0xF800 -> (11, 5)."""
    if mask == 0:
        return 0, 0
    shift = (mask & -mask).bit_length() - 1
    return shift, bin(mask >> shift).count("1")


def expand_channel(value: int, mask: int) -> int:
    """Extract a channel and scale it to 0..255.  mask == 0 -> opaque 255."""
    if mask == 0:
        return 255
    shift, bits = channel_shift_bits(mask)
    v = (value & mask) >> shift
    return v * 255 // ((1 << bits) - 1)


# --------------------------------------------------------------------------- #
# .RES container
# --------------------------------------------------------------------------- #

class Entry:
    __slots__ = ("name", "flags", "size", "offset", "section")

    def __init__(self, name, flags, size, offset, section):
        self.name, self.flags, self.size = name, flags, size
        self.offset, self.section = offset, section

    @property
    def base(self):
        """The part of the name before the first space."""
        return self.name.split(" ", 1)[0]

    def __repr__(self):
        return f"<Entry {self.section} {self.name!r} size={self.size}>"


def _cstr(data: bytes, pos: int) -> tuple[str, int]:
    end = data.index(b"\0", pos)
    return data[pos:end].decode("cp1251", "replace"), end + 1


def parse_res(path: str) -> tuple[bytes, dict[str, list[Entry]], list[tuple[str, int]]]:
    """Parse the textual RES directory.

    Returns (file bytes, {SECTION: [Entry]}, [(SECTION, count)]).
    Framing:  `<NAME> <count>\\0` then `count` NUL-terminated strings; for the
    `*FILES` sections each element is `<name> <flags>\\0` + `<u32 size>` + payload.
    """
    with open(path, "rb") as fh:
        data = fh.read()

    sections: dict[str, list[Entry]] = {}
    order: list[tuple[str, int]] = []
    pos = 0
    while pos < len(data):
        try:
            line, pos = _cstr(data, pos)
        except ValueError:
            break
        parts = line.split(" ")
        if len(parts) != 2 or not parts[1].isdigit():
            break
        name, count = parts[0], int(parts[1])
        order.append((name, count))
        if name.endswith("FILES"):
            entries: list[Entry] = []
            for _ in range(count):
                nm, pos = _cstr(data, pos)
                size = struct.unpack_from("<I", data, pos)[0]
                pos += 4
                entries.append(Entry(nm, "", size, pos, name))
                pos += size
            sections[name] = entries
        else:
            for _ in range(count):
                _, pos = _cstr(data, pos)
            sections.setdefault(name, [])
    return data, sections, order


def res_palette(data: bytes, sections) -> int | None:
    """Byte offset of the first palette triple inside the `.plm` entry.

    PLM layout: "PLM\\0" u32 length, then "PALT" u32 `0x300`, then 256 RGB triples.
    """
    for e in sections.get("PALETTEFILES", []):
        blob = data[e.offset:e.offset + e.size]
        at = blob.find(b"PALT")
        if at >= 0:
            return e.offset + at + 8
    return None


# --------------------------------------------------------------------------- #
# MASK blobs
# --------------------------------------------------------------------------- #

class MaskInfo:
    def __init__(self, tag, width, height, depth, consumed, blob_size,
                 trailer, rgba, gray_alpha, palette):
        self.tag, self.width, self.height = tag, width, height
        self.depth, self.consumed, self.blob_size = depth, consumed, blob_size
        self.trailer, self.rgba = trailer, rgba
        self.gray_alpha, self.palette = gray_alpha, palette

    def __str__(self):
        return (f"{self.tag.decode()} {self.width}x{self.height} "
                f"depth={self.depth} consumed={self.consumed}/"
                f"{self.blob_size - MASK_HEADER_BYTES} data bytes "
                f"(blob {self.blob_size}), trailer={self.trailer}")


def rle_expand(src: bytes, width: int, height: int, depth: int):
    """The RLE from king.exe @ 0x1bc1a9.

    Token stream, one byte `c` per run, `n = c & 0x7f`:

        c & 0x80  ->  n *transparent* pixels   (the engine zero-fills n*depth bytes)
        c & 0x7f  ->  n *literal* pixels, followed by n*depth raw bytes

    A row is finished as soon as its pixel counter reaches `width`, so `n` may
    legally overshoot the row end (it never does in the shipped data: every row
    of all 1056 masks sums exactly to `width`).

    Returns (decoded bytes, source bytes consumed).
    """
    out = bytearray(width * height * depth)
    pos = 0
    i = 0
    n_src = len(src)
    for _ in range(height):
        x = 0
        while x < width:
            if i >= n_src:
                raise ValueError("RLE stream truncated")
            c = src[i]
            i += 1
            n = c & 0x7F
            run = n * depth
            if c & 0x80:
                x += n                                   # bytes already zero
                pos += run
            else:
                if i + run > n_src:
                    raise ValueError("RLE literal run overruns the blob")
                out[pos:pos + run] = src[i:i + run]
                i += run
                x += n
                pos += run
    return bytes(out[:width * height * depth]), i


def decode_mask(blob: bytes, palette_override=None):
    """Decode a MASK/MSKR/MSK8/MS16 blob.

    Layout (proven byte-exact):

        +0x000  char[4]  tag : "MASK" | "MSKR" | "MSK8" | "MS16"
        +0x004  u16      width
        +0x006  u16      height
        +0x008  u8[768]  256 * RGB triples (8bpp palette; all-zero for MS16)
        +0x308  ...      RLE stream, exactly (blob_size - 776) bytes
        [MS16 only]      "PFRM" chunk + "ENDR" chunk (44 bytes)
    """
    if len(blob) < MASK_HEADER_BYTES or blob[:4] not in MASK_TAGS:
        raise ValueError("not a mask blob")
    tag = blob[:4]
    depth = MASK_TAGS[tag]
    width, height = struct.unpack_from("<HH", blob, 4)
    palette = blob[8:8 + PALETTE_BYTES]
    src = blob[MASK_HEADER_BYTES:]

    # The RLE is self-terminating at width*height pixels; anything after it is
    # a chunk trailer (MS16) which we parse rather than guess.
    raw, consumed = rle_expand(src, width, height, depth)

    trailer = ""
    rest = src[consumed:]
    masks: list[int] = []
    if rest:
        if rest[:4] != TRAILER_PFRM:
            raise ValueError(f"unexpected trailer {rest[:4]!r}")
        plen = struct.unpack_from("<I", rest, 4)[0]
        masks = list(struct.unpack_from("<%dI" % (plen // 4), rest, 8))
        after = rest[8 + plen:]
        if not after.startswith(TRAILER_ENDR):
            raise ValueError("missing ENDR after PFRM")
        trailer = f"PFRM({plen}B){[hex(m) for m in masks]} + ENDR"

    # ---- compose RGBA -------------------------------------------------------
    rgba = bytearray(width * height * 4)
    if depth == 1:
        for p in range(width * height):
            idx = raw[p]
            r, g, b = palette[idx * 3:idx * 3 + 3]
            opaque = 0 if idx == TRANSPARENT_INDEX else 255
            rgba[p * 4:p * 4 + 4] = bytes((r, g, b, opaque))
    else:
        rmask = masks[0] if len(masks) > 0 else 0x7C00
        gmask = masks[1] if len(masks) > 1 else 0x03E0
        bmask = masks[2] if len(masks) > 2 else 0x001F
        amask = masks[3] if len(masks) > 3 else 0
        for p in range(width * height):
            v = struct.unpack_from("<H", raw, p * 2)[0]
            r = expand_channel(v, rmask)
            g = expand_channel(v, gmask)
            b = expand_channel(v, bmask)
            if amask:
                a = expand_channel(v, amask)
            else:
                a = 0 if v == 0 else 255         # ASSUMED colour key
            rgba[p * 4:p * 4 + 4] = bytes((r, g, b, a))

    # 8-bit grayscale (Rec.601 luma) + the decoded alpha, as a separate plane.
    gray = bytearray(width * height * 2)
    for p in range(width * height):
        r, g, b, a = rgba[p * 4:p * 4 + 4]
        gray[p * 2] = (299 * r + 587 * g + 114 * b) // 1000
        gray[p * 2 + 1] = a

    return MaskInfo(tag, width, height, depth, consumed, len(blob), trailer,
                    bytes(rgba), bytes(gray), palette)


# --------------------------------------------------------------------------- #
# TEXTURE blobs
# --------------------------------------------------------------------------- #

class TexInfo:
    def __init__(self, kind, width, height, consumed, blob_size, fmt, rgba,
                 pixels_offset=0, pixels_bytes=0, chunks=""):
        self.kind, self.width, self.height = kind, width, height
        self.consumed, self.blob_size = consumed, blob_size
        self.fmt, self.rgba = fmt, rgba
        self.pixels_offset, self.pixels_bytes = pixels_offset, pixels_bytes
        self.chunks = chunks

    def __str__(self):
        return (f"{self.kind} {self.width}x{self.height} {self.fmt} "
                f"consumed={self.consumed}/{self.blob_size} bytes")


def decode_texture(blob: bytes, global_palette=None):
    """Decode a TEXTURE blob.  Two on-disk variants exist.

    (a) `LOFF` chunked true-colour texture (all MENV/*.res, COMMON.RES, 12057
        blobs).  Small header, base mip level is w*h LE u16 pixels, pixel format
        read from the trailing PFRM chunk:

            +0x00 u16 12        +0x02 u16 2
            +0x04 u16 0         +0x06 u16 0x1000
            +0x08 u32 0
            +0x0c u16 w         +0x0e u16 h
            +0x10 u16 0x2010    (8bpp would be 0x2008)
            +0x12 "LOFF"        +0x16 u32 4
            +0x1a u32 offset of the next chunk  (= 30 + 2*w*h)
            +0x1e w*h * u16 pixels (base level)
                  "PFRM" u32 len, len bytes of channel masks, possibly
                  "LVLS"/"LVMP" mip chunks, then "ENDR" u32 0

    (b) paletted 8bpp bitmap (the 172 LOAD.RES entries): 18-byte header,
        256 *BGR* triples, then w*h raw indices, index 0 = transparent.
    """
    if len(blob) < 8:
        raise ValueError("texture blob too small")

    # ---- (a) LOFF ----------------------------------------------------------
    if struct.unpack_from("<HH", blob, 0) == TXR_MAGIC and blob[18:22] == b"LOFF":
        width, height = struct.unpack_from("<HH", blob, 12)
        if struct.unpack_from("<H", blob, 16)[0] != 0x2010:
            raise ValueError("expected 16bpp LOFF texture")
        next_off = struct.unpack_from("<I", blob, 26)[0]
        if next_off != 30 + 2 * width * height:
            raise ValueError("LOFF size/next-offset mismatch")
        pix = blob[30:30 + 2 * width * height]
        pfrm = blob.find(TRAILER_PFRM, next_off)
        if pfrm < 0:
            raise ValueError("no PFRM chunk")
        plen = struct.unpack_from("<I", blob, pfrm + 4)[0]
        masks = list(struct.unpack_from("<%dI" % (plen // 4), blob, pfrm + 8))
        rmask = masks[0] if len(masks) > 0 else 0xF800
        gmask = masks[1] if len(masks) > 1 else 0x07E0
        bmask = masks[2] if len(masks) > 2 else 0x001F
        amask = masks[3] if len(masks) > 3 else 0
        fmt = (f"{bin(rmask).count('1')}:{bin(gmask).count('1')}:"
               f"{bin(bmask).count('1')}" +
               (f":{bin(amask).count('1')}" if amask else " (opaque)"))
        rgba = bytearray(width * height * 4)
        for p in range(width * height):
            v = struct.unpack_from("<H", pix, p * 2)[0]
            rgba[p * 4:p * 4 + 4] = bytes((
                expand_channel(v, rmask), expand_channel(v, gmask),
                expand_channel(v, bmask),
                expand_channel(v, amask) if amask else 255))
        # Byte accounting: base mip level (offset 30) plus the chunk trailer.
        # Chunks after the base level: "PFRM" (pixel format), optional "LVMP"
        # (reduced mip levels: u32 len, payload = u32 levels + u32 w + u32 h +
        #  u32 bytes/px + packed mip pixels; every one of the 281 LVMP chunks in
        #  the shipped data is followed by 2 zero bytes that the length field
        #  does not cover), then "ENDR".  The base level is unaffected either way.
        known = (TRAILER_PFRM, b"LVLS", b"LVMP", TRAILER_ENDR)
        tags = []
        off = next_off
        guard = 0
        while off < len(blob) and guard < 64:
            guard += 1
            tag = blob[off:off + 4]
            tags.append(tag.decode("latin1"))
            if tag == TRAILER_ENDR:
                off += 8
                break
            if tag in known:
                off += 8 + struct.unpack_from("<I", blob, off + 4)[0]
                while off < len(blob) and blob[off:off + 4] not in known:
                    off += 1                      # LVMP trailing pad bytes
            else:                                  # tolerate trailing padding
                break
        consumed = len(blob) if tags and tags[-1] == "ENDR" else 30 + 2 * width * height
        return TexInfo("txr/LOFF", width, height, consumed, len(blob), fmt,
                       bytes(rgba), 30, 2 * width * height, "/".join(tags))

    # ---- (b) paletted 8bpp --------------------------------------------------
    if struct.unpack_from("<HH", blob, 0) == RAWTEX_MAGIC:
        width, height = struct.unpack_from("<HH", blob, 12)
        if struct.unpack_from("<H", blob, 16)[0] != 0x2008:
            raise ValueError("expected 8bpp paletted texture")
        pal = blob[18:18 + PALETTE_BYTES]
        idx = blob[18 + PALETTE_BYTES:18 + PALETTE_BYTES + width * height]
        if len(idx) != width * height:
            raise ValueError("paletted texture truncated")
        rgba = bytearray(width * height * 4)
        for p, v in enumerate(idx):
            b, g, r = pal[v * 3:v * 3 + 3]         # stored B,G,R
            rgba[p * 4:p * 4 + 4] = bytes((r, g, b, 0 if v == 0 else 255))
        return TexInfo("txr/8bpp", width, height,
                       18 + PALETTE_BYTES + width * height, len(blob),
                       "8bpp indexed, BGR palette, key=0", bytes(rgba),
                       18 + PALETTE_BYTES, width * height, "raw")

    raise ValueError(f"unrecognised texture header {blob[:20].hex(' ')}")


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

def _safe(name: str) -> str:
    return name.replace("\\", "_").replace("/", "_").replace(" ", "_")


def _select(entries, name):
    if name is None:
        return list(entries)
    hits = [e for e in entries if e.base == name or e.name == name]
    if not hits:
        raise SystemExit(f"no entry named {name!r}")
    return hits


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Decode .msk / .txr blobs inside a Hard Truck 2 .RES file")
    ap.add_argument("res", help="path to a .res container")
    ap.add_argument("--list", action="store_true", help="list entries")
    ap.add_argument("--mask", metavar="NAME", action="append",
                    help="decode this mask (repeatable)")
    ap.add_argument("--txr", metavar="NAME", action="append",
                    help="decode this texture (repeatable)")
    ap.add_argument("--all", action="store_true",
                    help="decode every mask and texture")
    ap.add_argument("--out", metavar="DIR", default="tools/out",
                    help="PNG output directory (default tools/out)")
    ap.add_argument("--verify", action="store_true",
                    help="only check the byte-consumption budget")
    args = ap.parse_args(argv)

    data, sections, order = parse_res(args.res)
    masks = sections.get("MASKFILES", [])
    texs = sections.get("TEXTUREFILES", [])

    print(f"{args.res}: {len(masks)} masks, {len(texs)} textures")
    if args.list:
        for section, count in order:
            print(f"  {section} {count}")
        for e in masks + texs:
            print(f"    {e.section:<14} {e.name!r} size={e.size} @ {e.offset}")
        return 0

    want_masks = []
    for nm in (args.mask or []):
        want_masks += _select(masks, nm)
    want_texs = []
    for nm in (args.txr or []):
        want_texs += _select(texs, nm)
    if args.all:
        want_masks, want_texs = list(masks), list(texs)
    if not want_masks and not want_texs:
        print("nothing to do: pass --mask NAME, --txr NAME or --all")
        return 0

    os.makedirs(args.out, exist_ok=True)
    total = ok = 0

    for e in want_masks:
        blob = data[e.offset:e.offset + e.size]
        total += 1
        try:
            info = decode_mask(blob)
        except Exception as exc:                                # noqa: BLE001
            print(f"  !! {e.name}: {exc}")
            continue
        expect = e.size - MASK_HEADER_BYTES - (44 if info.depth == 2 else 0)
        good = info.consumed == expect
        ok += good
        print(f"  {e.name}: {info.width}x{info.height} depth={info.depth} "
              f"consumed={info.consumed} of {expect} RLE bytes "
              f"[{'EXACT' if good else 'MISMATCH'}]"
              + (f" trailer={info.trailer}" if info.trailer else ""))
        if args.verify or not good:
            continue
        stem = os.path.join(args.out, _safe(e.base))
        write_png(stem + ".png", info.width, info.height, info.rgba)
        write_png(stem + ".gray.png", info.width, info.height,
                  _gray_rgba(info.gray_alpha, info.width, info.height))

    for e in want_texs:
        blob = data[e.offset:e.offset + e.size]
        total += 1
        try:
            info = decode_texture(blob)
        except Exception as exc:                                # noqa: BLE001
            print(f"  !! {e.name}: {exc}")
            continue
        good = info.consumed == info.blob_size
        ok += good
        print(f"  {e.name}: {info.width}x{info.height} {info.fmt} "
              f"consumed={info.consumed}/{info.blob_size} "
              f"[{'EXACT' if good else 'MISMATCH'}]")
        if args.verify or not good:
            continue
        write_png(os.path.join(args.out, _safe(e.base) + ".png"),
                  info.width, info.height, info.rgba)

    print(f"{ok}/{total} blobs consumed exactly")
    return 0 if ok == total else 1


def _gray_rgba(gray: bytes, width: int, height: int) -> bytes:
    """Turn the 2-byte (luma, alpha) buffer into RGBA for PNG output."""
    out = bytearray(width * height * 4)
    for p in range(width * height):
        v, a = gray[p * 2], gray[p * 2 + 1]
        out[p * 4:p * 4 + 4] = bytes((v, v, v, a))
    return bytes(out)


if __name__ == "__main__":
    sys.exit(main())

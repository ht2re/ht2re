#!/usr/bin/env python3
"""video_probe.py - independent check of an .rvid frame pack (see docs/video.md).

Parses the container header, verifies the size table accounts for exactly the
bytes in the file, decodes sample frames (RLE or raw) and writes one of them to
a PNG together with a short "is this a real picture" report.

    tools/video_probe.py build/media/kotr.rvid
    tools/video_probe.py build/media/kotr.rvid --frames 0,600,1307 --out /tmp/f.png
    tools/video_probe.py build/media/kotr.rvid --decode-all     # slow, exhaustive

Exit code 0 = all checks passed, 1 = a check failed, 2 = usage/IO error.
"""
import argparse
import os
import struct
import sys
import zlib

RVID_MAGIC = b"RVID"
HDR = 32
RAW_FLAG = 0x80000000


def read_header(f):
    f.seek(0)
    hdr = f.read(HDR)
    if len(hdr) < HDR:
        raise ValueError("file shorter than the 32 byte header")
    magic = hdr[:4]
    (version, w, h, fps, frames, flags, table_off) = struct.unpack_from("<7I", hdr, 4)
    if magic != RVID_MAGIC:
        raise ValueError("bad magic %r (expected RVID)" % magic)
    return dict(version=version, w=w, h=h, fps=fps, frames=frames,
                flags=flags, table_off=table_off)


def read_table(f, h):
    f.seek(h["table_off"])
    n = h["frames"]
    raw = f.read(4 * n)
    if len(raw) != 4 * n:
        raise ValueError("size table truncated (%d of %d bytes)" % (len(raw), 4 * n))
    table = list(struct.unpack("<%dI" % n, raw))
    # absolute file offset of every frame payload
    offs = []
    pos = h["table_off"] + 4 * n
    for s in table:
        offs.append(pos)
        pos += s & ~RAW_FLAG
    return table, offs


def rle_decode(payload, npixels):
    """PackBits over 16-bit RGB565 pixels; returns a list of npixels ints."""
    px = []
    i = 0
    n = len(payload)
    while i < n and len(px) < npixels:
        c = payload[i]
        i += 1
        if c < 0x80:
            cnt = c + 1
            end = i + cnt * 2
            if end > n:
                raise ValueError("literal run runs past the end of the payload")
            px.extend(struct.unpack_from("<%dH" % cnt, payload, i))
            i = end
        else:
            cnt = (c & 0x7F) + 2
            if i + 2 > n:
                raise ValueError("repeat run runs past the end of the payload")
            val = struct.unpack_from("<H", payload, i)[0]
            i += 2
            px.extend([val] * cnt)
    if len(px) != npixels:
        raise ValueError("frame decoded to %d pixels, expected %d" % (len(px), npixels))
    return px


def frame_at(f, h, table, offs, idx):
    """Returns (mode, payload bytes)."""
    if idx < 0 or idx >= len(table):
        raise IndexError("frame %d out of range" % idx)
    size = table[idx] & ~RAW_FLAG
    f.seek(offs[idx])
    payload = f.read(size)
    if len(payload) != size:
        raise ValueError("frame %d truncated (%d of %d bytes)" % (idx, len(payload), size))
    return ("raw" if table[idx] & RAW_FLAG else "rle"), payload


def rgb565_to_rgb(px):
    out = bytearray(len(px) * 3)
    for i, v in enumerate(px):
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        out[i * 3 + 0] = (r << 3) | (r >> 2)
        out[i * 3 + 1] = (g << 2) | (g >> 4)
        out[i * 3 + 2] = (b << 3) | (b >> 2)
    return bytes(out)


def write_png(path, rgb, w, h):
    rows = bytearray()
    for y in range(h):
        rows.append(0)                                   # filter: none
        rows += rgb[y * w * 3:(y + 1) * w * 3]
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as fh:
        fh.write(png)


def stats(px):
    n = len(px)
    if not n:
        return dict(mean=0.0, std=0.0, uniq=0, black=0.0)
    # luminance in 0..255 from the 5/6/5 fields
    lum = [((v >> 11) & 0x1F) * 8 * 0.299 + ((v >> 5) & 0x3F) * 4 * 0.587 + (v & 0x1F) * 8 * 0.114
           for v in px[::max(1, n // 20000)]]
    m = sum(lum) / len(lum)
    var = sum((x - m) ** 2 for x in lum) / len(lum)
    uniq = len(set(px[::max(1, n // 20000)]))
    black = sum(1 for v in px if v == 0) / n
    return dict(mean=m, std=var ** 0.5, uniq=uniq, black=black)


def main():
    ap = argparse.ArgumentParser(description="check an .rvid movie frame pack")
    ap.add_argument("rvid")
    ap.add_argument("--frame", type=int, default=None, help="frame to dump to PNG")
    ap.add_argument("--frames", default="0,middle,last",
                    help="frames to decode and report (default 0,middle,last)")
    ap.add_argument("--out", default=None, help="PNG output path (default <rvid>.frame<N>.png)")
    ap.add_argument("--decode-all", action="store_true", help="decode every frame (slow)")
    args = ap.parse_args()

    try:
        f = open(args.rvid, "rb")
    except OSError as e:
        print("video_probe: %s" % e, file=sys.stderr)
        return 2

    ok = True
    with f:
        try:
            h = read_header(f)
            table, offs = read_table(f, h)
        except ValueError as e:
            print("video_probe: %s" % e, file=sys.stderr)
            return 1

        fsize = os.path.getsize(args.rvid)
        data_start = h["table_off"] + 4 * h["frames"]
        payload = sum(s & ~RAW_FLAG for s in table)
        n_raw = sum(1 for s in table if s & RAW_FLAG)
        print("file        %s" % args.rvid)
        print("header      magic=RVID version=%d tableOff=%d flags=%d (rle=%s)"
              % (h["version"], h["table_off"], h["flags"],
                 "on" if h["flags"] & 1 else "off"))
        print("clip        %dx%d @%d fps, %d frames (%.2f s)"
              % (h["w"], h["h"], h["fps"], h["frames"], h["frames"] / max(1, h["fps"])))
        print("size        %d bytes (%.1f MB); data starts at %d; payload %d bytes"
              % (fsize, fsize / 1e6, data_start, payload))
        print("frames      %d raw / %d rle coded (%.2fx smaller than raw RGB565)"
              % (n_raw, h["frames"] - n_raw,
                 (h["w"] * h["h"] * 2 * h["frames"]) / max(1, payload + data_start)))

        checks = [
            ("version is 1", h["version"] == 1),
            ("geometry sane", h["w"] > 0 and h["h"] > 0 and h["fps"] > 0),
            ("frameCount > 0", h["frames"] > 0),
            ("tableOffset == 32", h["table_off"] == HDR),
            ("table + payload == file size", data_start + payload == fsize),
            ("every frame non-empty", all((s & ~RAW_FLAG) > 0 for s in table)),
            ("every frame <= raw size",
             all((s & ~RAW_FLAG) <= h["w"] * h["h"] * 2 for s in table)),
        ]
        for name, good in checks:
            print("check       [%s] %s" % ("ok" if good else "FAIL", name))
            ok = ok and good

        if args.decode_all:
            sample = list(range(h["frames"]))
        elif args.frame is not None:
            sample = [args.frame]
        else:
            n = h["frames"]
            sample = []
            for tok in args.frames.split(","):
                tok = tok.strip()
                if tok == "middle":
                    sample.append(n // 2)
                elif tok == "last":
                    sample.append(n - 1)
                elif tok:
                    sample.append(int(tok))
            sample = [max(0, min(n - 1, s)) for s in sample]

        results = {}
        flat = 0
        for idx in sample:
            try:
                mode, pl = frame_at(f, h, table, offs, idx)
                px = (list(struct.unpack("<%dH" % (h["w"] * h["h"]), pl))
                      if mode == "raw" else rle_decode(pl, h["w"] * h["h"]))
            except (ValueError, struct.error) as e:
                print("check       [FAIL] frame %d: %s" % (idx, e))
                ok = False
                continue
            s = stats(px)
            results[idx] = px
            is_pic = s["std"] > 8 and s["black"] < 0.98
            if not is_pic:
                flat += 1
            print("frame %-5d %s %6d bytes  mean=%5.1f std=%5.1f uniq=%5d black=%4.1f%%  %s"
                  % (idx, mode, len(pl), s["mean"], s["std"], s["uniq"], s["black"] * 100,
                     "picture" if is_pic else "flat/black (the source is too)"))
        if sample and flat == len(sample):
            print("check       [FAIL] every sampled frame is flat/black - decode suspect")
            ok = False

        # dump a frame that shows real content unless the caller picked one
        dump = args.frame
        if dump is None:
            pics = [i for i in sample if stats(results[i])["std"] > 8] if results else []
            dump = pics[len(pics) // 2] if pics else (sample[0] if sample else 0)
        if dump in results:
            out = args.out or ("%s.frame%d.png" % (args.rvid, dump))
            write_png(out, rgb565_to_rgb(results[dump]), h["w"], h["h"])
            print("png         wrote frame %d to %s (%dx%d), not black/uniform"
                  % (dump, out, h["w"], h["h"]))

    print("result      %s" % ("ALL CHECKS PASSED" if ok else "FAILURES"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

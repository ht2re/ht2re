#!/usr/bin/env python3
"""
HT2:RE - inventory of the shipped sound effects.

Every `SOUNDFILES` entry of `original/COMMON/common1.res` (and, as a cross
check, `common2.res` / `COMMON.RES`) is a complete RIFF/WAVE file whose first
four bytes are the `RIFF` tag.  This tool parses the RIFF chunks straight out of
the container - no extraction to disk - and prints:

  * one row per effect: container size, RIFF size, encoding, channels, sample
    rate, bits/sample, `data` chunk bytes and duration;
  * the `SOUNDS` name -> 1-based SOUNDFILES index mapping (the game's logical
    sound names, e.g. `idleSound` -> `snd\\idle.wav`);
  * a summary: effect count, distinct formats, total bytes/duration, the
    cross-file agreement check, and the loop/one-shot candidates.

Run:  python3 tools/audio_probe.py [--res FILE] [--map] [--longest N]
"""

from __future__ import annotations

import argparse
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from res_msk import parse_res  # noqa: E402

DEFAULT_RES = "original/COMMON/common1.res"
CROSS_FILES = ["original/COMMON/common2.res", "original/COMMON/COMMON.RES"]

# audioFormat (wFormatTag) values seen in RIFF `fmt ` chunks
ENCODINGS = {
    0x0001: "PCM",
    0x0002: "MS ADPCM",
    0x0011: "IMA ADPCM",
    0x0055: "MP3",
    0x2000: "AC-3",
    0xFFFE: "extensible",
}


def parse_wav(blob: bytes):
    """Parse a RIFF/WAVE blob.  Returns a dict, or raises ValueError."""
    if len(blob) < 12 or blob[:4] != b"RIFF" or blob[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")
    riff_size = struct.unpack_from("<I", blob, 4)[0]
    info = {
        "riff_size": riff_size,
        "declared": riff_size + 8,       # bytes a WAV player reads
        "fmt": None, "channels": None, "rate": None, "bits": None,
        "byte_rate": None, "block_align": None, "data": 0, "chunks": [],
    }
    pos = 12
    n = len(blob)
    while pos + 8 <= n:
        cid = blob[pos:pos + 4]
        csz = struct.unpack_from("<I", blob, pos + 4)[0]
        body = pos + 8
        info["chunks"].append((cid.decode("latin1"), csz))
        if cid == b"fmt " and csz >= 16 and body + 16 <= n:
            (info["fmt"], info["channels"], info["rate"], info["byte_rate"],
             info["block_align"], info["bits"]) = struct.unpack_from("<HHIIHH", blob, body)
        elif cid == b"data":
            info["data"] = min(csz, max(0, n - body))
        pos = body + csz + (csz & 1)          # chunks are word aligned
    if info["rate"] is None:
        raise ValueError("no fmt chunk")
    return info


def duration(info) -> float:
    if not info["byte_rate"]:
        return 0.0
    return info["data"] / float(info["byte_rate"])


def sound_map(data: bytes):
    """`SOUNDS <n>` is a name -> 1-based SOUNDFILES index table, not a payload."""
    key = b"SOUNDS "
    at = data.find(key)
    if at < 0:
        return []
    end = data.index(b"\0", at)
    count = int(data[at + len(key):end])
    pos = end + 1
    out = []
    for _ in range(count):
        z = data.index(b"\0", pos)
        line = data[pos:z].decode("cp1251", "replace")
        pos = z + 1
        nm, _, idx = line.rpartition(" ")
        out.append((nm, int(idx)))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description="Inventory the HT2 sound effects")
    ap.add_argument("--res", default=DEFAULT_RES, help="container to read")
    ap.add_argument("--map", action="store_true", help="print the SOUNDS name map")
    ap.add_argument("--longest", type=int, default=12,
                    help="how many longest effects to list (0 = none)")
    args = ap.parse_args(argv)

    data, sections, _ = parse_res(args.res)
    entries = sections.get("SOUNDFILES", [])
    if not entries:
        print(f"{args.res}: no SOUNDFILES")
        return 1

    print(f"== {args.res}: {len(entries)} SOUNDFILES ==")
    hdr = (f"{'#':>3} {'name':<20} {'res B':>8} {'riff B':>8} {'enc':<9} "
           f"{'ch':>2} {'rate':>6} {'bits':>4} {'data B':>8} {'sec':>7}")
    print(hdr)
    print("-" * len(hdr))

    infos = []
    bad = 0
    for i, e in enumerate(entries, 1):
        blob = data[e.offset:e.offset + e.size]
        try:
            w = parse_wav(blob)
        except ValueError as exc:
            print(f"{i:>3} {e.name:<20} {e.size:>8}  !! {exc}")
            bad += 1
            continue
        infos.append((i, e, w))
        print(f"{i:>3} {e.name:<20} {e.size:>8} {w['declared']:>8} "
              f"{ENCODINGS.get(w['fmt'], hex(w['fmt'])):<9} {w['channels']:>2} "
              f"{w['rate']:>6} {w['bits']:>4} {w['data']:>8} {duration(w):>7.2f}")

    # ---- summary ----------------------------------------------------------
    print()
    print("== summary ==")
    fmt_hist = collections.Counter(
        (ENCODINGS.get(w["fmt"], hex(w["fmt"])), w["channels"], w["rate"], w["bits"])
        for _, _, w in infos)
    total_res = sum(e.size for _, e, _ in infos)
    total_data = sum(w["data"] for _, _, w in infos)
    total_sec = sum(duration(w) for _, _, w in infos)
    print(f"effects parsed     : {len(infos)}/{len(entries)}  ({bad} unreadable)")
    print(f"container bytes    : {total_res:,} B ({total_res/1048576:.2f} MiB)"
          f"  - 'size' fields include the 4-byte RIFF tag")
    print(f"PCM data bytes     : {total_data:,} B")
    print(f"total playing time : {total_sec:.1f} s")
    print("formats found      :")
    for (enc, ch, rate, bits), n in sorted(fmt_hist.items(), key=lambda kv: -kv[1]):
        print(f"    {n:>3} x {enc} {ch}ch {rate} Hz {bits}-bit")
    rates = {w["rate"] for _, _, w in infos}
    print(f"distinct rates     : {sorted(rates)}"
          f"  (target device: 44100 Hz 16-bit stereo)")
    print(f"truncated RIFFs    : "
          f"{sum(1 for _, e, w in infos if w['declared'] > e.size)}"
          f" of {len(infos)} (declared length > container payload)")
    odd = [e.name for _, e, _ in infos if w['declared'] % 2]
    if odd:
        print(f"odd RIFF lengths   : {len(odd)} e.g. {odd[:4]}")

    if args.longest:
        print(f"longest {args.longest} (loop/ambience candidates):")
        for _, e, w in sorted(infos, key=lambda t: -duration(t[2]))[:args.longest]:
            print(f"    {e.name:<20} {duration(w):>7.2f} s  {w['data']:>8} B")

    smap = sound_map(data)
    if smap:
        print(f"SOUNDS name map    : {len(smap)} logical names")
    if args.map:
        print()
        print("== SOUNDS map (logical name -> 1-based SOUNDFILES index) ==")
        by_idx = {i: e.name for i, e, _ in infos}
        for nm, idx in sorted(smap, key=lambda t: t[1]):
            print(f"  {idx:>3}  {nm:<22} {by_idx.get(idx, '?')}")

    # ---- cross check ------------------------------------------------------
    if args.res == DEFAULT_RES:
        names = [e.name for e in entries]
        for other in CROSS_FILES:
            if not os.path.exists(other):
                continue
            odata, osections, _ = parse_res(other)
            onames = [e.name for e in osections.get("SOUNDFILES", [])]
            same = onames == names
            print(f"cross check {other:<34} {len(onames)} entries, "
                  f"{'identical set' if same else 'DIFFERENT'}")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

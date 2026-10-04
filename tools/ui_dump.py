#!/usr/bin/env python3
"""HT2:RE - dump the on-screen interface art (HUD / loading screen / menus).

Research helper for docs/original_ui.md.  Reads a `.res` container and writes
PNGs for every mask/texture whose (lower-cased) name matches one of the given
substrings, printing size / depth / consumed-byte info as it goes.

Usage:
  python3 tools/ui_dump.py original/COMMON/esc16.RES --dir tools/out/ui --match tablo pager mess \\
      m_panel vtrpanel big0 colon scr mirr icon I_ up_mess
  python3 tools/ui_dump.py original/COMMON/ind16.RES --match load
  python3 tools/ui_dump.py original/COMMON/esc16.RES --list           # just list names
"""
from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import res_msk as R  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("res")
    ap.add_argument("--dir", default="tools/out/ui")
    ap.add_argument("--match", nargs="*", default=None,
                    help="case-insensitive substrings; empty = everything")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--scale", type=int, default=1, help="nearest-neighbour upscale")
    args = ap.parse_args()

    data, sections, order = R.parse_res(args.res)
    try:
        pal_off = R.res_palette(data, sections)
    except Exception:
        pal_off = None
    gp = data[pal_off:pal_off + 768] if pal_off is not None else None

    print(f"{args.res}: sections={[s for s, _ in order]}")
    os.makedirs(args.dir, exist_ok=True)
    pats = [p.lower() for p in (args.match or [])]

    def keep(name: str) -> bool:
        low = name.lower()
        return (not pats) or any(p in low for p in pats)

    total = 0
    for sec in ("MASKFILES", "TEXTUREFILES", "BACKFILES", "PALETTEFILES"):
        for e in sections.get(sec, []):
            if not keep(e.name):
                continue
            blob = data[e.offset:e.offset + e.size]
            stem = e.name.replace("\\", "_").replace("/", "_")
            total += 1
            try:
                if sec == "MASKFILES" or blob[:4] in R.MASK_TAGS:
                    info = R.decode_mask(blob)
                    kind = "MSK"
                else:
                    info = R.decode_texture(blob, gp)
                    kind = "TXR"
            except Exception as exc:                                # noqa: BLE001
                print(f"  ! {e.name:36s} {sec:12s} size={e.size:9d} DECODE FAIL: {exc}")
                continue
            note = ""
            if kind == "MSK":
                rgb = info.palette[3:6] if info.palette else b""
                note = f"tag={info.tag.decode()} pal[1]=({rgb[0]},{rgb[1]},{rgb[2]}) trailer={info.trailer}"
            else:
                note = f"fmt={info.fmt} chunks={info.chunks}"
            if args.list:
                print(f"  {kind} {e.name:36s} {info.width:4d}x{info.height:<4d} size={e.size:8d} {note}")
                continue
            w, h = info.width, info.height
            rgba = info.rgba
            if args.scale > 1:
                s = args.scale
                up = bytearray(w * s * h * s * 4)
                for y in range(h):
                    row = rgba[y * w * 4:(y + 1) * w * 4]
                    big = bytearray()
                    for x in range(w):
                        big += row[x * 4:x * 4 + 4] * s
                    for k in range(s):
                        up[(y * s + k) * w * s * 4:(y * s + k + 1) * w * s * 4] = big
                rgba, w, h = bytes(up), w * s, h * s
            out = os.path.join(args.dir, f"{stem}.png")
            R.write_png(out, w, h, rgba)
            print(f"  {kind} {e.name:36s} {info.width:4d}x{info.height:<4d} size={e.size:8d} -> {out}  {note}")
    print(f"{total} entries matched")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

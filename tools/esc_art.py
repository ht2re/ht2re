#!/usr/bin/env python3
"""HT2:RE - art-size lookup across every original .res (handles the `" noload"` suffix).

Usage: python3 tools/esc_art.py "m\\back_main.txr" "m_16\\savg_a.txr" ...
"""
from __future__ import annotations

import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import res_msk as R  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_CACHE: dict[str, tuple] | None = None


def _norm(name: str) -> str:
    return name.split(" noload")[0].strip().lower().replace("/", "\\")


def index() -> dict[str, tuple]:
    global _CACHE
    if _CACHE is not None:
        return _CACHE
    out: dict[str, tuple] = {}
    files = sorted(glob.glob(os.path.join(ROOT, "original", "**", "*.res"), recursive=True)
                   + glob.glob(os.path.join(ROOT, "original", "**", "*.RES"), recursive=True))
    for rp in files:
        try:
            d, sec, _ = R.parse_res(rp)
        except Exception:                                        # noqa: BLE001
            continue
        for s in ("MASKFILES", "TEXTUREFILES", "BACKFILES"):
            for e in sec.get(s, []):
                key = _norm(e.name)
                if key in out:
                    continue
                blob = d[e.offset:e.offset + e.size]
                try:
                    if s == "MASKFILES" or blob[:4] in R.MASK_TAGS:
                        i = R.decode_mask(blob)
                    else:
                        i = R.decode_texture(blob, None)
                    out[key] = (os.path.basename(rp), i.width, i.height, e.size)
                except Exception:                                # noqa: BLE001
                    out[key] = (os.path.basename(rp), -1, -1, e.size)
    _CACHE = out
    return out


def size(name: str):
    return index().get(_norm(name))


if __name__ == "__main__":
    for a in sys.argv[1:]:
        print(f"{a:28s} {size(a)}")

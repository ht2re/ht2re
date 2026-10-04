#!/usr/bin/env python3
"""Build a side-by-side PNG: the retail reference frame vs this port's frame.

usage: python3 tools/ref_compare.py REF PORT OUT [--crop x0,y0,x1,y1]

The reference frames in shots/ref/ are JPEG/WebP with a .png name, so they are
opened by content (Pillow), not by extension.  Both sides are scaled to the same
height and labelled in the file name.
"""
import os
import sys

from PIL import Image


def main() -> int:
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    ref, port, out = sys.argv[1], sys.argv[2], sys.argv[3]
    crop = None
    for i, a in enumerate(sys.argv):
        if a == "--crop" and i + 1 < len(sys.argv):
            crop = tuple(int(v) for v in sys.argv[i + 1].split(","))
    a = Image.open(ref).convert("RGB")
    b = Image.open(port).convert("RGB")
    if crop:
        a = a.crop(crop)
        b = b.crop(crop)
    h = max(a.size[1], b.size[1])
    a = a.resize((int(a.size[0] * h / a.size[1]), h), Image.LANCZOS)
    b = b.resize((int(b.size[0] * h / b.size[1]), h), Image.LANCZOS)
    gap = 8
    canvas = Image.new("RGB", (a.size[0] + b.size[0] + gap, h), (16, 16, 16))
    canvas.paste(a, (0, 0))
    canvas.paste(b, (a.size[0] + gap, 0))
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    canvas.save(out)
    print(f"{out}: reference {a.size[0]}x{h} | port {b.size[0]}x{h}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

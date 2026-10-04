#!/usr/bin/env python3
"""HT2:RE rear-view-mirror checks (docs/mirror.md).

Reads the four BMPs written by build/mirror_check (tools/mirror_check.sh) plus
its log lines, and asserts that the mirror really is a re-rendered 3D view in
the frame's glass hole, clipped to the frame rectangle, horizontally flipped,
and that the reflection tracks the truck.

    python3 tools/mirror_check.py shots/mirror.log
"""
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "shots", "mirror.log")
OUT = os.path.dirname(os.path.abspath(LOG))

failures = []
total = [0]


def check(name, cond, detail=""):
    total[0] += 1
    print(("  ok   " if cond else "  FAIL ") + name + ((" -- " + detail) if detail else ""))
    if not cond:
        failures.append(name)


def read_bmp(path):
    """(w, h, rows) with rows[0] = TOP row, pixels as (R,G,B)."""
    with open(path, "rb") as fh:
        data = fh.read()
    assert data[:2] == b"BM", path + ": not a BMP"
    off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    assert bpp == 24, f"{path}: bpp {bpp}"
    flip = h > 0
    h = abs(h)
    stride = (w * 3 + 3) & ~3
    rows = []
    for y in range(h):
        src = off + (h - 1 - y if flip else y) * stride
        raw = data[src:src + w * 3]
        rows.append([(raw[i * 3 + 2], raw[i * 3 + 1], raw[i * 3 + 0]) for i in range(w)])
    return w, h, rows


def region_diff(a, b, x0, y0, x1, y1, step=1):
    """% of px inside the rect that differ (any channel)."""
    n = tot = 0
    for y in range(y0, y1, step):
        for x in range(x0, x1, step):
            tot += 1
            if a[2][y][x] != b[2][y][x]:
                n += 1
    return (100.0 * n / tot) if tot else 0.0, n, tot


def main():
    log = open(LOG, encoding="utf-8", errors="replace").read()
    m = re.search(r"MIRROR loaded=(\d+) anchor=(.*?) frame=(\d+),(\d+),(\d+),(\d+) "
                  r"glass=(\d+),(\d+),(\d+),(\d+) target=(\d+)x(\d+) flip=(\d+)", log)
    if not m:
        print("FAIL: no MIRROR line in", LOG)
        return 1
    loaded = int(m.group(1))
    anchor = m.group(2)
    fx, fy, fw, fh = (int(m.group(i)) for i in (3, 4, 5, 6))
    gx, gy, gw, gh = (int(m.group(i)) for i in (7, 8, 9, 10))
    tw, th = int(m.group(11)), int(m.group(12))
    flip = int(m.group(13))
    cost = re.search(r"COST frames=(\d+) nomirror_ms=([\d.]+) mirror_ms=([\d.]+) "
                     r"delta_ms=([\d.]+) reflection_pass_ms=([\d.]+)", log)
    print(f"mirror: anchor={anchor} frame={fx},{fy},{fw},{fh} glass={gx},{gy},{gw},{gh} "
          f"target={tw}x{th} flip={flip}")
    if cost:
        print(f"cost:   {cost.group(2)} ms -> {cost.group(3)} ms "
              f"(+{cost.group(4)} ms, reflection pass {cost.group(5)} ms)")

    check("mirror frames loaded from esc16.RES", loaded == 1, f"loaded={loaded}")
    check("mirror camera mounted on the model's SLRSpace anchor",
          "SLRSpace" in anchor, anchor)
    check("mirror image is horizontally flipped", flip == 1, f"flip={flip}")
    check("reflection render target is smaller than the glass",
          0 < tw <= gw and 0 < th <= gh, f"target {tw}x{th} for glass {gw}x{gh}")

    no = read_bmp(os.path.join(OUT, "mirror_nomirror.bmp"))
    yes = read_bmp(os.path.join(OUT, "mirror_mirror.bmp"))
    noflip = read_bmp(os.path.join(OUT, "mirror_mirror_noflip.bmp"))
    moved = read_bmp(os.path.join(OUT, "mirror_mirror_moved.bmp"))

    # 1. the glass shows something else than the cockpit view behind it
    d, n, tot = region_diff(no, yes, gx, gy, gx + gw, gy + gh)
    check("mirror region differs from the un-mirrored frame", d > 30.0,
          f"{d:.1f}% of {tot} px ({n} px)")

    # 2. the reflection is a rendered world, not a flat fill: it must contain
    #    both sky and ground (the mirror camera sits low behind the truck)
    sky = ground = 0
    for y in range(gy, gy + gh):
        for x in range(gx, gx + gw):
            c = yes[2][y][x]
            if c[2] > 150 and c[2] > c[0] + 20:
                sky += 1
            else:
                ground += 1
    check("reflection shows sky and ground", sky > 0.05 * gw * gh and ground > 0.2 * gw * gh,
          f"{sky} sky px, {ground} ground px of {gw * gh}")

    # 3. the frame art is drawn over the border (outside the glass, inside the rect)
    border_diff = 0
    border_tot = 0
    for y in range(fy, fy + fh):
        if gy <= y < gy + gh:
            continue
        for x in range(fx, fx + fw):
            border_tot += 1
            if no[2][y][x] != yes[2][y][x]:
                border_diff += 1
    for x in range(fx, fx + fw):
        for y in range(gy, gy + gh):
            if gx <= x < gx + gw:
                continue
            border_tot += 1
            if no[2][y][x] != yes[2][y][x]:
                border_diff += 1
    check("mirror frame art drawn over the reflection",
          border_tot > 0 and border_diff > 0.5 * border_tot,
          f"{border_diff}/{border_tot} border px changed")

    # 4. clipped to the frame rectangle: nothing outside changes at all
    outside = 0
    for y in range(no[1]):
        for x in range(no[0]):
            if fx <= x < fx + fw and fy <= y < fy + fh:
                continue
            if no[2][y][x] != yes[2][y][x]:
                outside += 1
    check("mirror is clipped to its rectangle (0 px changed outside)",
          outside == 0, f"{outside} px outside the frame rect")

    # 5. the U flip really flips: flipped == horizontal mirror of unflipped
    bad = 0
    for y in range(gy, gy + gh):
        for i in range(gw):
            if yes[2][y][gx + i] != noflip[2][y][gx + gw - 1 - i]:
                bad += 1
    sym = 0
    for y in range(gy, gy + gh):
        for i in range(gw // 2):
            if yes[2][y][gx + i] != yes[2][y][gx + gw - 1 - i]:
                sym += 1
    check("reflection is the horizontal mirror of the unflipped render",
          bad == 0 and sym > 0, f"{bad} mismatched px, {sym} asymmetric px pairs")

    # 6. driving the truck changes what the mirror shows
    d2, n2, tot2 = region_diff(yes, moved, gx, gy, gx + gw, gy + gh)
    check("reflection follows the truck (content changes when it moves)",
          d2 > 20.0, f"{d2:.1f}% of {tot2} px ({n2} px)")

    print(f"mirror checks: {total[0] - len(failures)} ok, {len(failures)} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

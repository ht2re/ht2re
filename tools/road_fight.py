#!/usr/bin/env python3
"""Road *geometry* fighting probe ("дороги всё ещё дрожат").

The texture-sampling half of this bug is measured by tools/road_shimmer.py and
fixed with anisotropic filtering.  This tool measures the other half: does the
authored asphalt lose pixels to a *different* surface (the generated ground mesh,
or another tile triangle) when the camera moves by a fraction of a degree, and
does the ground sheet break through the asphalt at all?

Method
------
Every frame comes from the same engine with one flag changed, so no colour
classification is needed for the key metric:

  full       the whole scene (ground + tiles)
  noground   the same frame with --no-ground             -> "the ground changed here"
  roadonly   --only-mat mat_road --no-ground             -> the authored asphalt
  nothing    --only-mat <matches nothing> --no-ground    -> the background only

  asphalt    = |roadonly - nothing|             (the authored asphalt surface)
  ground_px  = |full - noground|                (pixels the ground mesh paints)
  covered(p) = asphalt pixels where `full` == `roadonly`  (the asphalt wins)

`flip` counts the asphalt pixels where coverage changes between two camera poses
a fraction of a degree apart - the direct z-fighting / popping number.  `buried`
counts asphalt the scene never draws at either pose, `ground_over` the asphalt
the ground mesh demonstrably covers.

usage: python3 tools/road_fight.py --tag before
       python3 tools/road_fight.py --tag after
"""
import argparse
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
try:
    import numpy as np
except ImportError:                                     # pragma: no cover
    sys.exit("numpy is required for the fighting metric")


def read_bmp(path):
    with open(path, "rb") as fh:
        data = fh.read()
    assert data[:2] == b"BM", "not a BMP: " + path
    off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    assert bpp == 24, f"unexpected bpp {bpp}"
    flip = h > 0
    h = abs(h)
    stride = (w * 3 + 3) & ~3
    raw = np.frombuffer(data, dtype=np.uint8, count=stride * h, offset=off)
    raw = raw.reshape(h, stride)[:, : w * 3].reshape(h, w, 3)
    out = np.empty((h, w, 3), dtype=np.int16)
    out[:, :, 0] = raw[:, :, 2]
    out[:, :, 1] = raw[:, :, 1]
    out[:, :, 2] = raw[:, :, 0]
    if flip:
        out = out[::-1]
    return out


def erode(mask, n):
    m = mask.copy()
    for _ in range(n):
        p = np.pad(m, 1, constant_values=False)
        m = (p[:-2, 1:-1] & p[2:, 1:-1] & p[1:-1, :-2] & p[1:-1, 2:] & p[1:-1, 1:-1])
    return m


def measure(bin_path=None, data=None, shots=None, tag="fight",
            camera="-1740.5,31,3960,80.5,-14", spawn="-2886,-4643,70",
            delta=0.03, erode_px=2, tol=6, ground="on", keep=False, verbose=True):
    """Run the fighting measurement and return a dict of the numbers.

    Every argument mirrors the CLI; the engine is launched with subprocess so the
    same measurement can be imported by tools/shot_test.py.
    """
    bin_path = bin_path or os.path.join(ROOT, "build", "ht2re")
    data = data or os.path.join(ROOT, "original")
    shots = shots or os.path.join(ROOT, "shots")
    os.makedirs(shots, exist_ok=True)
    base = ["--data", data, "--scene", "world", "--screenshot",
            "--spawn", spawn, "--traffic", "0"]
    if ground == "off":
        base.append("--no-ground")
    px, py, pz, yaw, pitch = (float(v) for v in camera.split(","))

    def shot(name, extra, dyaw=0.0):
        path = os.path.join(shots, f"_{tag}_{name}.bmp")
        cmd = [bin_path] + base + ["--camera", f"{px},{py},{pz},{yaw + dyaw},{pitch}",
                                   "--out", path] + extra
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"render failed ({r.returncode}): {' '.join(cmd)}\n{r.stdout[-2000:]}")
        return read_bmp(path)

    def anydiff(A, B):
        return np.any(np.abs(A - B) > tol, axis=2)

    roadonly = shot("roadonly", ["--only-mat", "mat_road", "--no-ground"])
    nothing = shot("nothing", ["--only-mat", "mat_zzznone", "--no-ground"])
    asphalt = erode(anydiff(roadonly, nothing), erode_px)
    n_road = int(asphalt.sum())
    if n_road < 500:
        sys.exit(f"asphalt mask is empty ({n_road} px) - wrong camera?")

    ctrl = int(anydiff(shot("a0", []), shot("a1", [])).sum())

    fullA = shot("fullA", [])
    roadA = shot("roadA", ["--only-mat", "mat_road", "--no-ground"])
    coveredA = ~anydiff(fullA, roadA) & asphalt
    groundA = np.zeros_like(asphalt)
    fullB = shot("fullB", [], delta)
    roadB = shot("roadB", ["--only-mat", "mat_road", "--no-ground"], delta)
    coveredB = ~anydiff(fullB, roadB) & asphalt
    groundB = np.zeros_like(asphalt)
    if ground == "on":
        nogA = shot("nogA", ["--no-ground"])
        nogB = shot("nogB", ["--no-ground"], delta)
        groundA = anydiff(fullA, nogA)
        groundB = anydiff(fullB, nogB)

    flip = asphalt & (coveredA != coveredB)
    buried = asphalt & ~coveredA & ~coveredB
    ground_over = asphalt & (groundA | groundB)

    vis = fullA.copy()
    vis[flip] = [255, 0, 0]
    vis[buried] = [0, 0, 255]
    h, w = fullA.shape[0], fullA.shape[1]
    with open(os.path.join(shots, f"{tag}_flip.ppm"), "wb") as fh:
        fh.write(b"P6\n%d %d\n255\n" % (w, h))
        fh.write(vis.astype(np.uint8).tobytes())

    res = dict(camera=camera, delta=delta, ground=ground, erode=erode_px,
               mask=n_road, control=ctrl,
               coveredA=int(coveredA.sum()), coveredB=int(coveredB.sum()),
               flip=int(flip.sum()), flip_pct=100.0 * flip.sum() / max(1, n_road),
               buried=int(buried.sum()), ground_over=int(ground_over.sum()),
               ground_px=int(groundA.sum()))
    if verbose:
        print(f"tag={tag} camera={camera} delta={delta} ground={ground}")
        print(f"control        {ctrl} px differ between two identical renders")
        print(f"asphalt mask   {n_road} px (eroded {erode_px})")
        print(f"covered A      {res['coveredA']} px   covered B {res['coveredB']} px")
        print(f"fight flip     {res['flip']} px ({res['flip_pct']:.3f}% of the road)")
        print(f"buried both    {res['buried']} px")
        print(f"ground overlap {res['ground_over']} px   ground pixels {res['ground_px']}")
        print("RESULT "
              f"mask={n_road} control={ctrl} coveredA={res['coveredA']} "
              f"coveredB={res['coveredB']} flip={res['flip']} "
              f"flip_pct={res['flip_pct']:.4f} buried={res['buried']} "
              f"ground_over={res['ground_over']} ground_px={res['ground_px']}")
    if not keep:
        for n in ("roadonly", "nothing", "a0", "a1", "fullA", "roadA", "nogA",
                  "fullB", "roadB", "nogB"):
            try:
                os.remove(os.path.join(shots, f"_{tag}_{n}.bmp"))
            except OSError:
                pass
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.path.join(ROOT, "build", "ht2re"))
    ap.add_argument("--data", default=os.path.join(ROOT, "original"))
    ap.add_argument("--shots", default=os.path.join(ROOT, "shots"))
    ap.add_argument("--tag", default="fight")
    ap.add_argument("--camera", default="-1740.5,31,3960,80.5,-14")
    ap.add_argument("--spawn", default="-2886,-4643,70")
    ap.add_argument("--delta", type=float, default=0.03, help="yaw perturbation (deg)")
    ap.add_argument("--erode", type=int, default=2)
    ap.add_argument("--tol", type=int, default=6, help="per-channel difference threshold")
    ap.add_argument("--ground", default="on", choices=["on", "off"],
                    help="ground in the measured frames (the suspect); off = control")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    measure(bin_path=args.bin, data=args.data, shots=args.shots, tag=args.tag,
            camera=args.camera, spawn=args.spawn, delta=args.delta, erode_px=args.erode,
            tol=args.tol, ground=args.ground, keep=args.keep)
    return 0


if __name__ == "__main__":
    sys.exit(main())

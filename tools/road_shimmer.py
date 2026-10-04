#!/usr/bin/env python3
"""Quantitative measurement of road-texture shimmer (minification aliasing).

The reported bug is "the road texture shimmers/jitters on curves and at
distance".  Shimmer is *temporal aliasing*: neighbouring pixels sample the wrong
texels, so the pattern changes from frame to frame.  This tool measures it three
ways and prints a row profile, so the effect can be attributed to distance.

Method
------
1. A grazing road/curve view is rendered twice from nearly identical camera
   poses (--pose with the yaw perturbed by --delta degrees; 0.02 deg is ~0.3 px
   at 1280x720, where the 60-deg vertical FOV is 12 px/deg vertically and
   ~14 px/deg horizontally).
2. A ground-truth reference is rendered from the *same* pose at --ss times the
   resolution and box-filtered down.  This is the anti-aliased answer: unlike
   "difference between two frames", the error against it cannot be reduced by
   simply blurring the image.
3. The road region is measured, not guessed: the engine is rendered once with
   `--only-mat mat_road` and once with a material prefix that matches nothing;
   the pixels where the two differ are road pixels.  The mask is eroded so
   silhouette edges (which move with the camera but are not texture shimmer) are
   excluded, and intersected with --band so no resolution-dependent UI is in it.

Metrics over the road pixels:
  mad        mean |A-B| between the two perturbed frames (naive "temporal" metric)
  lap_var    variance of the 3x3 Laplacian of frame A (total high-frequency energy)
  lap_mad    mean |lap(A)-lap(B)| (high-frequency energy that moves)
  mae/rmse   error of frame A against the supersampled reference (aliasing)
  speckle    variance of the Laplacian of that error (spurious high frequency)

usage: python3 tools/road_shimmer.py --tag before --aniso 1
       python3 tools/road_shimmer.py --tag after  --aniso 8
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
try:
    import numpy as np
except ImportError:                                    # pragma: no cover
    sys.exit("numpy is required for the shimmer metric")

ROAD_MAT = "mat_road"
NO_MAT = "mat_zzznone"


def read_bmp(path):
    """Return (h, w, ndarray[h][w][3]) with row 0 = TOP row, channels R,G,B."""
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
    out[:, :, 0] = raw[:, :, 2]                       # file order is B,G,R
    out[:, :, 1] = raw[:, :, 1]
    out[:, :, 2] = raw[:, :, 0]
    if flip:
        out = out[::-1]
    return h, w, out


def laplacian(img):
    """3x3 four-neighbour Laplacian, float32; output is 2 px smaller per axis."""
    f = img.astype(np.float32)
    return f[:-2, 1:-1] + f[2:, 1:-1] + f[1:-1, :-2] + f[1:-1, 2:] - 4.0 * f[1:-1, 1:-1]


def erode(mask, n):
    """Binary erosion by an n-pixel square (no scipy): n passes of 3x3 min."""
    m = mask.copy()
    for _ in range(n):
        p = np.pad(m, 1, constant_values=False)
        m = (p[:-2, 1:-1] & p[2:, 1:-1] & p[1:-1, :-2] & p[1:-1, 2:] & p[1:-1, 1:-1])
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.path.join(ROOT, "build", "ht2re"))
    ap.add_argument("--data", default=os.path.join(ROOT, "original"))
    ap.add_argument("--shots", default=os.path.join(ROOT, "shots"))
    ap.add_argument("--pose", default="580,30,-70,174.5,-3",
                    help="camera x,y,z,yaw,pitch of the grazing road/curve view")
    ap.add_argument("--delta", type=float, default=0.02,
                    help="yaw perturbation in degrees between the two frames")
    ap.add_argument("--tag", default="shimmer", help="output name prefix in --shots")
    ap.add_argument("--aniso", default=None,
                    help="HT2_ANISO value for the engine; 1 disables anisotropy")
    ap.add_argument("--erode", type=int, default=3,
                    help="erosion of the road mask (silhouette edge rejection)")
    ap.add_argument("--threshold", type=int, default=6,
                    help="|road-only - no-road| needed to call a pixel road")
    ap.add_argument("--ss", type=int, default=4,
                    help="supersampling factor of the ground-truth reference (0 = off)")
    ap.add_argument("--band", default="0,150,1040,680",
                    help="x0,y0,x1,y1 region with no resolution-dependent UI")
    ap.add_argument("--keep", action="store_true", help="keep the raw BMPs")
    args = ap.parse_args()

    px, py, pz, yaw, pitch = (float(v) for v in args.pose.split(","))
    bx0, by0, bx1, by1 = (int(v) for v in args.band.split(","))
    os.makedirs(args.shots, exist_ok=True)

    env = dict(os.environ)
    if args.aniso is not None:
        env["HT2_ANISO"] = str(args.aniso)

    work = os.path.join(args.shots, f"{args.tag}_raw") if args.keep \
        else tempfile.mkdtemp(prefix="ht2_shimmer_")

    def render(name, dyaw=0.0, extra=(), size="1280x720"):
        out = os.path.join(work, f"{name}.bmp")
        cam = f"{px},{py},{pz},{yaw + dyaw},{pitch}"
        cmd = [args.bin, "--data", args.data, "--scene", "world",
               "--spawn", "-3020,-4625,0", "--traffic", "0", "--size", size]
        cmd += list(extra) + ["--camera", cam, "--screenshot", "--out", out]
        r = subprocess.run(cmd, capture_output=True, text=True, errors="replace", env=env)
        if r.returncode != 0:
            sys.exit(f"engine failed: {' '.join(cmd)}\n{r.stdout[-2000:]}\n{r.stderr[-2000:]}")
        return read_bmp(out)[2]

    A = render("a")
    B = render("b", args.delta)
    A2 = render("a2")                                  # determinism control
    road = render("road", extra=("--only-mat", ROAD_MAT))
    noroad = render("noroad", extra=("--only-mat", NO_MAT))
    ref = None
    if args.ss > 1:
        n = args.ss
        big = render("ref", size=f"{1280 * n}x{720 * n}")
        ref = big.reshape(720, n, 1280, n, 3).astype(np.float32).mean(axis=(1, 3))

    # --- road mask ---------------------------------------------------------
    m = np.abs(road.astype(np.int16) - noroad.astype(np.int16)).max(axis=2) > args.threshold
    raw_px = int(m.sum())
    m = erode(m, args.erode)
    band = np.zeros_like(m)
    band[by0:by1, bx0:bx1] = True
    sel = m & band

    det = float(np.abs(A - A2).mean())
    det_px = int((np.abs(A - A2).max(axis=2) > 0).sum())

    print(f"tag={args.tag} aniso={args.aniso} pose={args.pose} delta={args.delta}deg "
          f"erode={args.erode} thr={args.threshold} ss={args.ss}")
    print(f"  determinism : mean|A-A2| = {det:.4f} over the whole frame "
          f"({det_px} differing px)")
    print(f"  road mask   : {raw_px} px raw, {int(m.sum())} px after {args.erode}px erosion, "
          f"{int(sel.sum())} px inside band {bx0},{by0},{bx1},{by1}")

    tot = float(np.abs(A.astype(np.float32) - B.astype(np.float32))[sel].mean())
    dmax = np.abs(A.astype(np.int16) - B.astype(np.int16)).max(axis=2)
    lv = laplacian(A)[sel[1:-1, 1:-1]].var()
    lm = np.abs(laplacian(A) - laplacian(B))[sel[1:-1, 1:-1]].mean()
    print(f"  frame pair  : mad={tot:8.4f}  changed={100 * (dmax[sel] > 0).mean():6.2f}%  "
          f"changed>8={100 * (dmax[sel] > 8).mean():5.2f}%  lap_var={lv:9.1f}  lap_mad={lm:7.4f}")
    out = dict(tag=args.tag, aniso=args.aniso, pose=args.pose, delta=args.delta,
               determinism=det, road_px=int(sel.sum()), mad=tot, lap_var=float(lv),
               lap_mad=float(lm))
    profile = []
    if ref is not None:
        e2 = A.astype(np.float32) - ref
        e = e2[sel]
        el = laplacian(e2)[sel[1:-1, 1:-1]]
        out.update(mae=float(np.abs(e).mean()), rmse=float(np.sqrt((e * e).mean())),
                   max_err=float(np.abs(e).max()), speckle=float(el.var()))
        print(f"  vs truth    : mae={np.abs(e).mean():8.4f}  rmse={np.sqrt((e * e).mean()):8.4f}  "
              f"max|e|={np.abs(e).max():6.1f}  speckle={el.var():9.1f}")
        print("  row profile (distance):")
        print("     rows    |   px    |  mae   |  rmse")
        for y in range(by0, by1, 50):
            s2 = np.zeros_like(sel)
            s2[y:min(y + 50, by1)] = sel[y:min(y + 50, by1)]
            if s2.sum() < 200:
                continue
            ee = e2[s2]
            profile.append(dict(rows=[y, min(y + 50, by1)], px=int(s2.sum()),
                                mae=float(np.abs(ee).mean()),
                                rmse=float(np.sqrt((ee * ee).mean()))))
            print(f"   {y:4d}-{min(y+50,by1):<4d}| {int(s2.sum()):7d} | {np.abs(ee).mean():6.3f} | "
                  f"{np.sqrt((ee*ee).mean()):6.3f}")
        out["profile"] = profile
    with open(os.path.join(args.shots, f"{args.tag}_metrics.json"), "w") as fh:
        import json
        json.dump(out, fh, indent=1)

    # --- deliverable PNGs --------------------------------------------------
    try:
        from PIL import Image
        for name, arr in (("a", A), ("b", B)):
            Image.fromarray(arr.astype(np.uint8)).save(
                os.path.join(args.shots, f"{args.tag}_{name}.png"))
        d = np.abs(A.astype(np.int16) - B.astype(np.int16)).max(axis=2)
        Image.fromarray(np.clip(d * 6, 0, 255).astype(np.uint8)).save(
            os.path.join(args.shots, f"{args.tag}_diff6x.png"))
        vis = A.astype(np.uint8).copy()
        vis[sel] = (vis[sel] * 0.35 + np.array([255, 40, 40]) * 0.65).astype(np.uint8)
        Image.fromarray(vis).save(os.path.join(args.shots, f"{args.tag}_mask.png"))
        if ref is not None:
            Image.fromarray(np.clip(np.abs(A.astype(np.float32) - ref) * 4, 0, 255)
                            .astype(np.uint8)).save(
                os.path.join(args.shots, f"{args.tag}_refdiff4x.png"))
    except ImportError:
        pass
    if not args.keep:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()

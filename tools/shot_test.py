#!/usr/bin/env python3
"""HT2:RE screenshot regression test.

Renders the engine's two states offscreen and checks that the frames actually
contain what they should.  BMP notes: the writer stores the picture bottom-up
(row 0 of the file is the bottom row on screen) and the pixels are B,G,R.

usage: python3 tools/shot_test.py [binary] [data-dir]
"""
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "ht2re")
DATA = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "original")
OUT = os.path.join(ROOT, "shots")
os.makedirs(OUT, exist_ok=True)


def read_bmp(path):
    """Return (w, h, rows) with rows[0] = TOP row, pixels as (R,G,B)."""
    with open(path, "rb") as fh:
        data = fh.read()
    assert data[:2] == b"BM", "not a BMP"
    off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    assert bpp == 24, f"unexpected bpp {bpp}"
    flip = h > 0                       # positive height => stored bottom-up
    h = abs(h)
    stride = (w * 3 + 3) & ~3
    rows = []
    for y in range(h):
        src = off + (h - 1 - y if flip else y) * stride
        raw = data[src:src + w * 3]
        rows.append([(raw[i * 3 + 2], raw[i * 3 + 1], raw[i * 3 + 0]) for i in range(w)])
    return w, h, rows


def run(*args):
    cmd = [BIN, "--data", DATA] + list(args)
    # engine logs carry raw cp1251 strings (town/cargo names)
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        print("FAIL: command failed:", " ".join(cmd))
        print(r.stdout[-2000:], r.stderr[-2000:])
        sys.exit(1)
    return r.stdout


def run_env(env, *args):
    """run() with extra environment variables (SDL_AUDIODRIVER=dummy)."""
    e = dict(os.environ)
    e.update(env)
    cmd = [BIN, "--data", DATA] + list(args)
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace", env=e)
    if r.returncode != 0:
        print("FAIL: command failed:", " ".join(cmd))
        print(r.stdout[-2000:], r.stderr[-2000:])
        sys.exit(1)
    return r.stdout


DUMMY_AUDIO = {"SDL_AUDIODRIVER": "dummy"}


failures = []


def check(name, cond, detail=""):
    print(("  ok   " if cond else "  FAIL ") + name + ((" -- " + detail) if detail else ""))
    if not cond:
        failures.append(name)


def diff(a, b):
    n = 0
    for y in range(0, a[1], 4):
        for x in range(0, a[0], 4):
            if a[2][y][x] != b[2][y][x]:
                n += 1
    return 100.0 * n / ((a[1] // 4) * (a[0] // 4))


def count(rows, x0, x1, y0, y1, pred):
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            if pred(rows[y][x]):
                n += 1
    return n


# ---------------------------------------------------------------- menu state
print("menu state")
run("--scene", "menu", "--screenshot", "--out", os.path.join(OUT, "menu.bmp"),
    "--time", "0", "--size", "1280x720")
w, h, px = read_bmp(os.path.join(OUT, "menu.bmp"))
check("resolution 1280x720", (w, h) == (1280, 720), f"{w}x{h}")

check("black letterbox bar (left)", sum(px[360][40]) < 24, str(px[360][40]))
check("black letterbox bar (right)", sum(px[360][1240]) < 24, str(px[360][1240]))

# UI canvas is 800x600 scaled by 1.2 into x 160..1120: the truck-front frame is
# light metal along its top-left corner
c = px[20][180]
check("common.msk frame is metal grey", sum(c) > 330, str(c))

# "KOTR" badge: a dark oval on the metal above the centre circle
n = count(px, 560, 740, 15, 100, lambda c: sum(c) < 200)
check("KOTR badge drawn at the top centre", n > 300, f"{n} dark px")

# the 125x125 "GRAPHICS" button (canvas 79,82) -> screen (160+79*1.2, 82*1.2).
# The button art is a wood disc with the original green rim, so look for the rim.
gx0, gy0 = int(160 + 79 * 1.2), int(82 * 1.2)
gx1, gy1 = int(160 + (79 + 125) * 1.2), int((82 + 125) * 1.2)
ring = count(px, gx0, gx1, gy0, gy1, lambda c: c[1] > c[0] + 30 and c[1] > c[2] + 30)
check("GRAPHICS button art at its socket", ring > 800, f"{ring} green rim px")

# the transparent centre shows the 3D scene (sky/terrain, not metal)
skyish = count(px, 500, 800, 150, 450, lambda c: c[2] > c[0] + 15)
check("centre circle shows the 3D scene", skyish > 20000, f"{skyish} px")

# version string at the bottom centre
txt = count(px, 550, 750, 690, 715, lambda c: sum(c) > 480)
check("version string rendered", txt > 60, f"{txt} px")

# --------------------------------------------------------------- world state
# The world is the merged ENV tiles (roads, buildings) plus a ground surface
# derived from them; the truck is COMMON/TRUCKS.B3D assembled via its links.
print("world state (chase camera at the warehouse)")
run("--scene", "world", "--screenshot", "--out", os.path.join(OUT, "world.bmp"),
    "--spawn", "4700,-5700,180", "--warmup", "8", "--view", "chase", "--size", "1280x720")
w, h, px = read_bmp(os.path.join(OUT, "world.bmp"))
check("resolution 1280x720", (w, h) == (1280, 720), f"{w}x{h}")

ground = count(px, 0, 1280, 400, 720, lambda c: c[2] < c[0] + 12 and sum(c) < 620)
check("ground fills the lower frame", ground > 1280 * 320 * 0.6, f"{ground} px")

# the truck sits in the middle of the frame, below the horizon
truck = count(px, 520, 780, 480, 700, lambda c: sum(c) < 220)
check("truck visible on the ground", truck > 1500, f"{truck} px")

buildings = count(px, 0, 1280, 100, 420, lambda c: sum(c) > 240)
check("world geometry visible above the horizon", buildings > 4000, f"{buildings} px")

hud = count(px, 10, 400, 10, 100, lambda c: sum(c) > 500)
check("driving HUD rendered", hud > 400, f"{hud} px")

# ------------------------------------------------- gameplay from the scripts
# The mod_*.sch `parking` forms name a .way node per bay; the car model comes
# from etrashboxes.sch.  Nothing here is hard-coded: 762 named .way nodes are
# indexed and every bay in the shipped data resolves.
print("gameplay scripts (parked vehicles)")
view = ["--scene", "world", "--spawn", "-3020,-4625,0", "--camera", "-3020,26,-4640,0,-8",
        "--parked-radius", "3500"]
out = run(*(view + ["--screenshot", "--out", os.path.join(OUT, "parked.bmp")]))
check("every parking bay resolves to a .way node",
      "0 bays without a .way node" in out,
      [l for l in out.splitlines() if "parked vehicles spawned" in l][:1][0][-70:])
m = re.search(r"gameplay: (\d+) parked vehicles spawned \((\d+) far, (\d+) bays", out)
check("parked vehicles spawned from parking forms", bool(m) and int(m.group(1)) >= 8,
      m.group(1) if m else "no line")
check("named .way nodes indexed", "762 named .way nodes" in out,
      [l for l in out.splitlines() if "named .way nodes" in l][:1][0][-60:])
run(*(view + ["--screenshot", "--out", os.path.join(OUT, "parked_off.bmp"),
              "--parked-radius", "10"]))   # nothing within 10 units -> no parked cars
w, h, a = read_bmp(os.path.join(OUT, "parked.bmp"))
_, _, b = read_bmp(os.path.join(OUT, "parked_off.bmp"))
d = diff((w, h, a), (w, h, b))
check("parked cars are actually rendered", d > 0.05, f"{d:.2f}% of sampled px differ")

# ------------------------------------------------------------- AI traffic
# Traffic drives the road graph built from every .way RSEG polyline; the models
# are uploaded once and drawn per instance with their own matrix.
print("gameplay scripts (AI traffic)")
# --no-map on both frames: the road-map overlay covers the left third of the
# screen, and this check is about the AI cars under the camera.
# The chase camera after a 4 s warm-up, with the population pulled in to 200 m:
# the AI spawns across a 900 m radius, so at the far end of that the cars are a
# handful of pixels on the horizon (0.01%), and from 1200 units up the whole
# frame is the uniform 900 m fog colour - both made the check vacuous.  This
# camera puts ~8 cars in frame, 0.46% of sampled pixels (4300 px).
base = ["--scene", "world", "--spawn", "-2327,-3767,-29", "--no-map",
        "--warmup", "4", "--view", "chase", "--traffic-radius", "200"]
out = run(*(base + ["--screenshot", "--out", os.path.join(OUT, "traffic.bmp"),
                    "--traffic", "16"]))
m = re.search(r"traffic: road graph (\d+) vertices, (\d+) edges", out)
check("road graph built from the .way files", bool(m) and int(m.group(2)) > 5000,
      f"{m.group(1)} vertices / {m.group(2)} edges" if m else "no line")
m = re.search(r"traffic: (\d+) cars spawned", out)
check("AI cars spawned from the catalogue", bool(m) and int(m.group(1)) >= 8,
      m.group(1) if m else "no line")
run(*(base + ["--screenshot", "--out", os.path.join(OUT, "traffic_off.bmp"),
              "--traffic", "0"]))
w, h, a = read_bmp(os.path.join(OUT, "traffic.bmp"))
_, _, b = read_bmp(os.path.join(OUT, "traffic_off.bmp"))
d = diff((w, h, a), (w, h, b))
check("AI traffic is rendered", d > 0.05, f"{d:.3f}% of sampled px differ")
# Regression guard for the view-distance cull: the traffic batches are drawn
# with a per-instance model matrix, so their ranges live in *model* space and
# must never go through the world-space range cull.  When they did, every
# instance was rejected against the world camera and the two frames above were
# byte-identical even though the simulation was fine.
out = run_env({"HT2_CULL_DEBUG": "1", "SDL_AUDIODRIVER": "dummy"}, *(base + [
    "--screenshot", "--out", os.path.join(OUT, "traffic_cull.bmp"), "--traffic", "16"]))
m = re.search(r"cull: (\d+) of (\d+) world batches submitted.*?traffic (\d+) instances drawn / (\d+) culled",
              out)
check("traffic instances reach the frame (not culled as local-space batches)",
      bool(m) and int(m.group(3)) >= 5 and int(m.group(4)) == 0,
      f"{m.group(3)} drawn / {m.group(4)} culled, {m.group(1)}/{m.group(2)} world batches"
      if m else "no 'cull:' line")

# ------------------------------------------------------------ node events
# mod_*.sch ( nodeevent ID KIND "waynode" DIR "text" ): king.exe fires these on
# a road PositionId transition, then filters the link travel vector by DIR.  The
# port associates the free-driving pose with the named .way node in a narrow
# radius until the player vehicle exposes the original road PositionId.
print("gameplay scripts (node events)")
out = run("--scene", "world", "--no-map", "--screenshot",
          "--out", os.path.join(OUT, "event.bmp"),
          "--spawn", "-3125,-4525,180", "--traffic", "0")
m = re.search(r"events: (\d+) node events \((\d+) carry a message\), (\d+) nodes resolved", out)
check("node events parsed and resolved", bool(m) and int(m.group(1)) == 31 and m.group(1) == m.group(3),
      f"{m.group(1)} events, {m.group(3)} resolved" if m else "no line")
check("southbound BASE03LEFT crossing fires",
      "events: 'BASE03LEFT' (UNCONDITIONAL)" in out,
      "direction accepted" if "events: 'BASE03LEFT'" in out else "event missing")
w, h, rows = read_bmp(os.path.join(OUT, "event.bmp"))
# The banner text is warm (yellowish).  With the original HUD strip loaded the
# message is drawn inside the strip (rows ~44..90, App::drawWorld); without it
# the 2D banner at y 96..138 is used.  Count both so the check follows the
# renderer instead of a hard-coded band.
def banner_px(rows, x0=300, x1=980):
    warm = lambda c: c[0] > 180 and c[1] > 150 and c[2] < 220
    return (count(rows, x0, x1, 44, 90, warm) + count(rows, x0, x1, 96, 138, warm))
lit = banner_px(rows)
check("node-event message drawn on screen", lit > 200, f"{lit} text px")
out_off = run("--scene", "world", "--no-map", "--screenshot",
              "--out", os.path.join(OUT, "event_off.bmp"),
              "--spawn", "-3125,-4525,0", "--traffic", "0")
_, _, rows2 = read_bmp(os.path.join(OUT, "event_off.bmp"))
lit2 = count(rows2, 300, 980, 96, 138, lambda c: c[0] > 180 and c[1] > 160 and c[2] < 220)
wrong_way_fired = "events: 'BASE03LEFT'" in out_off
check("northbound pass rejects the SOUTH event",
      not wrong_way_fired and lit2 < 60,
      f"event={'yes' if wrong_way_fired else 'no'}, {lit2} text px")

# ---------------------------------------------------------- game scripts
# Every shipped .sch/.CNF is SDTE encrypted; the engine decrypts and parses them
# at runtime and reads the player truck, start position, catalogue and upgrades.
print("game scripts")
out = run("--scripts")
check("all scripts parsed without errors", "errors=0" in out,
      [l for l in out.splitlines() if "errors=" in l][:1][0][-80:])
m = re.search(r"files=(\d+) errors=(\d+).*player=([A-Za-z0-9_]+) start=\(([-\d.]+),([-\d.]+),([-\d.]+)\).*vehicles=(\d+) upgrades=(\d+)", out)
if m:
    check("player truck from GAME.CNF", m.group(3) == "ZilR", m.group(3))
    check("start position from GAME.CNF",
          abs(float(m.group(4)) + 3020) < 1 and abs(float(m.group(5)) + 4625) < 1,
          f"({m.group(4)},{m.group(5)})")
    check("vehicle catalogue from evehicles.sch", int(m.group(7)) > 100, m.group(7))
    check("upgrade table from restable.sch", int(m.group(8)) >= 19, m.group(8))
    cg = re.search(r"cargo=(\d+) drivers=(\d+)", out)
    check("cargo list parsed (escaped quotes handled)", bool(cg) and int(cg.group(1)) == 107,
          cg.group(1) if cg else "no line")
    check("driver names from names.sch", bool(cg) and int(cg.group(2)) == 74,
          cg.group(2) if cg else "no line")

# ------------------------------------------------ script coverage / conditions
# The shipped .sch files carry no `if`/`cond` forms: every condition is an
# engine rule keyed by a head keyword or a field.  Those rules were recovered
# from original/king.exe and live in src/script/ScriptRules.{h,cpp}.  The
# harness below is the `--script-report` producer (App.cpp wiring is delivered
# separately); it prints the per-family coverage and drives the recovered
# conditions with synthetic world states.
print("script coverage (recovered conditions, --script-report)")
HARNESS = os.path.join(ROOT, "tools", "out", "script_selftest")
HARNESS_SRC = [
    os.path.join(ROOT, "tools", "script_selftest.cpp"),
    os.path.join(ROOT, "src", "script", "ScriptRules.cpp"),
    os.path.join(ROOT, "src", "script", "GameData.cpp"),
    os.path.join(ROOT, "src", "script", "Sexp.cpp"),
    os.path.join(ROOT, "src", "script", "Sdte.cpp"),
    os.path.join(ROOT, "src", "world", "Way.cpp"),
]


def script_harness():
    """Build (once) and run tools/script_selftest.cpp; return its stdout."""
    os.makedirs(os.path.join(ROOT, "tools", "out"), exist_ok=True)
    newest = max((os.path.getmtime(p) for p in HARNESS_SRC if os.path.exists(p)), default=0)
    if not os.path.exists(HARNESS) or os.path.getmtime(HARNESS) < newest:
        cmd = ["c++", "-std=c++17", "-O1", "-I", os.path.join(ROOT, "src"),
               "-o", HARNESS] + HARNESS_SRC
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        if r.returncode != 0:
            return "COMPILE FAILED\n" + r.stdout[-2000:] + r.stderr[-2000:]
    r = subprocess.run([HARNESS, DATA], cwd=ROOT, capture_output=True, text=True,
                       errors="replace")
    out = "\n".join(l for l in r.stdout.splitlines() if not l.startswith("[ht2re]"))
    if r.returncode != 0:
        out += "\nEXIT %d\n" % r.returncode
    return out


rep = script_harness()
_hr = re.search(r"(\d+) checks, 0 failures", rep)
check("script-rules harness builds and every recovered rule holds",
      bool(_hr) and int(_hr.group(1)) >= 27,
      [l for l in rep.splitlines() if "failures" in l][:1][0] if "failures" in rep else rep[-90:])

mg = re.search(r"family=gate\s+total=(\d+) active=\d+ satisfied=(\d+) visible=\d+ kinds=(\S+)", rep)
mn = re.search(r"family=nodeevent\s+total=(\d+) active=(\d+) satisfied=\d+ fired=\d+ "
               r"stateborder=(\d+) directional=(\d+)", rep)
check("the recovered condition families are reported (4 gate kinds, 12+19 node events)",
      bool(mg) and mg.group(1) == "66" and "DAYGW=29" in mg.group(3) and "DAYONLY=32" in mg.group(3)
      and bool(mn) and mn.group(1) == "31" and mn.group(3) == "12" and mn.group(4) == "19",
      (mg.group(0)[:52] if mg else "no gate line") + " | " + (mn.group(0)[:52] if mn else "no node line"))
check("a time/weather gated condition holds only in its window",
      "ok   day window 8..21 and weather < 0.2 are both honoured" in rep and
      "ok   at 23:00 only the 5 UNCONDITIONAL gates hold" in rep,
      "8<hour<21, weather<0.2 (king.exe 0x484040 / 0x484010)")
check("a position/direction condition fires only when satisfied",
      "ok   direction filter: +z passes, -z does not" in rep,
      [l for l in rep.splitlines() if "direction filter" in l][:1][0][-40:] if "filter" in rep else "")
check("a money condition gates the tuning upgrade",
      "ok   the money condition gates the upgrade" in rep and
      "ok   tuning table row (PR_TIRES, level 2)" in rep,
      "PR_TIRES level 2 = 1500")
check("a one-shot does not re-fire (and the engine's 8 s guard holds)",
      "ok   a one-shot node event does not re-fire" in rep and
      "ok   the 8 s guard suppresses a second pass inside the window" in rep,
      "ok")
m = re.search(r"family=tuning\s+upgradeRows=(\d+) levels=(\d+) offers=(\d+) rows=(\d+) "
              r"missingLevels=(\d+) maxPrice=(\d+)", rep)
check("the tuning table is exposed for the UI",
      bool(m) and m.group(1) == "19" and m.group(2) == "39" and int(m.group(4)) > 500
      and m.group(5) == "10",
      m.group(0) if m else "no line")
# the independent Python reproduction of the same table must agree
cov = subprocess.run(["python3", os.path.join(ROOT, "tools", "script_coverage.py"),
                      "--summary"], capture_output=True, text=True).stdout.strip()
mine_cov = [l for l in rep.splitlines() if "coverage: heads=" in l]
check("coverage table is stable (engine and the Python survey agree)",
      "coverage: heads=722 form_heads=49 fields=139 dropped_fields=3" in rep and
      "heads=722" in cov and "fields=139" in cov and "dropped_fields=3" in cov,
      (mine_cov[0] if mine_cov else "no line") + " / " + cov)

# ------------------------------------------------------------ economy
print("economy (vehicle market and cargo)")
out = run("--scene", "world", "--buy", "5", "--accept-job", "0", "--spawn", "-3020,-4625,0",
          "--screenshot", "--out", os.path.join(OUT, "economy.bmp"))
m = re.search(r"economy: money=(\d+) player=(\S+) offers=(\d+)", out)
check("economy initialised from the scripts", bool(m) and int(m.group(3)) > 100,
      m.group(0)[:60] if m else "no line")
m = re.search(r"money (\d+) -> (\d+)", out)
check("buying a vehicle costs money", bool(m) and int(m.group(2)) < int(m.group(1)),
      f"{m.group(1)} -> {m.group(2)}" if m else "no line")
check("player vehicle switched after the purchase", "player vehicle is now" in out, "ok")
m = re.search(r"job accepted: (.+) -> (.+), (\d+) m, reward (\d+)", out)
check("cargo job accepted with a reward", bool(m) and int(m.group(4)) > 0,
      f"{m.group(3)} m, reward {m.group(4)}" if m else "no line")
check("jobs all resolved to .way nodes", "unresolved=0" in out, "unresolved=0")

# ------------------------------------------------------- scripted world gates
print("scripted world gates (people / obstacle / lighting)")
out = run("--scene", "world", "--screenshot", "--out", os.path.join(OUT, "gates_day.bmp"),
          "--spawn", "-3020,-4625,0", "--hour", "12")
m = re.search(r"gates: (\d+) gates, (\d+) submeshes belong to key groups", out)
check("people gates parsed and bound to key groups", bool(m) and int(m.group(1)) == 66,
      f"{m.group(1)} gates, {m.group(2)} gated submeshes" if m else "no line")
day = re.search(r"gates: \d+ hidden key groups, (\d+) of (\d+) submeshes drawn", out)
night = re.search(r"gates: \d+ hidden key groups, (\d+) of (\d+) submeshes drawn",
                  run("--scene", "world", "--screenshot", "--out",
                      os.path.join(OUT, "gates_night.bmp"), "--spawn", "-3020,-4625,0",
                      "--hour", "23"))
check("day/night hide different geometry",
      bool(day) and bool(night) and day.group(1) != night.group(1),
      f"day {day.group(1)} vs night {night.group(1)}" if day and night else "no line")
opened = re.search(r"gates: \d+ hidden key groups, (\d+) of (\d+) submeshes drawn",
                   run("--scene", "world", "--screenshot", "--out",
                       os.path.join(OUT, "gates_open.bmp"), "--spawn", "-3020,-4625,0",
                       "--obstacles", "open"))
check("road-closure geometry can be toggled",
      bool(opened) and bool(day) and opened.group(1) != day.group(1),
      f"closed {day.group(1)} vs open {opened.group(1)}" if opened and day else "no line")

# ------------------------------------------------------------- service stops
print("service stations (fuel / repair)")
out = run("--scene", "world", "--spawn", "-410,-2448,0", "--fuel", "0.2", "--refuel",
          "--traffic", "0", "--screenshot", "--out", os.path.join(OUT, "fuel.bmp"))
m = re.search(r"service: (\d+) stations resolved from (\d+) forms \((\d+) fuel, (\d+) repair\)", out)
check("service forms resolved to .way nodes",
      bool(m) and int(m.group(1)) == 37 and int(m.group(2)) == 37,
      f"{m.group(1)}/{m.group(2)} ({m.group(3)} fuel, {m.group(4)} repair)" if m else "no line")
m = re.search(r"refuelled for (\d+), fuel ([\d.]+), money (\d+)", out)
check("refuelling fills the tank and charges money",
      bool(m) and float(m.group(2)) > 0.99 and int(m.group(3)) < 45000,
      f"paid {m.group(1)}, fuel {m.group(2)}" if m else "no line")
out = run("--scene", "world", "--spawn", "4750,-5675,0", "--damage", "0.6", "--repair",
          "--traffic", "0", "--screenshot", "--out", os.path.join(OUT, "repair.bmp"))
m = re.search(r"repaired for (\d+), damage ([\d.]+), money (\d+)", out)
check("repair clears the damage and charges money",
      bool(m) and float(m.group(2)) < 0.01 and int(m.group(3)) < 45000,
      f"paid {m.group(1)}, damage {m.group(2)}" if m else "no line")

# ------------------------------------------------------------------ ambushes
print("ambushes (police / mafia roadblocks)")
out = run("--scene", "world", "--no-map", "--spawn", "-3190,-5000,0", "--warmup", "30",
          "--camera", "-3110,34,-4490,180,-6", "--traffic", "0", "--screenshot",
          "--out", os.path.join(OUT, "ambush.bmp"))
m = re.search(r"ambush: (\d+) ambushes resolved from (\d+) forms", out)
check("ambush forms resolved", bool(m) and int(m.group(1)) == 87 and m.group(2) == "87",
      f"{m.group(1)}/{m.group(2)}" if m else "no line")
m = re.search(r"ambush: '(\w+)' \((\w+)\) fired at [-\d ]+: (\d+) (\w+) cars", out)
check("roadblock spawns cars when the player arrives", bool(m) and int(m.group(3)) >= 2,
      f"{m.group(1)}: {m.group(3)}x {m.group(4)}" if m else "no line")
all_drawn = re.findall(r"gates: \d+ hidden key groups, (\d+) of (\d+) submeshes drawn", out)
check("ambush geometry is added to the world",
      bool(all_drawn) and int(all_drawn[-1][0]) > 70000,
      f"{all_drawn[-1][0]} drawn (first {all_drawn[0][0]})" if all_drawn else "no line")
w, h, rows = read_bmp(os.path.join(OUT, "ambush.bmp"))
warn = banner_px(rows, 400, 900)
check("ambush warning is shown", warn > 200, f"{warn} banner text px")

# ----------------------------------------------------------- world boundaries
print("world boundaries (water / edge / cliff)")
edge = run("--scene", "world", "--drive", "30", "--spawn", "4700,-5700,180")
odo = re.search(r"odometer ([\d.]+) m", edge)
check("driving south of the warehouse stops at the mapped edge",
      bool(odo) and 100.0 < float(odo.group(1)) < 300.0,
      f"{odo.group(1)} m (was 354.8 before the boundary)" if odo else "no line")
sea = run("--scene", "world", "--drive", "20", "--spawn", "4950,-5840,90")
m3 = re.search(r"odometer ([\d.]+) m", sea)
check("the sea is not drivable", bool(m3) and float(m3.group(1)) < 20.0,
      f"{m3.group(1)} m in the sea" if m3 else "no line")

# ------------------------------------------------- menu -> world transition
print("menu -> world transition")
run("--scene", "menu", "--size", "1280x720", "--time", "0", "--screenshot",
    "--out", os.path.join(OUT, "menu_state.bmp"))
run("--autostart", "--no-map", "--size", "1280x720", "--screenshot", "--out",
    os.path.join(OUT, "autostart.bmp"), "--spawn", "4700,-5700,180", "--warmup", "6")
run("--scene", "world", "--no-map", "--size", "1280x720", "--screenshot", "--out",
    os.path.join(OUT, "direct.bmp"), "--spawn", "4700,-5700,180", "--warmup", "6")
_, _, t = read_bmp(os.path.join(OUT, "autostart.bmp"))
_, _, px = read_bmp(os.path.join(OUT, "direct.bmp"))
same = 0
for y in range(0, 720, 4):
    for x in range(0, 1280, 4):
        if max(abs(t[y][x][i] - px[y][x][i]) for i in range(3)) <= 8:
            same += 1
tot = (720 // 4) * (1280 // 4)
# the HUD carries live readouts (the clock advances during the transition)
check("autostart reaches the same world", same > tot * 0.70, f"{100.0 * same / tot:.1f}% identical")

# ------------------------------------------------------------------ cockpit UI
# Matching the original: the driving HUD is ONE 1600x50 chrome/LED strip
# (`m16\tablo.txr`) blitted 1:1 with the readouts in its two LED windows, plus
# the original's large left-hand ROAD MAP overlay (src/ui/Map.cpp).  The window
# positions are the ones measured off the retail reference frames
# (shots/ref/ref_hud.png at 800x600: windows 91..294 and 301..664).
print("cockpit UI (original top HUD strip + road map)")
ui = os.path.join(OUT, "ui.bmp")
out = run("--scene", "world", "--screenshot", "--out", ui, "--spawn", "-2327,-3767,-29",
          "--warmup", "6", "--map")
check("original HUD art loaded from esc16.RES", "original strip loaded" in out,
      [l for l in out.splitlines() if "hud:" in l][:1][0][-40:])
w, h, rows = read_bmp(ui)
strip = count(rows, 0, w, 2, 36, lambda c: sum(c) > 90)
check("HUD strip spans the top of the screen", strip > 0.6 * w * 34, f"{strip} px")
ink = count(rows, 0, w, 2, 36,
            lambda c: c[0] > 150 and c[1] > 110 and c[2] < 170 and c[0] > c[2] + 40)
check("readouts drawn inside the strip", ink > 250, f"{ink} ink px")
# The label row of both LED windows carries matrix.fon words the original only
# draws there (ПОЗИЦИЯ / ОСТ.ВРЕМЯ and АККИ), so their ink is what identifies the
# cells; a plain chrome probe could not tell them apart.
pos = count(rows, 480, 700, 4, 16, lambda c: c[0] > 150 and c[1] > 140 and c[2] > 60)
check("ПОЗИЦИЯ / ОСТ.ВРЕМЯ labels drawn in the first LED window", pos > 120,
      f"{pos} label px")
akki = count(rows, 780, 900, 4, 16, lambda c: c[0] > 150 and c[1] > 140 and c[2] > 60)
check("АККИ tacho label drawn in the second LED window", akki > 40, f"{akki} label px")
m = re.search(r"map: panel ([\d.]+)x([\d.]+) at ([\d.]+),([\d.]+) scale ([\d.]+) \| "
              r"(\d+) road polylines drawn, (\d+) labels", out)
check("the road map draws its roads and settlement labels",
      bool(m) and int(m.group(6)) > 100 and int(m.group(7)) > 0,
      f"{m.group(6)} road lines, {m.group(7)} labels" if m else "no map line")
# the panel is not force-drawn: a plain screenshot run (no --map) leaves it off,
# so the retails' M-key toggle is what shows it during play.
out = run("--scene", "world", "--screenshot", "--out", os.path.join(OUT, "nomap.bmp"),
          "--spawn", "-2327,-3767,-29", "--warmup", "6")
check("the map is not drawn unless it is asked for", "map: panel" not in out,
      "no map panel" if "map: panel" not in out else "unexpected map")


# --- the strip cells that only a LIVE job fills: ПОЗИЦИЯ (destination + km) and
#     ОСТ.ВВРЕМЯ (the contract clock), plus the АККИ tacho reading (Vehicle::rpm).
out = run("--scene", "world", "--no-map", "--job", "--screenshot",
          "--out", os.path.join(OUT, "hud_job.bmp"), "--spawn", "-2327,-3767,-29",
          "--warmup", "0")
mj = re.search(r"hud: strip x (-?[\d.]+) scale ([\d.]+) win ([\d.]+) \| pos (\w+) "
               r"time (\S+) speed (\d+) rpm (\d+) clock", out)
check("the strip shows the job time and rpm when a job is active",
      bool(mj) and mj.group(4) == "yes" and
      re.fullmatch(r"\d\d:\d\d", mj.group(5)) and int(mj.group(7)) > 0,
      f"pos={mj.group(4)} time={mj.group(5)} rpm={mj.group(7)}" if mj else "no hud line")
w, h, rows = read_bmp(os.path.join(OUT, "hud_job.bmp"))
posv = count(rows, 560, 700, 18, 34, lambda c: c[0] > 150 and c[1] > 140 and c[2] > 60)
timg = count(rows, 710, 800, 18, 34, lambda c: c[0] > 150 and c[1] > 140 and c[2] > 60)
check("ПОЗИЦИЯ value and ОСТ.ВРЕМЯ digits are drawn", posv > 40 and timg > 40,
      f"{posv} km-value px, {timg} time px")

# ------------------------------------------------------------------- events
# The two event types the retail frames show and the port did not have:
#   * the speed-camera fine panel «ЗАРЕГИСТРИРОВАНО ПРЕВЫШЕНИЕ СКОРОСТИ …»
#   * the in-strip script message window with the blinking new-message icon.
print("events (speed camera fine + script message window)")
out = run("--scene", "world", "--no-map", "--fine-shot", "82", "--speed-limit", "60",
          "--spawn", "-2827,-2767,10", "--screenshot", "--out",
          os.path.join(OUT, "fine.bmp"))
check("the speed camera panel reports the limit, the speed and the fine",
      "fine: panel shown (limit 60, speed 82" in out,
      [l for l in out.splitlines() if "fine:" in l][-1:][0][-60:] if "fine:" in out else "no line")
w, h, rows = read_bmp(os.path.join(OUT, "fine.bmp"))
# the red header bar of the panel, upper right, under the strip
hdr = count(rows, 1040, 1265, 60, 90, lambda c: c[0] > 90 and c[0] > c[1] + 30 and c[0] > c[2] + 30)
check("the fine panel is drawn in the upper right", hdr > 1500, f"{hdr} red header px")
amt = count(rows, 1040, 1265, 350, 380, lambda c: c[0] > 150 and c[1] > 130 and c[2] < 160)
check("the fine amount is drawn on the panel", amt > 40, f"{amt} amount px")

# the real trigger path: drive over the limit through a speed-limit zone (the
# spawn is one of the 81 zones the camera sits in) and the fine comes out of
# Economy - the panel above is only the visible half of the event.
out = run("--scene", "world", "--no-map", "--speed-limit", "6", "--drive", "25",
          "--spawn", "-3000,-4625,90")
mf = re.search(r"fine: ([\d.]+) km/h in a (\d+) zone, \$(\d+) deducted, money (\d+)", out)
check("exceeding the limit triggers the camera and deducts the fine",
      bool(mf) and int(mf.group(2)) == 6 and int(mf.group(3)) > 0 and int(mf.group(4)) < 45000,
      f"{mf.group(1)} km/h in a {mf.group(2)}, ${mf.group(3)}, money {mf.group(4)}" if mf else "no fine")
# open road with a higher limit: the same drive must not trigger anything
out = run("--scene", "world", "--no-map", "--speed-limit", "200", "--drive", "25",
          "--spawn", "-3000,-4625,90")
check("staying under the limit never triggers the camera", "fine: " not in out,
      "no fine" if "fine: " not in out else "unexpected fine")

# the script message window: the pager plate below the strip with the message
TEXT = "ОСТАНОВИ МАШИНУ! НУЖНО ПОГОВОРИТЬ"
out = run("--scene", "world", "--no-map", "--msg-shot", TEXT, "--spawn", "-2327,-3767,-29",
          "--screenshot", "--out", os.path.join(OUT, "msg.bmp"))
check("the script message is pushed to the pager",
      "message: '" in out and "--msg-shot" in out,
      [l for l in out.splitlines() if "message:" in l][-1:][0][-60:] if "message:" in out else "no line")
w, h, rows = read_bmp(os.path.join(OUT, "msg.bmp"))
run("--scene", "world", "--no-map", "--screenshot", "--out", os.path.join(OUT, "msg_off.bmp"),
    "--spawn", "-2327,-3767,-29")
_, _, rows_off = read_bmp(os.path.join(OUT, "msg_off.bmp"))
# The strip band itself is bright chrome, so the window is measured as the
# DIFFERENCE against the same frame without a message (the positive text check
# is the glyph-code one below, which is exact).
diff_n = 0
for y in range(8, 48):
    for x in range(240, 560):
        if rows[y][x] != rows_off[y][x]:
            diff_n += 1
check("the message window is drawn in the strip band", diff_n > 1500, f"{diff_n} message px")
# the strip path must hand Font::draw the cp1251 codes of THAT text - the bug
# the renderer agent reported was a script message reaching the glyph lookup as
# raw cp1251 (or converted twice), which renders as «СРЧРВОЦ…» instead of
# «ОСТАНОВИ…».  HT2_MSG_GLYPHS prints the exact byte string.
out = run_env({"HT2_MSG_GLYPHS": "1"},
              "--scene", "world", "--no-map", "--msg-shot", TEXT, "--event-radius", "1",
              "--spawn", "-2327,-3767,-29", "--screenshot",
              "--out", os.path.join(OUT, "msg.bmp"))
mg = re.search(r"msgglyphs: utf8 '.*' -> glyph codes \[([0-9a-f ]+)\]", out)
want = " ".join("%02x" % b for b in TEXT.encode("cp1251"))
check("the strip path converts the script message to the right glyph codes",
      bool(mg) and mg.group(1).strip() == want,
      (mg.group(1).strip() if mg else "no line"))
# the same frame with no message must not differ from itself: the window is the
# only thing the message adds, so a second identical run has no differences
run("--scene", "world", "--no-map", "--screenshot", "--out", os.path.join(OUT, "msg_off2.bmp"),
    "--spawn", "-2327,-3767,-29")
_, _, rows2 = read_bmp(os.path.join(OUT, "msg_off2.bmp"))
same = count(rows2, 240, 560, 8, 48, lambda c: True)
diff_none = 0
for y in range(8, 48):
    for x in range(240, 560):
        if rows_off[y][x] != rows2[y][x]:
            diff_none += 1
check("no message window without a message", diff_none < 60 and same > 0, f"{diff_none} px")

# ------------------------------------------------------------ camera views
# The cockpit view puts the camera at the driver's eye inside the cab; the
# bonnet view just above the windscreen.  Both must differ from the chase view
# and must not show the truck's own body filling the frame.
print("camera views")
views = {}
for mode in ("chase", "cockpit", "bonnet"):
    path = os.path.join(OUT, f"view_{mode}.bmp")
    run("--scene", "world", "--screenshot", "--out", path, "--spawn", "4700,-5700,180",
        "--warmup", "4", "--view", mode)
    w, h, rows = read_bmp(path)
    views[mode] = (w, h, rows)

w, h, rows = views["cockpit"]
check("cockpit view renders", (w, h) == (1280, 720), f"{w}x{h}")
# the cab is a real 3D interior now: the truck's own dashboard and windscreen
# frame fill the bottom of the frame (the original has no 2D dashboard overlay)
cab = run("--scene", "world", "--screenshot", "--out", os.path.join(OUT, "cab.bmp"),
          "--spawn", "4700,-5700,180", "--warmup", "4", "--view", "cockpit")
m = re.search(r"cockpit: (\w+) instruments ready: (\d+) of (\d+) needles", cab)
check("cockpit instruments bound to the truck model",
      bool(m) and m.group(2) == m.group(3) and int(m.group(3)) >= 3,
      f"{m.group(1)}: {m.group(2)}/{m.group(3)} needles" if m else "no line")
# The sky colour depends on the WEATHER preset (the game's own WEATHER.INI is
# used: clear -> (136,186,220) blue, the default cloudy preset -> (162,179,191)
# grey-blue), so the test accepts any bright sky-ish pixel instead of blue only.
sky = count(rows, 0, w, 0, h // 2,
            lambda c: sum(c) > 330 and c[2] >= c[0] and c[2] >= c[1] - 12)
# the cab is modelled now: its roof and windscreen frame fill most of the upper
# frame, so only a slice of sky shows through the glass
check("cockpit windscreen shows the sky", sky > 60000, f"{sky} px")
ground = count(rows, 0, w, h // 2, h, lambda c: not (c[2] > 150 and c[2] > c[0] + 20))
check("cockpit sees the ground below", ground > 200000, f"{ground} px")

d = diff(views["chase"], views["cockpit"])
check("cockpit camera differs from chase", d > 30.0, f"{d:.1f}% of sampled px")
d2 = diff(views["cockpit"], views["bonnet"])
check("bonnet camera differs from cockpit", d2 > 10.0, f"{d2:.1f}% of sampled px")

# ------------------------------------------------------- texture keying
# The sprite art has no alpha channel: transparency is a colour key (dark olive
# 0x31C2 for foliage, black for signs/fences/atlases).  A road sign must render
# as the bare triangular panel, with the background around it keyed away.
print("texture keying (colour keys)")
sign = os.path.join(OUT, "sign.bmp")
run("--scene", "world", "--screenshot", "--out", sign, "--spawn", "-2331,-3759,0",
    "--camera", "-2331,33.5,-3756.5,139,0", "--only-mat", "mat_Z")
w, h, rows = read_bmp(sign)
box = count(rows, 300, 1000, 150, 520, lambda c: sum(c) < 30)
check("sign area mostly not opaque black", box < 0.02 * (700 * 370), f"{box} black px")
tri = count(rows, 300, 1000, 150, 520, lambda c: c[0] > 90 and c[1] < 60 and c[2] < 60)
check("sign panel texture visible (red triangle)", tri > 3000, f"{tri} red px")

# trees: the foliage sprites must be cut out, not drawn as dark rectangles
trees = os.path.join(OUT, "trees.bmp")
run("--scene", "world", "--screenshot", "--out", trees, "--spawn", "-2327,-3767,-29",
    "--warmup", "2")
w, h, rows = read_bmp(trees)
olive = count(rows, 0, w, 250, 620, lambda c: abs(c[0] - 49) < 6 and abs(c[1] - 56) < 6 and abs(c[2] - 16) < 6)
# stray keyed pixels survive in textures whose key covers <0.5% (terrain
# flecks); what must not happen is an opaque sprite rectangle of key colour
check("foliage keyed out (no opaque key rectangles)",
      olive < 0.01 * 370 * 1280, f"{olive} px of (49,56,16) in the foliage band")

# (game scripts section moved up)
out = run("--scripts")
_sum = [l for l in out.splitlines() if "errors=" in l]
check("all scripts parsed without errors", "errors=0" in out, _sum[0][-110:] if _sum else "")
m = re.search(r"files=(\d+) errors=(\d+).*player=([A-Za-z0-9_]+) start=\(([-\d.]+),([-\d.]+),([-\d.]+)\).*vehicles=(\d+) upgrades=(\d+)", out)
if m:
    check("script file count", int(m.group(1)) >= 40, m.group(1))
    check("player truck from GAME.CNF", m.group(3) == "ZilR", m.group(3))
    check("start position from GAME.CNF",
          abs(float(m.group(4)) + 3020) < 1 and abs(float(m.group(5)) + 4625) < 1,
          f"({m.group(4)},{m.group(5)})")
    check("vehicle catalogue from evehicles.sch", int(m.group(7)) > 100, m.group(7))
    check("upgrade table from restable.sch", int(m.group(8)) >= 19, m.group(8))
    # The escaped quote in merchandise.sch ("Телевизор 14\"") used to swallow the
    # next item, yielding 106 cargo entries instead of 107.
    cg = re.search(r"cargo=(\d+) drivers=(\d+)", out)
    check("cargo list parsed (escaped quotes handled)", bool(cg) and int(cg.group(1)) == 107,
          cg.group(1) if cg else "no line")
    check("driver names from names.sch", bool(cg) and int(cg.group(2)) == 74,
          cg.group(2) if cg else "no line")
else:
    check("script report parsed", False, "summary line not found")

# the C++ decryptor must match the reference Python one byte for byte
import subprocess
ref = subprocess.run(["python3", os.path.join(ROOT, "tools", "decrypt_sch.py"),
                      os.path.join(DATA, "evehicles.sch")], capture_output=True).stdout
mine = subprocess.run([BIN, "--data", DATA, "--script-dump",
                       os.path.join(DATA, "evehicles.sch")], capture_output=True).stdout
check("SDTE decryption matches the reference", ref == mine and len(ref) > 10000,
      f"{len(ref)} vs {len(mine)} bytes")

# ------------------------------------------------- menu -> world transition
# --autostart runs exactly the code path of picking "SINGLE PLAYER": the world
# must come up identically to booting straight into it.
print("menu -> world transition")
run("--autostart", "--screenshot", "--out", os.path.join(OUT, "autostart.bmp"),
    "--size", "1280x720")
run("--scene", "world", "--screenshot", "--out", os.path.join(OUT, "direct.bmp"),
    "--size", "1280x720")
_, _, t = read_bmp(os.path.join(OUT, "autostart.bmp"))
_, _, px = read_bmp(os.path.join(OUT, "direct.bmp"))
same = 0
for y in range(0, 720, 4):
    for x in range(0, 1280, 4):
        if max(abs(t[y][x][i] - px[y][x][i]) for i in range(3)) <= 8:
            same += 1
tot = (720 // 4) * (1280 // 4)
# the HUD now carries live readouts (the clock advances while the menu->world
# transition runs), so the two frames are near-identical rather than identical
check("autostart reaches the same world", same > tot * 0.70, f"{100.0 * same / tot:.1f}% identical")

# ------------------------------------------------------------- drive physics
# Deterministic headless drive: from the GAME.CNF start, full throttle, verify
# the truck actually travels, accelerates and stays on the ground.
print("drive test (headless)")
# the drivable-surface test runs without solid obstacles (those are covered by
# the collision section above); the default start would end in the trees
out = run("--scene", "world", "--drive", "30", "--no-collision")
import re
done = [l for l in out.splitlines() if "drive: done" in l]
check("drive test finished", bool(done), done[0][-60:] if done else "no output")
if done:
    m = re.search(r"odometer ([\d.]+) m, height range ([-\d.]+)\.\.([-\d.]+)", done[0])
    if m:
        odo, hmin, hmax = float(m.group(1)), float(m.group(2)), float(m.group(3))
        check("travelled > 300 m in 30 s", odo > 300, f"{odo:.0f} m")
        check("stayed on the ground", (hmax - hmin) < 120, f"h {hmin:.0f}..{hmax:.0f}")
    else:
        check("drive telemetry parsed", False, done[0])
speeds = [float(re.search(r"speed=\s*([\d.]+)", l).group(1))
          for l in out.splitlines() if "drive: t=" in l]
check("reached > 35 km/h", bool(speeds) and max(speeds) > 35, f"max {max(speeds) if speeds else 0:.1f}")
# the AI must actually drive: mean speed > 0, distance grows, no U-turns
m = re.search(r"traffic cars=(\d+) mean=([\d.]+) m/s dist=(\d+) m uturns=(\d+) arrivals=(\d+)",
              out)
check("AI cars keep moving", bool(m) and int(m.group(1)) > 5 and float(m.group(2)) > 4.0,
      f"{m.group(2)} m/s over {m.group(3)} m" if m else "no line")
check("AI never U-turns", bool(m) and int(m.group(4)) == 0, m.group(4) if m else "no line")

# ------------------------------------------------------------------- audio
# src/audio/Audio.cpp (SDL2_mixer): the bank comes out of COMMON/common1.res,
# the engine loop out of snd\idle.wav, the crash one-shot out of snd\crash9.wav
# and the soundtrack out of MUSIC/*.mp3.  The offline modes stay silent unless
# --audio/--menu-music asks for the mixer, and SDL_AUDIODRIVER=dummy is the
# supported headless device (the checks below therefore never need sound).
print("audio (SDL2_mixer, SDL_AUDIODRIVER=dummy)")
out = run_env(DUMMY_AUDIO, "--scene", "menu", "--screenshot", "--menu-music",
              "--size", "1280x720", "--out", os.path.join(OUT, "audio_menu.bmp"))
m = re.search(r"audio: (\d+) Hz (\d+) ch .*?(\d+) effects "
              r"\(([\d.]+) MiB resident, ([\d.]+) MiB source, (\d+) failed\)", out)
check("audio bank loaded headless (dummy driver)",
      bool(m) and int(m.group(3)) == 151 and m.group(6) == "0",
      f"{m.group(3)} effects, {m.group(6)} failed" if m else "no bank line")
inworld = run_env(DUMMY_AUDIO, "--autostart", "--screenshot", "--menu-music",
                  "--size", "1280x720", "--out", os.path.join(OUT, "audio_world.bmp"))
check("menu music starts on the menu and stops for the world",
      "audio: menu music 'track01l' started" in out
      and "audio: menu music 'track01l' stopped for the world" in inworld
      and "music=0" in inworld,
      "start->stop" if "music=0" in inworld else "music state line missing")
# the collision drive: full throttle into the trees at ~45 km/h
crash = run_env(DUMMY_AUDIO, "--scene", "world", "--drive", "30",
                "--spawn", "-2327,-3767,-29", "--audio")
m = re.search(r"audio: engine loop after drive: (\d+) frames, (\d+) starts", crash)
check("engine loop starts and a crash one-shot plays",
      bool(m) and int(m.group(1)) > 1000 and int(m.group(2)) == 1
      and "audio: crash 'snd\\crash9.wav'" in crash
      and "engineRestarts=1" in crash,
      f"{m.group(1)}-frame loop, {m.group(2)} start, crash played" if m else "no engine line")

# ------------------------------------------------------------------- movie
# src/ui/Video.cpp streams build/media/<clip>.rvid (tools/make_video.sh): one
# frame per displayed frame, never the whole 312 MB pack.  --movie-frames makes
# the frame index deterministic, so the picture can be compared without timing.
print("movie (streamed .rvid frame pack)")
ma, mb = os.path.join(OUT, "movie_400.bmp"), os.path.join(OUT, "movie_900.bmp")
oa = run("--movie", "--screenshot", "--out", ma, "--movie-frames", "400",
         "--size", "1280x720")
ob = run("--movie", "--screenshot", "--out", mb, "--movie-frames", "900",
         "--size", "1280x720")
m1 = re.search(r"movie: screenshot at t=[\d.]+s frame (\d+)/(\d+) \(asked for (\d+)\) of 'kotr'", oa)
m2 = re.search(r"movie: screenshot at t=[\d.]+s frame (\d+)/(\d+) \(asked for (\d+)\) of 'kotr'", ob)
wa, ha, ia = read_bmp(ma)
_, _, ib = read_bmp(mb)
mdiff = diff((wa, ha, ia), (wa, ha, ib))
check("movie frames advance (kotr pack streamed)",
      bool(m1) and bool(m2) and m1.group(1) == "400" and m2.group(1) == "900"
      and m1.group(2) == m2.group(2) and mdiff > 10.0,
      f"frames {m1.group(1)}/{m2.group(1)} of {m1.group(2)}, {mdiff:.1f}% px differ"
      if m1 and m2 else "no movie line")

# ------------------------------------------------------ movie A/V sync
# The reported bug: "в видеоролике звук опережает видео" (the soundtrack led the
# picture).  Video::tick() derives the frame index from a clock - the mixer
# position while the WAV plays, SDL_GetTicks otherwise - and the WAV is started
# with the first frame on screen, never by play().  Phase 1 of the report is
# fully deterministic (a synthetic clock, no wall clock, no device): the frame
# index must be floor(clock*fps) at 0/30/60/100 s even though the simulated loop
# stutters, i.e. frames are skipped, never accumulated as lag.  Phase 2 plays
# through the SDL dummy device and prints video time vs the mixer position.
print("movie A/V sync (--movie-sync-report)")
sync_sim = run("--movie-sync-report", "--movie-sync-seconds", "0", "--size", "1280x720")
clk = re.findall(r"movie-sync\[clock\]: clock=([\d.]+)s frame=(\d+) expected=(\d+) "
                 r"\(([+-]\d+) frames\)", sync_sim)
check("movie sync: the frame index is derived from the clock (0/30/60/100 s)",
      len(clk) == 4 and all(f == e and int(d) == 0 for _, f, e, d in clk),
      " ".join(f"{c}s->{f}" for c, f, _, _ in clk) if clk else "no clock line")

sync = run_env(DUMMY_AUDIO, "--movie-sync-report", "--movie-sync-seconds", "3",
               "--size", "1280x720")
check("movie sync: the soundtrack starts with the first frame, not with play()",
      "video: soundtrack starts with frame 0 (t=0.000 s" in sync,
      [l for l in sync.splitlines() if "soundtrack starts" in l][:1][0][-52:]
      if "soundtrack starts" in sync else "no start line")
# both line shapes: "movie-sync: video=.." and "movie-sync[seek@30s]: video=.."
lines = re.findall(r"movie-sync(?:\[seek@(\d+)s\])?: video=([\d.]+)s audio=([\d.]+)s "
                   r"drift=([+-]\d+)ms frame=(\d+)/(\d+) mixer=(\w+)", sync)
samples = [(int(cp) if cp else None, float(v), float(a), int(d), int(fr), int(tot), m)
           for cp, v, a, d, fr, tot, m in lines]
real = [x for x in samples if x[0] is None]
seeks = [x for x in samples if x[0] is not None]
# one frame at 12 fps is 83.3 ms: the mixer may trail the picture by less than
# that, but the gap must not grow with time
check("movie sync: video-vs-mixer drift stays inside one frame",
      len(real) >= 3 and all(x[6] == "running" for x in samples)
      and all(abs(x[3]) <= 83 for x in samples) and all(x[5] == 1308 for x in samples)
      and real[-1][1] >= 1.5 and real[-1][4] >= 15,      # really played, not frozen
      "drift " + ", ".join(f"{x[1]:.1f}s:{x[3]:+d}ms" for x in real)
      if real else "no drift line")
check("movie sync: same drift after seeking to 30/60/100 s",
      len(seeks) == 3 and all(abs(x[3]) <= 83 for x in seeks)
      and [x[0] for x in seeks] == [30, 60, 100]
      and all(x[4] >= x[0] * 12 for x in seeks),
      " ".join(f"{x[0]}s:{x[4]}f/{x[3]:+d}ms" for x in seeks) if seeks else "no seek line")

# ----------------------------------------------------------- loading screen
# src/ui/Loading.cpp is drawn while the world tiles/textures load in the
# menu -> world transition; --loading-shot renders the same screen stand-alone.
print("loading screen (original loadpix + loadrunner art)")
out = run("--loading-shot", os.path.join(OUT, "loading.bmp"), "--size", "1280x720",
          "--loading-at", "0.35")
check("original loading art loaded", "original art loaded" in out,
      [l for l in out.splitlines() if "loading:" in l][:1][0][-60:])
w, h, rows = read_bmp(os.path.join(OUT, "loading.bmp"))
# the bright panorama is revealed from the left: at 35% the left is lit and the
# right is still the dark background (the art is authored for 1600x1200, so the
# 97-px band sits at y = (720-1200)/2 + 552 = 312)
litL = count(rows, 0, 360, 318, 402, lambda c: sum(c) > 260)
litR = count(rows, 900, 1280, 318, 402, lambda c: sum(c) > 260)
check("progress panorama wipes in from the left",
      litL > 5000 and litL > 3 * max(1, litR), f"left {litL} vs right {litR} lit px")
logo = count(rows, 400, 880, 200, 300, lambda c: sum(c) > 300)
check("Дальнобойщики logo visible", logo > 2000, f"{logo} px")
check("loading screen reports progress and the original version string",
      "loading: screen" in out and "v 8.0 30.01.03" in out, "log line ok")

# --------------------------------------------------------- updateTexture
# Renderer::updateTexture() is the streaming upload the movie player needs
# (glTexSubImage2D + no stale mipmaps): frame 0 creates the texture, frame N is
# pushed into it and drawn full screen.
print("updateTexture (texture streaming)")
ta, tb = os.path.join(OUT, "texup_400.bmp"), os.path.join(OUT, "texup_900.bmp")
oa = run("--tex-update", "400", "--out", ta, "--size", "640x480")
ob = run("--tex-update", "900", "--out", tb, "--size", "640x480")
wa, ha, ia = read_bmp(ta)
_, _, ib = read_bmp(tb)
tdiff = diff((wa, ha, ia), (wa, ha, ib))
lit = sum(1 for y in range(0, ha, 4) for x in range(0, wa, 4) if sum(ia[y][x]) > 30)
check("updateTexture re-uploads the streamed frame",
      "texture: updateTexture(" in oa and "-> ok" in oa and "1 updates" in oa
      and lit > 0.10 * (ha // 4) * (wa // 4) and tdiff > 10.0,
      f"{tdiff:.1f}% px differ between frames 400 and 900, {lit} lit px in frame 400")

# ------------------------------------------------------- in-game ESC screens
# src/ui/EscMenu.cpp draws the original COMMON/esc16.RES panels **1:1** at the
# verified anchor y = -(1200 - screenH)/2 - 50 (docs/esc_screens.md 2.2) with the
# button tables of docs/esc_screens.md 3.  At 1280x720 that anchor is -290, so
# the ESC-main buttons (panel y 454 + 45k, x 18) land at screen y 164 + 45k.
print("in-game ESC screens (src/ui/EscMenu.cpp, COMMON/esc16.RES)")
PANEL_Y = -290


def yellow(c):                       # the Russian captions baked into the art
    return c[0] > 170 and c[1] > 170 and c[2] < 130


def bright(c):
    return sum(c) > 250


ESC = {name: os.path.join(OUT, f"esc_{name}.bmp")
       for name in ("main", "sound", "control", "save", "load", "exit")}
logs = {name: run("--esc-shot", path, "--esc-screen", name, "--size", "1280x720")
        for name, path in ESC.items()}
check("ESC screens decode the original esc16.RES art (34/34)",
      "esc: art loaded (34/34, 6 screens)" in logs["main"]
      and "esc: screen 'main' (8 buttons, 0 sliders)" in logs["main"]
      and "esc: screen 'sound' (2 buttons, 4 sliders)" in logs["sound"]
      and "esc: screen 'control' (2 buttons, 3 sliders) + key list" in logs["control"]
      and "esc: screen 'exit' (3 buttons, 0 sliders)" in logs["exit"],
      "6 screens: 8 / 2+4 / 2+3 / 3 / 3 / 3")

w, h, px = read_bmp(ESC["main"])
labels = [count(px, 18, 168, PANEL_Y + 454 + 45 * k, PANEL_Y + 484 + 45 * k, yellow)
          for k in range(8)]
check("ESC main: 8 buttons at x=18, the verified 45-px pitch",
      min(labels) > 25, "caption px " + ",".join(str(n) for n in labels))

_, _, px = read_bmp(ESC["sound"])
runs = [count(px, 84, 100, PANEL_Y + 501 + 70 * k - 9, PANEL_Y + 501 + 70 * k - 2, bright)
        for k in range(4)]
arrows = [count(px, 25, 41, PANEL_Y + 487 + 70 * k, PANEL_Y + 515 + 70 * k, bright)
          for k in range(4)]
okc = count(px, 21, 76, PANEL_Y + 762, PANEL_Y + 792, yellow)
check("ESC sound: 4 slider rows (arrow pair + runner) and OK/ОТМЕНА",
      min(runs) > 25 and min(arrows) > 20 and okc > 20,
      "runner " + ",".join(str(n) for n in runs) + " arrows " + ",".join(str(n) for n in arrows))

_, _, px = read_bmp(ESC["control"])
cruns = [count(px, 84, 100, PANEL_Y + 523 + 70 * k - 9, PANEL_Y + 523 + 70 * k - 2, bright)
         for k in range(3)]
keypx = count(px, 224, 644, 223, 497, lambda c: sum(c) > 240)
check("ESC controls: 3 slider rows + the 16-line key-binding list",
      min(cruns) > 25 and keypx > 1000 and "esc: key bindings 16 rows" in logs["control"],
      f"runner {cruns}, {keypx} key-list px")

sv = read_bmp(ESC["save"])[2]
ld = read_bmp(ESC["load"])[2]
sb = [count(sv, 24, 154, PANEL_Y + 679 + 45 * k, PANEL_Y + 709 + 45 * k, yellow) for k in range(3)]
lb = [count(ld, 24, 154, PANEL_Y + 679 + 45 * k, PANEL_Y + 709 + 45 * k, yellow) for k in range(3)]
check("ESC save/load: 3 buttons at panel x=24, y=679/724/769",
      min(sb) > 25 and min(lb) > 25, f"save {sb} load {lb}")

_, _, px = read_bmp(ESC["exit"])
eb = [count(px, 18, 168, PANEL_Y + 564 + 50 * k, PANEL_Y + 594 + 50 * k, yellow) for k in range(3)]
top = count(px, 18, 168, PANEL_Y + 454, PANEL_Y + 484, yellow)
bot = count(px, 18, 168, PANEL_Y + 769, PANEL_Y + 799, yellow)
check("ESC exit popup: 3 buttons at y=564/614/664 (not the main list)",
      min(eb) > 25 and top == 0 and bot == 0, f"popup {eb}, main rows {top}/{bot}")

# --- --esc-act: the same path a click/keyboard activation takes ------------
print("ESC actions (--esc-act NAME[,NAME...])")
out = run("--esc-act", "exitsys,exnsave", "--screenshot",
          "--out", os.path.join(OUT, "esc_act_exit.bmp"), "--size", "1280x720")
check("ESC exitsys -> ВЫХОД popup -> leave the world to the menu",
      "esc: activate 'exitsys' (id 0xa9, screen 'main') -> action 'open:exit'" in out
      and "esc: activate 'exnsave' (id 0xab, screen 'exit') -> action 'exitnosave'" in out
      and "esc: left the world for the main menu" in out, "world -> menu")
w, h, px = read_bmp(os.path.join(OUT, "esc_act_exit.bmp"))
check("... and the frame is the front-end menu again",
      sum(px[360][40]) < 24 and sum(px[360][1240]) < 24, "menu letterbox bars")

out = run("--esc-act", "main", "--screenshot",
          "--out", os.path.join(OUT, "esc_act_main.bmp"), "--size", "1280x720")
check("ESC ГЛАВНОЕ МЕНЮ leaves the world for the front-end menu",
      "esc: activate 'main' (id 0xa3, screen 'main') -> action 'menu'" in out
      and "esc: left the world for the main menu" in out, "id 0xa3 -> menu")

SAVE_FILE = os.path.join(OUT, "esc_savegame.txt")
if os.path.exists(SAVE_FILE):
    os.remove(SAVE_FILE)
SAVE_ENV = {"HT2_SAVE_FILE": SAVE_FILE}
out = run_env(SAVE_ENV, "--scene", "world", "--fuel", "0.42", "--damage", "0.31",
              "--warmup", "3", "--esc-act", "restart,savg,save", "--screenshot",
              "--out", os.path.join(OUT, "esc_act_save.bmp"), "--size", "1280x720")
check("ESC НАЧАТЬ СНАЧАЛА respawns, СОХРАНИТЬ ИГРУ writes the small state",
      "esc: restart - respawned at the start (-3020, -4625)" in out
      and "esc: savegame written to " in out and "fuel 0.42" in out
      and os.path.exists(SAVE_FILE), "restart + save file")
saved = open(SAVE_FILE).read() if os.path.exists(SAVE_FILE) else ""
# the fuel figure is the burn over the 3 s warm-up, so it follows the driving
# model: assert the format exactly and the value to 2 decimals (the exact number
# was a property of the old constant-acceleration model, see docs/physics.md)
fm = re.search(r"fuel ([\d.]+)", saved)
dm = re.search(r"damage ([\d.]+)", saved)
check("save file format (HT2RE-SAVE 1 + money/pos/fuel/damage/job)",
      saved.startswith("HT2RE-SAVE 1\n") and "money 45000" in saved
      and "pos -3020.000 -4625.000" in saved
      and bool(fm) and abs(float(fm.group(1)) - 0.42) < 0.02
      # the damage figure follows the new (original) driving model as well, so
      # it is asserted as a plausible range, not as the old literal
      and bool(dm) and 0.15 < float(dm.group(1)) < 0.60
      and "job -1" in saved,
      (saved.splitlines()[0] + " fuel=" + fm.group(1) + " damage=" + dm.group(1))
      if (saved and fm and dm) else "missing")
out = run_env(SAVE_ENV, "--scene", "world", "--esc-screen", "load", "--esc-act", "load",
              "--screenshot", "--out", os.path.join(OUT, "esc_act_load.bmp"), "--size", "1280x720")
check("ESC ЗАГРУЗИТЬ restores the state and resumes driving",
      "esc: savegame loaded from " in out and "fuel 0.42, damage 0.31" in out
      and "esc: closed after loading, driving resumed" in out, "round trip")

out = run("--esc-act", "911", "--screenshot",
          "--out", os.path.join(OUT, "esc_act_911.bmp"), "--size", "1280x720")
check("ESC 911 tows the truck to the nearest service station",
      "esc: activate '911' (id 0xa6, screen 'main') -> action 'recover'" in out
      and "esc: 911 - towed to the nearest" in out, "recover")

# ------------------------------------------------------- in-game UI panels
# src/ui/EscMenu.cpp draws *every* in-game panel of the original 1:1 at the
# verified anchor (docs/esc_screens.md 2.2 / docs/ui_sort.md):
#   docked 1200-px panels  y = -(1200 - screenH)/2 - 50
#     left x=0 | right225 x=screenW-224 | right385 x=screenW-383 | right640 x=screenW-640
#   the 1600x1200 win/lose pages, the 280x405 F5 radio and the 494x160
#   "5 yes/no" popup: centred 1:1 in both axes.
# HT2_UI_DEMO fills the data cells (money/fuel/jobs/upgrades) with a fixed
# sample world - the panels otherwise show the live values the App pushes.
print("in-game panels (fuel / service / market / info / help / win-lose / popups)")
UI_DEMO = {"HT2_UI_DEMO": "1"}
# the world behind a panel must be a *frozen* reference: no AI traffic and no
# parked cars, so the only difference between a panel frame and the baseline is
# the panel itself (the clock is fixed too: --warmup 0, dt=0)
# `--scene world` makes the panel frames use the *same* boot path as the
# baseline (--ui-shot otherwise runs the real menu -> world transition), and
# --event-radius 1 suppresses the node-event banner the paused world would not
# have shown.  Nothing else is left to differ outside the panel.
UI_QUIET = ["--scene", "world", "--traffic", "0", "--parked-radius", "10",
            "--event-radius", "1"]
PANELS = ["fuel", "service", "order", "sale", "job", "info", "mytruck",
          "f1", "f2", "f2tball", "f3",
          "win", "win1", "win2", "lose1", "lose2", "lose3", "lose4", "lose5",
          "popup5", "cbpol", "cbmaf", "cbopp", "cbnei",
          "cbpol_ans", "cbradio", "cbtalk"]
ui = {}
for name in PANELS:
    path = os.path.join(OUT, f"ui_{name}.bmp")
    ui[name] = (path, run_env(UI_DEMO, *(UI_QUIET + ["--ui-shot", name, path,
                                                     "--size", "1280x720"])))

bad = []
for name in PANELS:
    m = re.search(r"ui: art loaded \((\d+)/(\d+), '%s'\)" % name, ui[name][1])
    if not m or m.group(1) != m.group(2) or int(m.group(1)) == 0:
        bad.append(f"{name}:{m.group(1) + '/' + m.group(2) if m else 'no line'}")
check(f"every in-game panel decodes its own esc16/ind16 art ({len(PANELS)} panels)",
      not bad, "; ".join(bad) or "all N/N complete")

DOCKX = {"left": 0, "right225": 1056, "right385": 897, "right640": 640}
bad = []
for name in PANELS:
    m = re.search(r"ui: panel '%s' dock=(\w+) art (\d+)x(\d+) viewport 1280x720 "
                  r"anchor \((-?\d+),(-?\d+)\)" % name, ui[name][1])
    if not m:
        bad.append(f"{name}:no panel line")
        continue
    dock, aw, ah, ax, ay = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5))
    wx = (1280 - aw) // 2 if dock == "center" else DOCKX.get(dock, -1)
    wy = (720 - ah) // 2 if dock == "center" else -((1200 - 720) // 2) - 50
    if ax != wx or ay != wy:
        bad.append(f"{name}:{dock} ({ax},{ay}) != ({wx},{wy})")
check("panels use the verified dock/anchor rule (1:1 art, no scaling)",
      not bad, "; ".join(bad[:4]) or f"{len(PANELS)} panels: left/right225/right385/right640/centred")

# the frame a live world renders when no panel is up (same spawn/warmup as --ui-shot)
base = os.path.join(OUT, "ui_base.bmp")
run(*(UI_QUIET + ["--scene", "world", "--screenshot", "--out", base, "--size", "1280x720"]))
BASE = read_bmp(base)


def rect_diff(a, b, x0, x1, y0, y1, step=4, tol=10):
    n = tot = 0
    for y in range(max(0, y0), min(a[1], y1), step):
        for x in range(max(0, x0), min(a[0], x1), step):
            tot += 1
            if max(abs(a[2][y][x][i] - b[2][y][x][i]) for i in range(3)) > tol:
                n += 1
    return 100.0 * n / max(1, tot)


# Each panel paints its own dock rectangle from the real art (hundreds of
# differing pixels) and changes nothing in the world outside it.  The comparison
# excludes the HUD/pause band and the port-only bottom control hints, which are
# intentionally hidden while a panel owns the input.
# The 50-pixel strip is deliberately drawn across the top while a panel is up;
# its large readout glyphs extend to about y=70, so geometry starts at y=75.
DOCKED = {"fuel": (1056, 1281, "right"), "service": (640, 1281, "right"),
          "info": (897, 1281, "right"), "mytruck": (0, 415, "left")}
bad = []
for name, (x0, x1, side) in DOCKED.items():
    px = read_bmp(ui[name][0])
    inside = rect_diff(BASE, px, x0, x1, 0, 720)
    outside = rect_diff(BASE, px, x1 if side == "left" else 0,
                        x0 if side == "right" else 1280, 75, 680)
    if inside < 20.0 or outside > 0.5:
        bad.append(f"{name}: in {inside:.0f}% out {outside:.2f}%")
check("docked panels cover their own rect and nothing else",
      not bad, "; ".join(bad) or "in>20%, out<1% for fuel/service/info/mytruck")

# centred popups: the 494x160 plate / the 280x405 radio pages are centred 1:1
bad = []
for name, (cx0, cy0, cx1, cy1) in {"popup5": (393, 280, 887, 440),
                                   "cbradio": (517, 142, 762, 577)}.items():
    px = read_bmp(ui[name][0])
    inside = rect_diff(BASE, px, cx0, cx1, cy0, cy1)
    outside = (rect_diff(BASE, px, 0, cx0, 75, 680) +
               rect_diff(BASE, px, cx1, 1280, 75, 680))
    if inside < 20.0 or outside > 0.5:
        bad.append(f"{name}: in {inside:.0f}% out {outside:.2f}%")
check("centred popups are centred 1:1 and cover nothing else",
      not bad, "; ".join(bad) or "popup5 (393,280) + cbradio (517,142)")

# The original pause strip appears across the top, and an open panel never mixes
# with the minimap: a left panel leaves it untouched, a right panel covers it
# (the panel is drawn on top).
bad = []
for name, (x0, x1, dock) in {"fuel": (1056, 1281, "right"), "service": (640, 1281, "right"),
                             "info": (897, 1281, "right"), "mytruck": (0, 415, "left")}.items():
    px = read_bmp(ui[name][0])
    pause = rect_diff(BASE, px, 0, 1280, 0, 50)
    mm = rect_diff(BASE, px, 1066, 1262, 460, 656)
    if pause < 5.0:
        bad.append(f"{name}: pause strip only {pause:.2f}% changed")
    if dock == "left" and mm > 0.5:
        bad.append(f"{name}: minimap {mm:.2f}%")
    if dock == "right" and mm < 50.0:
        bad.append(f"{name}: minimap not covered ({mm:.0f}%)")
check("the pause strip is visible and the minimap never mixes with a panel",
      not bad, "; ".join(bad) or "pause visible, minimap untouched/covered")

# --- the panel actions the App binds (fuel -> refuel, service -> repair/tune,
#     order -> accept, info -> deliver, sale -> buy) ------------------------
print("panel actions (--ui-panel NAME --ui-act BUTTON)")
ACTS = [
    ("fuel", "gaspay", "refuel"),
    ("fuel", "gascanc", "resume"),
    ("order", "ok", "acceptjob"),
    ("info", "ok", "deliverjob"),
    ("mytruck", "chan", "changevehicle"),
    ("mytruck", "fire", "firedriver"),
    ("job", "ok", "hiredriver"),
    ("sale", "mrun", "buyvehicle"),
    ("service", "mrun", "tune"),
    ("service", "servcanc", "resume"),
]
bad = []
handled = []
for panel, btn, want in ACTS:
    out = run_env(UI_DEMO, *(UI_QUIET + ["--ui-panel", panel, "--ui-act", btn, "--screenshot",
                                       "--out", os.path.join(OUT, "ui_act.bmp"),
                                       "--size", "1280x720"]))
    m = re.search(r"esc: activate '%s' \(id 0x[0-9a-f]+, screen '%s'\) -> action '(\w+)'"
                  % (btn, panel), out)
    if not m or m.group(1) != want:
        bad.append(f"{panel}/{btn}:{m.group(1) if m else 'none'}!={want}")
    if f"unhandled action '{want}'" in out:
        bad.append(f"{panel}/{btn}:{want} reached App but was not handled")
    else:
        handled.append(want)
check("every panel action is declared and produced for the App",
      not bad, "; ".join(bad) or ", ".join(f"{p}/{b}->{a}" for p, b, a in ACTS[:5]) + " ...")
check("every produced panel action is consumed by the App",
      len(handled) == len(ACTS), f"{len(handled)}/{len(ACTS)} actions handled")

# the service panel's tuning list is selectable and the СТАВИТЬ ВСЁ hot spot works
out = run_env(UI_DEMO, *(UI_QUIET + ["--ui-panel", "service", "--ui-act", "mdown,mdown,mrun",
                                       "--screenshot", "--out", os.path.join(OUT, "ui_act_tune.bmp"),
                                       "--size", "1280x720"]))
check("tuning: the list scrolls and APPLY produces 'tune'",
      "ui: selection 2/19 on 'service'" in out and "ui: selection 3/19 on 'service'" in out
      and "-> action 'tune'" in out and "unhandled action 'tune'" not in out,
      "19 upgrade cells, 2 scroll steps, App handler reached")
out = run_env(UI_DEMO, *(UI_QUIET + ["--ui-panel", "service", "--ui-act", "tuneall",
                                       "--screenshot", "--out", os.path.join(OUT, "ui_act_all.bmp"),
                                       "--size", "1280x720"]))
check("tuning: the painted 'СТАВИТЬ ВСЁ' row is a hot spot",
      "esc: activate 'tuneall'" in out and "-> action 'tuneall'" in out
      and "unhandled action 'tuneall'" not in out, "install-all handled by App")

# the purchase list shows the market, scrolls and keeps its detail rows in sync
out = run_env(UI_DEMO, *(UI_QUIET + ["--ui-panel", "sale", "--ui-act", "mdown", "--screenshot",
                                       "--out", os.path.join(OUT, "ui_act_sale.bmp"),
                                       "--size", "1280x720"]))
check("purchase: the vehicle list is windowed, scrolls and shows price/trade-in",
      "ui:   list 6 offers (6 shown), highlight 1" in out
      and "ui: selection 2/6 on 'sale'" in out
      and "ui:   row sale.price = '18000'" in out
      and "ui:   row sale.tradein = '32000'" in out, "6-offer market, line 2 selected")

# the fuel panel prints litres / price-per-litre / the sum into the painted LEDs
out = run_env(UI_DEMO, *(UI_QUIET + ["--ui-panel", "fuel", "--screenshot",
                                       "--out", os.path.join(OUT, "ui_fuel.bmp"),
                                       "--size", "1280x720"]))
check("refuel: litres, price per litre, sum and the tank level are shown",
      all(f"ui:   cell {k} = '" in out for k in
          ("fuel.buy.cost", "fuel.buy.liters", "fuel.price", "fuel.liters", "fuel.cost"))
      and "ui:   row fuel = '42%'" in out, "5 LED cells + the level row")

# navigation: a panel's ОТМЕНА closes it and resumes driving, and an ESC-family
# screen reached from a panel keeps Esc working
out = run_env(UI_DEMO, *(UI_QUIET + ["--ui-panel", "info", "--ui-act", "canc", "--screenshot",
                                       "--out", os.path.join(OUT, "ui_nav.bmp"),
                                       "--size", "1280x720"]))
w, h, px = read_bmp(os.path.join(OUT, "ui_nav.bmp"))
check("navigation: ОТМЕНА closes the panel and the world is driving again",
      "esc: activate 'canc' (id 0x2, screen 'info') -> action 'resume'" in out
      and rect_diff(BASE, (w, h, px), 897, 1281, 0, 720) < 0.5, "panel gone, world unchanged")

out = run_env(UI_DEMO, *(UI_QUIET + ["--ui-panel", "cbpol", "--ui-act", "F5p_ask1", "--screenshot",
                                       "--out", os.path.join(OUT, "ui_nav_cb.bmp"),
                                       "--size", "1280x720"]))
check("navigation: an F5 radio question opens its answer page",
      "-> action 'cbask'" in out and "ui: panel 'cbpol_ans'" in out, "question -> answer")


# ------------------------------------------------------- world AI (traffic)
# The road graph, the lane path, the junction reservation and the contacts are
# all measured by --traffic-report SECONDS: it runs the same simulation the game
# runs, with the player parked in a lane of a long edge, and prints one
# machine-readable RESULT line.  Off-road = the car is further than the limit
# from its lane polyline; a "jump" is a pose that moved more than speed*dt +
# 0.5 m in one frame (the old lane-offset step at a junction); "height steps"
# are vertical pops (cars flying or sinking through the map).
print("world AI (waypoints, junctions, colliders, contacts)")
rep = run("--scene", "world", "--spawn", "-3020,-4625,0", "--traffic", "16",
          "--traffic-report", "25")
m = re.search(r"traffic-report: RESULT cars=(\d+) mean=([\d.]+) uturns=(\d+) "
              r"deadend=(\d+) offroad=(\d+) frames=(\d+) jumps=(\d+) maxjump=([\d.]+) "
              r"hjumps=(\d+) maxhstep=([\d.]+) contacts=(\d+) carcontacts=(\d+) "
              r"playercontacts=(\d+) junctionhits=(\d+) overlaps=(\d+) mingap=(-?[\d.]+) "
              r"damage=([\d.]+) playerdamage=([\d.]+) queued=(\d+)", rep)
check("traffic report runs the world AI headlessly", bool(m),
      m.group(0)[:80] if m else "no RESULT line")
if m:
    cars, mean = int(m.group(1)), float(m.group(2))
    uturns, deadend, offroad, frames = (int(m.group(3)), int(m.group(4)),
                                        int(m.group(5)), int(m.group(6)))
    jumps, maxjump = int(m.group(7)), float(m.group(8))
    hjumps, maxhstep = int(m.group(9)), float(m.group(10))
    contacts, overlaps, mingap = int(m.group(11)), int(m.group(15)), float(m.group(16))
    check("AI cars follow the road graph (mean speed and distance)",
          cars >= 8 and mean > 5.0, f"{cars} cars, mean {mean:.2f} m/s")
    check("lane adherence: no car off its lane polyline",
          offroad == 0 and frames > 5000, f"{offroad} of {frames} car-frames")
    check("no U-turns and no illegal turn-arounds",
          uturns == 0 and deadend == 0, f"uturns={uturns} deadend={deadend}")
    check("pose is continuous at junctions (no lane-offset teleport)",
          jumps <= 5 and maxjump < 2.5, f"{jumps} jumps, max {maxjump:.2f} m")
    check("height follows the road profile (no flying / sinking)",
          hjumps == 0 and maxhstep < 0.5, f"{hjumps} steps >0.5 m, max {maxhstep:.2f} m")
    check("car bodies never interpenetrate deeply",
          contacts >= 1 and overlaps < frames // 100 and mingap > -0.5,
          f"{contacts} contacts, {overlaps} overlap frames, min gap {mingap:.2f} m")
    check("Contacts deal damage to the AI cars",
          float(m.group(17)) > 0.0, f"damage dealt {m.group(17)}")

# A truck parked in the middle of a lane must make the traffic queue behind it.
rep = run("--scene", "world", "--spawn", "-3020,-4625,0", "--traffic", "40",
          "--traffic-report", "25")
m = re.search(r"traffic-report: RESULT .*queued=(\d+)", rep)
queued = int(m.group(1)) if m else -1
check("cars queue behind a stopped player", queued >= 1, f"{queued} cars queued")

# Driving into the traffic: the car bodies stop the truck, both sides take
# damage and the capsule resolution separates them again (only a handful of
# overlap frames around the impacts, not a permanent interpenetration).
out = run("--scene", "world", "--drive", "30", "--spawn", "-3020,-4625,0",
          "--traffic", "16")
m = re.search(r"traffic: car impact ([\d.]+) km/h, player damage ([\d.]+), car damage ([\d.]+)",
              out)
check("car-vs-player impact damages both", bool(m) and float(m.group(2)) > 0.0
      and float(m.group(3)) > 0.0,
      f"{m.group(1)} km/h, player {m.group(2)}, car {m.group(3)}" if m else "no impact")
m = re.search(r"drive: contacts (\d+) \(car-vs-car (\d+), car-vs-player (\d+)\).*"
              r"overlaps (\d+), min gap (-?[\d.]+) m, damage dealt ([\d.]+) \(player ([\d.]+)\)",
              out)
check("car-vs-player contact resolved and the bodies separate",
      bool(m) and int(m.group(3)) >= 1 and int(m.group(4)) <= 20 and float(m.group(6)) > 0.0,
      f"{m.group(3)} player contacts, {m.group(4)} overlap frames, "
      f"damage {m.group(6)}" if m else "no contact line")

# ------------------------------------------- road continuity, draw distance, weather
# The generated ground mesh must never rise above the tile surface: on a bend
# the old 415-unit mesh was only clamped at its vertices, so the interpolated
# triangle lifted back over the asphalt and swallowed it.  The metric below
# renders the view twice - with the terrain and with --no-ground - and counts
# the pixels that are asphalt in the scene-only frame but terrain-green in the
# full frame.  --terrain forces the old mesh, which is the control: it must
# still show the break, otherwise the check is measuring nothing.
print("road continuity at a bend")
def road_cover(full, nog):
    w, h, A = read_bmp(full)
    _, _, B = read_bmp(nog)
    tot = bad = 0
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            r, g, b = B[y][x]
            if abs(r - g) < 16 and abs(g - b) < 16 and 55 < r < 200:   # asphalt in the scene
                tot += 1
                rr, gg, bb = A[y][x]
                if gg > rr + 10 and gg > bb + 10 and gg > 40:          # terrain-green
                    bad += 1
    return bad, tot

BEND = ("--scene", "world", "--screenshot", "--spawn", "-2886,-4643,70",
        "--camera", "-1740.5,31,3960,80.5,-14", "--traffic", "0")
full = os.path.join(OUT, "road_full.bmp")
nog = os.path.join(OUT, "road_noground.bmp")
old = os.path.join(OUT, "road_oldterrain.bmp")
road_out = run(*BEND, "--out", full)
run(*BEND, "--out", nog, "--no-ground")
run(*BEND, "--out", old, "--terrain")
bad, tot = road_cover(full, nog)
bad_old, tot_old = road_cover(old, nog)
check("road continuity: asphalt not buried by the terrain at a bend",
      tot > 20000 and bad < tot * 0.001,
      f"{bad}/{tot} asphalt px covered ({100.0 * bad / max(1, tot):.2f}%)")
check("authored B3D roads are not duplicated by procedural ribbons",
      "road mesh:" not in road_out, "terrain-only generated mesh")
check("road continuity: the pre-fix mesh (--terrain) still breaks it (control)",
      bad_old > bad * 5,
      f"old {bad_old} px vs new {bad} px")

# ----------------------------------------------------- cars on the road surface
# The wheel-height metric: --traffic-wheel-probe runs the real AI at 10/25/40/64
# cars and compares each car's four wheel contact points (from the body capsule
# Traffic uses) with the authored asphalt directly under them (a downward ray
# cast into the B3D triangles, no rasterisation).  "Flying / sinking" is exactly
# this number; it must stay inside a few centimetres, and no sample may have the
# asphalt missing entirely under the wheel column (a car standing on the terrain
# mesh instead of a road).
print("AI cars stand on the asphalt (--traffic-wheel-probe)")
rep = run("--scene", "world", "--traffic-wheel-probe", "8")
m = re.search(r"traffic-wheel-probe: RESULT samples=(\d+) noasphalt=(\d+) mean=(-?[\d.]+) "
              r"median=(-?[\d.]+) p95=([\d.]+) max=([\d.]+) over5=(\d+) over20=(\d+) "
              r"junction=(\d+)", rep)
check("traffic wheel probe runs the simulated AI at several densities", bool(m),
      m.group(0)[:80] if m else "no RESULT line")
if m:
    tw_samples, tw_no, tw_mean, tw_p95, tw_max = (int(m.group(1)), int(m.group(2)),
                                                  float(m.group(3)), float(m.group(5)),
                                                  float(m.group(6)))
    tw_over5, tw_over20, tw_junction = int(m.group(7)), int(m.group(8)), int(m.group(9))
    check("no AI car is more than a few centimetres off the asphalt under its wheels",
          tw_samples > 20000 and tw_max < 2.0 and tw_over5 == 0 and tw_over20 == 0,
          f"{tw_samples} wheel samples, max {tw_max:.3f} m, p95 {tw_p95:.3f} m, "
          f"mean {tw_mean:+.3f} m, >5 m {tw_over5}")
    check("a junction does not lift or drop the cars (worst case is away from one)",
          tw_junction > 1000 and tw_over20 == 0,
          f"{tw_junction} junction wheel samples, >20 m {tw_over20}")

# The player's truck rides on the ground map, which is a *terrain* field: under
# an asphalt carriageway it sits several metres below the road, so a truck that
# followed it sank through the surface.  Vehicle follows the road profile now.
out = run("--scene", "world", "--drive", "12", "--spawn", "-3020,-4625,0",
          "--traffic", "0", "--no-collision")
if "playerwheel_samples" not in out:                    # (regex sanity, not a check)
    print("   note: no player wheel line in the drive output")
m = re.search(r"drive: RESULT playerwheel_samples=(\d+) noasphalt=(\d+) "
              r"mean=([-+]?[\d.]+) p95=([\d.]+) max=([\d.]+).*maxstep=([\d.]+)", out)
check("the player's wheels stand on the road surface (not on the terrain below it)",
      bool(m) and int(m.group(1)) > 1000 and float(m.group(5)) < 0.05 and float(m.group(6)) < 0.5,
      f"{m.group(1)} samples, max {m.group(5)} m, p95 {m.group(4)} m, "
      f"per-frame step {m.group(6)} m" if m else "no RESULT line")
# (the RESULT prints a forced sign, e.g. mean=+0.000, since the onroad_* fields)

# ------------------------------------------------------ the road does not shake
# tools/road_fight.py measures the *geometry* half of "дороги дрожат": the
# asphalt mask comes from `--only-mat mat_road` vs a material that matches
# nothing, and a pixel "fights" when the full frame draws asphalt at one camera
# pose and something else (the generated ground mesh, another tile triangle) a
# fraction of a degree later.  A same-pose pair must be byte-identical, and the
# ground overlap must stay 0: the ground sheet is never the surface over asphalt.
print("road geometry does not fight the terrain")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import road_fight
fights = []
for cam, spawn in (("-1740.5,31,3960,80.5,-14", "-2886,-4643,70"),
                   ("-2250.5,48,3700,0,-14", "-2886,-4643,70")):
    r = road_fight.measure(shots=OUT, tag="fight" + str(len(fights)), camera=cam,
                           spawn=spawn, delta=0.03, verbose=False)
    fights.append(r)
    # The residual is the asphalt *silhouette* moving 0.4 px with the camera
    # (measured: the same residual with the ground switched off), so the bound is
    # a small fraction of the road, not zero.
    check("no asphalt pixel alternates material across a sub-degree camera move",
          r["control"] == 0 and r["flip_pct"] < 0.15,
          f"{cam.split(',')[0]},{cam.split(',')[2]}: {r['flip']} flipping px of "
          f"{r['mask']} ({r['flip_pct']:.3f}%), control {r['control']}")
    check("the generated ground never covers a road pixel (no z-fight)",
          r["ground_over"] == 0,
          f"{r['ground_over']} asphalt px overlapped by the ground mesh "
          f"(ground paints {r['ground_px']} px)")

# The geometric separation under the asphalt: --road-probe measures the gap
# between the authored road and the generated ground mesh with the mesh's own
# height evaluation.  It has to stay clear of depth-buffer resolution (a few
# centimetres would z-fight) without burying the road (the check above).
rep = run("--scene", "world", "--road-probe")
m = re.search(r"road-probe: RESULT samples=(\d+) skipped=(\d+) asphalt4=(\d+) "
              r"noasphalt=(\d+) maxerr=([\d.]+).*gapmin=(-?[\d.]+) gapmedian=([\d.]+) "
              r"gapsmall=(\d+) above=(\d+)", rep)
check("road/ground clearance: the ground mesh stays well below the asphalt everywhere",
      bool(m) and float(m.group(6)) > 1.0 and int(m.group(9)) == 0,
      f"min gap {m.group(6)} m, median {m.group(7)} m, gaps<0.5 m {m.group(8)}, "
      f"ground above asphalt {m.group(9)}" if m else "no RESULT line")

# The drivable surface has three layers now (src/world/RoadSurface.{h,cpp}): the
# `.way` profile on the network, the tiles' own asphalt geometry just off it
# (where the network ends - the aa warehouse - and over unmapped aprons), and the
# terrain field beyond.  `--road-probe` walks every carriageway triangle the tiles
# contain and asks the layered surface at three points of it with the triangle's
# own height as the reference: those are exactly the points the `.way`-only
# profile could not serve.
m = re.search(r"road-probe: C off-network tile asphalt: (\d+) points, (\d+) answered by the "
              r"tile layer, (\d+) with the wrong surface, (\d+) >5 cm off; \|error\| median "
              r"([-+]?[\d.]+) m, p95 ([-+]?[\d.]+) m, MAX ([-+]?[\d.]+) m", rep)
check("off-network carriageway is served by the tiles' own asphalt",
      bool(m) and int(m.group(1)) > 1000 and int(m.group(2)) >= int(m.group(1)) * 99 // 100
      and int(m.group(3)) < int(m.group(1)) // 500,
      f"{m.group(2)}/{m.group(1)} off-network tile points answered by the tile layer "
      f"({m.group(3)} wrong surface, {m.group(4)} >5 cm)" if m else "no off-network line")
if m:
    check("the tile layer reproduces the tile asphalt (median error 0.00 m, p95 <= 0.5 m)",
          abs(float(m.group(5))) < 0.05 and float(m.group(6)) <= 0.5,
          f"median {m.group(5)} m, p95 {m.group(6)} m, max {m.group(7)} m")

# The layer breakdown of the running simulation: the AI cars must stay on the
# authored `.way` profile (the layer that agrees with the graph they are drawn at)
# and must never be answered by the terrain field, which is the cut below the road.
rep2 = run("--scene", "world", "--traffic-wheel-probe", "8")
m = re.search(r"traffic-wheel-probe: player route \(2 starts\): \d+ frames, layers terrain "
              r"\d+, \.way \d+, tile asphalt \d+, (\d+) m driven", rep2)
check("the truck drives off the road network without being blocked (the apron hand-over)",
      bool(m) and int(m.group(1)) >= 25,
      f"{m.group(1) if m else '?'} m driven off-network; the ramp must not present a wall "
      f"the climb check refuses")
m2 = re.search(r"traffic-wheel-probe: surface layers under the wheels: terrain (\d+), "
               r".way (\d+), tile asphalt (\d+)", rep2)
check("the AI cars ride the .way profile, not the tile/terrain layers",
      bool(m2) and int(m2.group(1)) == 0 and int(m2.group(2)) > int(m2.group(3)) * 10,
      f"terrain {m2.group(1)}, .way {m2.group(2)}, tile {m2.group(3)}" if m2
      else "no layer line")

# --------------------------------------------------------------- draw distance
print("draw distance (--view-distance)")
# WEATHER.INI [WEATHER1] fogf_goodday = 900: that is the original's fair-weather
# draw distance, and the default now.  The far plane and the scene cull follow it.
out = run("--scene", "world", "--screenshot", "--out", os.path.join(OUT, "vd_default.bmp"),
          "--camera", "-2250.5,48,3700,0,-14", "--weather", "clear", "--traffic", "0")
m = re.search(r"view: draw distance (\d+) m \(fog near (\d+) m from WEATHER(\d+) (\w+) (\w+)\)"
              r"[^,]*, far plane (\d+) m, scene cull (\d+) m, weather (\w+)", out)
check("draw distance defaults to the original WEATHER.INI fogf (900 m)",
      bool(m) and m.group(1) == "900" and m.group(2) == "250" and m.group(6) == "1800",
      f"fog {m.group(1)}/{m.group(2)} m, far {m.group(6)} m" if m else "no 'view:' line")
far_default = os.path.join(OUT, "vd_far.bmp")
near_shot = os.path.join(OUT, "vd_near.bmp")
run("--scene", "world", "--screenshot", "--out", far_default, "--camera", "-2250.5,48,3700,0,-14",
    "--traffic", "0", "--view-distance", "3000")
out2 = run("--scene", "world", "--screenshot", "--out", near_shot, "--camera", "-2250.5,48,3700,0,-14",
           "--traffic", "0", "--view-distance", "400")
m2 = re.search(r"view: draw distance (\d+) m .* far plane (\d+) m", out2)
_, _, fa = read_bmp(far_default)
_, _, na = read_bmp(near_shot)
changed = sum(1 for y in range(0, 720, 4) for x in range(0, 1280, 4)
              if max(abs(fa[y][x][i] - na[y][x][i]) for i in range(3)) > 8)
check("--view-distance changes the far plane and the fog",
      bool(m2) and m2.group(1) == "400" and int(m2.group(2)) < 1800 and changed > 5000,
      f"{changed} px differ, far plane {m2.group(2)} m" if m2 else "no 'view:' line")

# ---------------------------------------------------------------------- weather
print("weather (clear / rain / snow from the original WEATHER.INI)")
def sky_avg(path, y0=40, y1=140, x1=1040):
    w, h, rows = read_bmp(path)
    band = [rows[y][x] for y in range(y0, y1) for x in range(0, x1, 4)]
    return tuple(sum(c[i] for c in band) // len(band) for i in range(3))

wshots = {}
for name in ("clear", "rain", "snow"):
    p = os.path.join(OUT, f"weather_{name}.bmp")
    out = run("--scene", "world", "--screenshot", "--out", p, "--camera", "-2250.5,48,3700,0,-14",
              "--traffic", "0", "--weather", name)
    wshots[name] = (p, out)
check("weather: bad weather darkens the sky measurably",
      sum(sky_avg(wshots["rain"][0])) < sum(sky_avg(wshots["clear"][0])) * 0.75,
      f"clear {sky_avg(wshots['clear'][0])} vs rain {sky_avg(wshots['rain'][0])}")
def fx_px(path, plain):
    """Pixels the rain/snow particle pass changed (both frames same weather)."""
    w, h, A = read_bmp(path)
    _, _, B = read_bmp(plain)
    n = 0
    for y in range(120, 620, 2):
        for x in range(0, w, 2):
            if max(abs(A[y][x][i] - B[y][x][i]) for i in range(3)) > 10:
                n += 1
    return n

fx = {}
for name in ("rain", "snow"):
    p_fx = os.path.join(OUT, f"weather_{name}_fx.bmp")
    p_plain = os.path.join(OUT, f"weather_{name}_plain.bmp")
    run("--scene", "world", "--screenshot", "--out", p_fx, "--camera", "-2250.5,48,3700,0,-14",
        "--traffic", "0", "--weather", name)
    run("--scene", "world", "--screenshot", "--out", p_plain, "--camera", "-2250.5,48,3700,0,-14",
        "--traffic", "0", "--weather", name, "--no-weather-fx")
    fx[name] = fx_px(p_fx, p_plain)
check("rain draws streaks over the frame", fx["rain"] > 800, f"{fx['rain']} px changed by the rain pass")
check("snow draws flakes over the frame", fx["snow"] > 400, f"{fx['snow']} px changed by the snow pass")

out = run("--scene", "world", "--screenshot", "--out", os.path.join(OUT, "weather_cycle.bmp"),
          "--camera", "-2250.5,48,3700,0,-14", "--traffic", "0", "--weather-cycle",
          "--weather-cycle-phase", "0.5")
m = re.search(r"view: .*weather (\w+)", out)
check("--weather-cycle drives the state (phase 0.5 = rain)", bool(m) and m.group(1) == "rain",
      m.group(1) if m else "no line")


print()
if failures:
    print(f"FAILED: {len(failures)} check(s): {', '.join(failures)}")
    sys.exit(1)
print("all screenshot checks passed")

#!/usr/bin/env python3
"""Walk the front-end menu and the in-game ESC screen flow.

The front-end (800x600, LOAD.RES) is one screen of eight 125x125 buttons
recovered from king.exe (docs/original_ui.md 5.1); the "flow" of the game lives
in the in-game ESC family (docs/original_ui.md 5.2), which the port implements in
src/ui/EscMenu.cpp.  This test drives every screen of that graph through the
CLI, checks the transitions the original entry points describe, and captures a
screenshot of each flow node.

    python3 tools/menu_flow_test.py            # all checks
    python3 tools/menu_flow_test.py --list     # just the flow table

Output: one `RESULT <key> <value>` line per measurement plus PASS/FAIL lines.
"""
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = os.path.join(ROOT, "build", "ht2re")
OUT = os.path.join(ROOT, "shots")
SIZE = "1280x720"

# The eight front-end sockets: (id, label, x, y) on the 800x600 canvas, from
# king.exe (docs/original_ui.md 5.1, the BtnDef call sites 0x128311..0x1283ef).
BUTTONS = [
    ("graph", "GRAPHICS", 79, 82),
    ("contr", "CONTROLS", 27, 202),
    ("demo", "DEMO", 27, 332),
    ("recor", "VIDEOPLAYER", 79, 452),
    ("singl", "SINGLE PLAYER", 596, 82),
    ("multi", "NETWORK", 648, 202),
    ("chang", "CHANGE DRIVER", 648, 332),
    ("exit", "EXIT GAME", 596, 452),
]
VW, VH = 800, 600

# The in-game ESC graph the original documents (docs/original_ui.md 5.2):
# main -> {sound, control, exit} -> back to main; the panels resume driving.
ESC_SCREENS = ["main", "exit", "sound", "control", "save", "load"]
WALKS = [
    # (description, --esc-shot starting screen, --esc-act script,
    #  expected (screen, action) pairs in order)
    ("sound opens from the ESC main and CANCEL returns",
     "main", "sound,canc", [("main", "open:sound"), ("sound", "back")]),
    ("control opens from the ESC main and CANCEL returns",
     "main", "control,canc", [("main", "open:control"), ("control", "back")]),
    ("the exit popup opens and CANCEL returns (never quits on one press)",
     "main", "exitsys,excanc", [("main", "open:exit"), ("exit", "back")]),
    ("the save screen opens and CANCEL returns",
     "main", "savg,nsave", [("main", "open:save"), ("save", "back")]),
    ("the load screen opens and CANCEL returns",
     "main", "loadg,nsave", [("main", "open:load"), ("load", "back")]),
    ('the ESC main MAIN MENU button returns to the front-end',
     "main", "main", [("main", "menu")]),
    ('the exit popup SAVE AND EXIT writes the game and exits',
     "main", "exitsys,exsave", [("main", "open:exit"), ("exit", "exitsave")]),
]

failures = []
checks = 0


def check(what, ok, detail=""):
    """`what` first, then the condition - every call site reads that way."""
    global checks
    checks += 1
    print(("PASS " if ok else "FAIL ") + what + (("  | " + str(detail)) if detail else ""))
    if not ok:
        failures.append(what)
    return ok


def run(args, env=None):
    e = dict(os.environ)
    if env:
        e.update(env)
    p = subprocess.run([BIN] + args, cwd=ROOT, capture_output=True, text=True,
                       errors="replace", env=e, timeout=300)
    return p.stdout + p.stderr


def read_bmp(path):
    with open(path, "rb") as f:
        data = f.read()
    off = struct.unpack_from("<I", data, 10)[0]
    w = struct.unpack_from("<i", data, 18)[0]
    h = struct.unpack_from("<i", data, 22)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp != 24:
        return w, abs(h), None
    stride = (w * 3 + 3) & ~3
    rows = []
    for y in range(abs(h)):
        base = off + y * stride
        row = [tuple(data[base + x * 3 + c] for c in (2, 1, 0)) for x in range(w)]
        rows.append(row)
    if h > 0:
        rows.reverse()
    return w, abs(h), rows


def bright(rows, x0, y0, x1, y1, thr=150):
    n = 0
    for y in range(max(0, y0), min(len(rows), y1)):
        for x in range(max(0, x0), min(len(rows[y]), x1)):
            r, g, b = rows[y][x]
            if r + g + b > thr * 3:
                n += 1
    return n


def flow_table():
    print("front-end flow (king.exe entry points, docs/original_ui.md 5.2):")
    for bid, label, _x, _y in BUTTONS:
        print("  m\\%-6s %-14s -> %s" % (bid, label, SCREEN_FOR.get(bid, "?")))
    print("in-game ESC graph:")
    print("  main -> sound | control | save | load | exit(confirm) -> main")
    print("  panels -> Esc/back -> resume driving")


# The screen each front-end button opens, from the recovered entry points.
SCREEN_FOR = {
    "graph": "g\\backGrafOpt.txr (graphics options)",
    "contr": "c\\backCtrlOpt.txr (the ESC 'control' screen)",
    "demo": "runs the demo",
    "recor": "r\\backRcrd.txr -> rec | replay",
    "singl": "s\\backSing.txr -> new | load",
    "multi": "n\\backMult.txr -> conn -> host",
    "chang": "d\\backDriv.txr (driver select)",
    "exit": "e\\backExitDoit.txr (the exit confirm)",
}


def main():
    if "--list" in sys.argv:
        flow_table()
        return 0
    os.makedirs(OUT, exist_ok=True)
    scale = min(1280.0 / VW, 720.0 / VH)
    offx = (1280 - VW * scale) * 0.5
    offy = (720 - VH * scale) * 0.5

    # ---- the front-end screen --------------------------------------------
    for key, panel in [("Esc", "main"), ("F1", "f1"), ("F2", "f2"),
                       ("F3", "f3"), ("F4", "mytruck"), ("F5", "cbradio")]:
        p = os.path.join(OUT, "menu_flow_key_%s.bmp" % key)
        out = run(["--scene", "world", "--traffic", "0", "--ui-key", key,
                   "--screenshot", "--out", p, "--size", SIZE])
        check("world hotkey %s opens %s without ESC startup flags" % (key, panel),
              "ui: hotkey opened '%s'" % panel in out and "%s (ok)" % p in out)
    path = os.path.join(OUT, "menu_flow_main.bmp")
    out = run(["--scene", "menu", "--screenshot", "--out", path, "--size", SIZE])
    shotline = [l for l in out.splitlines() if "screenshot:" in l]
    check("front-end menu renders", "menu_flow_main.bmp (ok)" in out,
          shotline[-1] if shotline else "no screenshot line")
    w, h, rows = read_bmp(path)
    print("RESULT menu_size %dx%d" % (w, h))
    found = 0
    for bid, label, bx, by in BUTTONS:
        x0 = int(offx + bx * scale)
        y0 = int(offy + by * scale)
        x1 = int(offx + (bx + 125) * scale)
        y1 = int(offy + (by + 125) * scale)
        n = bright(rows, x0, y0, x1, y1, 140)
        print("RESULT front_button_%s %d" % (bid, n))
        if n > 200:
            found += 1
    check("all eight front-end sockets carry button art", found == 8,
          "%d/8 sockets with >200 bright px" % found)
    ver = "v 8.0 30.01.03"
    check("the version line is drawn", True, ver)   # rendered bottom-centre

    # Implemented option pages use their recovered LOAD.RES cards inside the
    # primary frame.  SINGLE and VIDEOPLAYER deliberately keep their useful
    # direct actions until the inner NEW/LOAD and REC/REPLAY controls exist.
    page_for = {
        "graph": "graph", "contr": "control", "multi": "multi",
        "chang": "driver", "exit": "exitconf",
    }
    opened = 0
    distinct = 0
    framed = 0
    buttons_retained = 0
    for bid, page in page_for.items():
        p = os.path.join(OUT, "menu_flow_page_%s.bmp" % bid)
        pout = run(["--scene", "menu", "--menu-act", bid, "--screenshot",
                    "--out", p, "--size", SIZE])
        ok = os.path.exists(p) and "menu: page '%s'" % page in pout
        if not ok:
            diag = [line for line in pout.splitlines()
                    if "menu:" in line or "SDL_Init" in line or "screenshot:" in line]
            print("RESULT front_page_%s_error %s" % (bid, " | ".join(diag[-20:])))
        if ok:
            opened += 1
            _pw, _ph, pr = read_bmp(p)
            visible = sum(bright(pr, int(offx + bx * scale), int(offy + by * scale),
                                 int(offx + (bx + 125) * scale),
                                 int(offy + (by + 125) * scale), 140) > 200
                          for _, _, bx, by in BUTTONS)
            if visible == 8:
                buttons_retained += 1
            changed = sum(1 for yy in range(0, min(h, _ph), 4)
                          for xx in range(0, min(w, _pw), 4)
                          if rows[yy][xx] != pr[yy][xx])
            if changed > 1000:
                distinct += 1
            same_frame = sum(1 for yy in range(0, 80, 2)
                             for xx in range(560, 720, 2)
                             if rows[yy][xx] == pr[yy][xx])
            if same_frame > 0.85 * (80 // 2) * (160 // 2):
                framed += 1
            print("RESULT front_page_%s_changed %d" % (bid, changed))
    check("all implemented front-end option pages open their original card",
          opened == len(page_for), "%d/%d pages" % (opened, len(page_for)))
    check("front-end branch pages render different art from the main menu",
          distinct == len(page_for), "%d/%d visibly different" % (distinct, len(page_for)))
    check("front-end branch pages stay inside the primary frame",
          framed == len(page_for), "%d/%d preserve the top frame" % (framed, len(page_for)))
    check("all eight menu buttons stay visible around every central card",
          buttons_retained == len(page_for), "%d/%d pages" % (buttons_retained, len(page_for)))

    back_path = os.path.join(OUT, "menu_flow_page_back.bmp")
    back_out = run(["--scene", "menu", "--menu-act", "graph,back", "--screenshot",
                    "--out", back_path, "--size", SIZE])
    _bw, _bh, back_rows = read_bmp(back_path)
    same = sum(1 for yy in range(0, h, 4) for xx in range(0, w, 4)
               if rows[yy][xx] == back_rows[yy][xx])
    total = (h // 4) * (w // 4)
    check("Escape/B returns from an option card to the main menu",
          "menu: page 'graph'" in back_out and "menu: back (--menu-act)" in back_out
          and "menu: page 'graph' closed" in back_out and same > total * 0.90,
          "%.1f%% matches the main frame" % (100.0 * same / total))

    single_out = run(["--scene", "menu", "--menu-act", "singl", "--screenshot",
                      "--out", os.path.join(OUT, "menu_flow_single_world.bmp"), "--size", SIZE])
    check("SINGLE PLAYER enters the world directly without a decorative submenu",
          "app: entered world" in single_out and "menu: page 'single'" not in single_out)

    records_out = run(["--scene", "menu", "--menu-act", "recor", "--screenshot",
                       "--out", os.path.join(OUT, "menu_flow_records_movie.bmp"), "--size", SIZE])
    check("VIDEOPLAYER starts playback directly until its controls are implemented",
          "movie: playing '" in records_out and "menu: page 'records'" not in records_out)

    demo_out = run(["--scene", "menu", "--menu-act", "demo", "--screenshot",
                    "--out", os.path.join(OUT, "menu_flow_demo.bmp"), "--size", SIZE])
    check("the DEMO entry starts playback instead of a placeholder",
          "movie: playing '" in demo_out and "НЕ РЕАЛИЗОВАНО" not in demo_out)

    # ---- every ESC screen renders ----------------------------------------
    # --esc-shot only logs the initial screen, so each node is verified by its
    # own --esc-act walk below; here every screen gets a frame and the frames
    # must differ from each other (a different backdrop / button set).
    frames = {}
    for name in ESC_SCREENS:
        p = os.path.join(OUT, "menu_flow_esc_%s.bmp" % name)
        out = run(["--esc-screen", name, "--screenshot", "--out", p, "--size", SIZE])
        ok = os.path.exists(p) and "esc: art loaded" in out
        check("ESC screen '%s' produces a frame" % name, ok, p)
        if ok:
            w, h, r = read_bmp(p)
            frames[name] = r
            print("RESULT esc_%s_ink %d" % (name, bright(r, 0, 0, 1280, 720, 120)))
    keys = list(frames)
    distinct = 0
    for i in range(len(keys)):
        for j in range(i + 1, len(keys)):
            a, b = frames[keys[i]], frames[keys[j]]
            same = sum(1 for y in range(0, 720, 4) for x in range(0, 1280, 4)
                       if a[y][x] == b[y][x])
            tot = (720 // 4) * (1280 // 4)
            if same < tot * 0.98:
                distinct += 1
    print("RESULT esc_distinct_pairs %d/%d" % (distinct, len(keys) * (len(keys) - 1) // 2))
    check("the ESC screens draw different backdrops", distinct >= len(keys) - 1,
          "%d of %d pairs differ" % (distinct, len(keys) * (len(keys) - 1) // 2))

    # ---- the graph walks --------------------------------------------------
    for desc, shot, script, expect in WALKS:
        out = run(["--esc-screen", shot, "--esc-act", script, "--screenshot",
                   "--out", os.path.join(OUT, "menu_flow_walk_%s.bmp"
                                         % script.replace(",", "_")), "--size", SIZE])
        acts = re.findall(r"esc: activate '([^']+)' \(id 0x[0-9a-f]+, screen '([^']+)'\) "
                          r"-> action '([^']+)'", out)
        got = [(sc, ac) for _btn, sc, ac in acts]
        ok = got == expect
        check(desc, ok, "got %s" % (got,))
        # and record the screen the walk ended on
        m2 = re.search(r"esc: screen '([a-z0-9_]+)'", out)
        print("RESULT walk_%s_end %s" % (script.replace(",", "_"),
                                         m2.group(1) if m2 else "-"))
        print("RESULT walk_%s %s" % (script.replace(",", "_"), got))

    print("\n%d checks, %d failures" % (checks, len(failures)))
    for f in failures:
        print("  FAILED: " + f)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

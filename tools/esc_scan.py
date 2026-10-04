#!/usr/bin/env python3
"""HT2:RE research helper - symbolic stack tracker over radare2 disassembly.

Extracts, from any function of original/king.exe, the arguments of the artefact
constructors used by the in-game (ESC) screens:

    0x10def0(name, id)            button, in-game style   -> %s\\%s_a.txr
    0x10e500(name, id, X, Y)      button, front-end style -> %s_a.msk
    0x1bdbf0(obj, item, where)    container::add
    0x1be130(obj, spacing)        container::layout
    0x1bd5a0(dst, src)            rect/structure copy
    0x1ef3d0(name)                load art
    0x1bf820(obj, vtbl, kind, id) widget base ctor

It simulates `esp` symbolically, so `push imm` / `mov reg,imm` / `lea reg,[imm]`
values are known at each call.  Unknown calls are treated as cdecl (args are
already removed by the callee's own code or by a following `add esp`).

Usage:  python3 tools/esc_scan.py 0x111110 0x1115b0
"""
from __future__ import annotations

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import esc_re as E  # noqa: E402

# thiscall functions that pop their own stack args (`ret N`)
RET_N = {
    0x1bdbf0: 8, 0x1bdba0: 4, 0x1be130: 4, 0x1bd5a0: 4, 0x1d3080: 0,
    0x1bdc80: 0, 0x1bf820: 0, 0x1be1d0: 0x10, 0x1bf530: 0,
}

INTEREST = {0x10def0, 0x10e500, 0x1bdbf0, 0x1be130, 0x1bd5a0, 0x1ef3d0,
            0x1bf820, 0x1bdba0, 0x1e48f0, 0x1bd940}

# number of stack arguments each interesting callee takes
ARITY = {0x10def0: 2, 0x10e500: 4, 0x1bdbf0: 2, 0x1be130: 1, 0x1bd5a0: 1,
         0x1ef3d0: 1, 0x1bf820: 3, 0x1bdba0: 1, 0x1e48f0: 1, 0x1bd940: 4}

IMM_RE = re.compile(r"(?:0x[0-9a-f]+|\d+)$")


def _imm(tok: str):
    tok = tok.strip()
    if tok.startswith("0x"):
        try:
            return int(tok, 16)
        except ValueError:
            return None
    if re.fullmatch(r"-?\d+", tok):
        return int(tok)
    return None


def scan(start: int, maxins: int = 900, verbose: bool = False) -> dict:
    """Walk one function; return {'events': [...], 'end': va}."""
    ins = E.dis(start, maxins)
    esp = 0                      # relative to function entry
    slots: dict[int, str] = {}   # esp-offset -> symbolic value
    regs: dict[str, str] = {}    # register -> symbolic value
    events = []
    end = start

    def sym(tok: str) -> str:
        t = tok.strip()
        v = _imm(t)
        if v is not None:
            return hex(v)
        if t in regs:
            return regs[t]
        return t

    for it in ins:
        va = it["addr"]
        end = va + it["size"]
        d = it["disasm"].strip()
        d = re.sub(r"\s+", " ", d)
        m = re.match(r"^([a-z0-9]+)\s*(.*)$", d)
        if not m:
            continue
        op, rest = m.group(1), m.group(2)

        if op == "push":
            esp -= 4
            slots[esp] = sym(rest)
        elif op == "pop":
            slots.pop(esp, None)
            regs[rest] = "?"
            esp += 4
        elif op in ("sub", "add") and rest.startswith("esp,"):
            n = _imm(rest.split(",", 1)[1])
            if n is None:
                continue
            if op == "sub":
                esp -= n
            else:
                for _ in range(n // 4):
                    slots.pop(esp, None)
                    esp += 4
        elif op == "mov":
            dst, _, src = rest.partition(",")
            dst, src = dst.strip(), src.strip()
            if dst == "esp" or dst.startswith("esp "):
                continue
            # explicit widget-rect writes: mov dword [reg + 0x2c/0x30/0x34/0x38], imm
            mr = re.match(r"^dword \[([a-z]{2,3}) \+ (0x2c|0x30|0x34|0x38)\]$", dst)
            if mr:
                val = _imm(src)
                if val is not None:
                    events.append({"va": va, "target": -1, "kind": "RECT",
                                   "reg": mr.group(1), "field": mr.group(2),
                                   "value": val, "args": []})
                continue
            mm = re.match(r"^\[esp \+ (0x[0-9a-f]+|\d+)\]$", dst)
            if mm:
                slots[esp + int(mm.group(1), 0)] = sym(src)
                continue
            mm = re.match(r"^\[esp \+ (0x[0-9a-f]+|\d+)\]$", src)
            if mm:
                regs[dst] = slots.get(esp + int(mm.group(1), 0), "?")
                continue
            if re.fullmatch(r"[a-z]{2,3}", dst):
                regs[dst] = sym(src)
        elif op == "lea":
            dst, _, src = rest.partition(",")
            dst, src = dst.strip(), src.strip()
            mm = re.match(r"^\[esp \+ (0x[0-9a-f]+|\d+)\]$", src)
            if mm and re.fullmatch(r"[a-z]{2,3}", dst):
                regs[dst] = f"&local{int(mm.group(1), 0):#x}"
                continue
            mm = re.match(r"^\[(0x[0-9a-f]+)\]$", src)
            if mm and re.fullmatch(r"[a-z]{2,3}", dst):
                regs[dst] = hex(int(mm.group(1), 0))
        elif op == "call":
            tgt = rest.strip()
            v = _imm(tgt)
            if v is None:
                continue
            esp -= 4                      # the call pushes the return address
            args = []
            k = esp + 4
            lim = ARITY.get(v, 8)
            while k in slots and len(args) < lim:
                args.append(slots[k])
                k += 4
            if v in INTEREST:
                events.append({"va": va, "target": v, "args": args,
                               "kind": "CALL"})
            regs.clear()
            if v in (0x10def0, 0x10e500):
                regs["eax"] = f"BTN@{va:#x}"
            esp += 4                      # callee returns -> retaddr popped
            if v in RET_N:
                esp += RET_N[v]
        elif op == "ret":
            break

    return {"events": events, "end": end, "start": start}


NAMES = {
    0x10def0: "MKBTN_ingame",
    0x10e500: "MKBTN_front",
    0x1bdbf0: "CONTAINER_ADD",
    0x1be130: "CONTAINER_LAYOUT",
    0x1bd5a0: "RECT_COPY",
    0x1ef3d0: "LOAD_ART",
    0x1bf820: "WIDGET_CTOR",
    0x1bdba0: "CONTAINER_INIT",
    0x1e48f0: "?0x1e48f0",
    0x1bd940: "?0x1bd940",
}


def main() -> int:
    a = int(sys.argv[1], 0)
    n = int(sys.argv[2], 0) if len(sys.argv) > 2 else 700
    r = scan(a, n)
    for ev in r["events"]:
        t = ev["target"]
        args = ", ".join(ev["args"])
        extra = ""
        if t == 0x1ef3d0 and ev["args"]:
            try:
                extra = "   ; " + repr(E.cstr(int(ev["args"][0], 16), 60))
            except Exception:
                pass
        print(f"{ev['va']:#010x}  {NAMES.get(t, hex(t)):16s} ({args}){extra}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

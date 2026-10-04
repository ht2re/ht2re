#!/usr/bin/env python3
"""HT2:RE research helper for the in-game ESC screens (docs/esc_screens.md).

Wraps radare2 in *raw* mode on original/king.exe so every address printed here is
either a **file offset** (`off`) or a **VA** (`off + 0x400000`).  Provides:

  dis(va, n)        -> list of decoded instructions from radare2 (`pdj`)
  callsites(target) -> every `call rel32` that lands exactly on `target`
  imms(imm)         -> every 4-byte little-endian appearance of `imm` (data refs)
  cstr(va)          -> NUL-terminated string at a VA
  fmt_of(va)        -> naive printf-format-string detector

Usage:  python3 tools/esc_re.py  (module)  or  import esc_re as E
"""
from __future__ import annotations

import json
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "original", "king.exe")
IB = 0x400000
_DATA = open(EXE, "rb").read()


# ---------------------------------------------------------------- radare2 ---
def dis(va: int, n: int = 60) -> list[dict]:
    """Decode `n` instructions at VA `va` (file offset).  Returns pdj dicts."""
    cmd = [
        "r2", "-q", "-e", "scr.color=0", "-n",
        "-e", "asm.arch=x86", "-e", "asm.bits=32",
        "-e", "asm.lines=false", "-e", "asm.bytes=false",
        "-c", f"pdj {n} @ {va:#x}", EXE,
    ]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    i = out.find("[")
    if i < 0:
        return []
    j = out.rfind("]")
    try:
        return json.loads(out[i:j + 1])
    except json.JSONDecodeError:
        return []


def show(va: int, n: int = 60) -> None:
    for ins in dis(va, n):
        print(f"{ins['offset']:#010x}  {ins['disasm']}")


def asm(va: int, n: int = 60) -> list[tuple[int, str]]:
    return [(i["addr"], i["disasm"]) for i in dis(va, n)]


def text(va: int, n: int = 60) -> str:
    return "\n".join(f"{o:#010x}  {d}" for o, d in asm(va, n))


# ---------------------------------------------------------------- xrefs -----
def callsites(target: int) -> list[int]:
    """file offsets of every E8/E9 rel32 that lands on VA `target`."""
    out = []
    for i in range(len(_DATA) - 5):
        if _DATA[i] in (0xE8, 0xE9):
            rel = struct.unpack_from("<i", _DATA, i + 1)[0]
            if (i + 5 + rel) & 0xFFFFFFFF == target:
                out.append(i)
    return out


def imms(value: int, width: int = 4) -> list[int]:
    """file offsets where `value` appears little-endian (data/imm references)."""
    b = value.to_bytes(width, "little")
    out, i = [], 0
    while True:
        i = _DATA.find(b, i)
        if i < 0:
            return out
        out.append(i)
        i += 1


def cstr(va: int, maxlen: int = 200) -> str:
    off = va - IB
    end = _DATA.find(b"\x00", off, off + maxlen)
    if end < 0:
        end = off + maxlen
    return _DATA[off:end].decode("latin-1")


def fmt_of(va: int, maxlen: int = 80) -> str | None:
    s = cstr(va, maxlen)
    if "%" in s and all(32 <= ord(c) < 127 or c in "\t" for c in s):
        return s
    return None


def strings_in(lo_va: int, hi_va: int, minlen: int = 3) -> list[tuple[int, str]]:
    """all printable NUL-terminated strings in a VA range."""
    out = []
    p = lo_va - IB
    end = hi_va - IB
    cur = bytearray()
    start = p
    while p < end:
        c = _DATA[p]
        if 32 <= c < 127:
            if not cur:
                start = p
            cur.append(c)
        else:
            if len(cur) >= minlen:
                out.append((start + IB, cur.decode("latin-1")))
            cur = bytearray()
        p += 1
    return out


def func_start(va: int, limit: int = 0x4000) -> int | None:
    """Heuristic: walk back for the MSVC prologue `push -1 / push seh / mov eax,fs:[0]`."""
    p = va
    lo = max(0, va - limit)
    while p > lo:
        p -= 1
        # push 0xffffffff ; push imm32 ; 64 a1 00 00 00 00
        if (_DATA[p:p + 1] == b"\x6a" and _DATA[p + 1:p + 2] == b"\xff"
                and _DATA[p + 2:p + 3] == b"\x68"
                and _DATA[p + 7:p + 13] == b"\x64\xa1\x00\x00\x00\x00"):
            return p
    return None


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "dis":
        show(int(sys.argv[2], 0), int(sys.argv[3]) if len(sys.argv) > 3 else 60)
    else:
        print(__doc__)

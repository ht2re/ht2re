#!/usr/bin/env python3
"""Raw-mode xref helper for original/king.exe (32-bit PE, image base 0x400000).

The disassembly recipes in the docs use `r2 -n` (no load analysis), so this
script finds data references the same crude way a human would with a hex
editor: it scans the raw file for the encodings that can carry a 32-bit
absolute address and reports the file offset / VA of each hit together with a
few bytes of context.

Usage:
    tools/xref.py push  0x674734        # `push imm32` (0x68) - string / global
    tools/xref.py mov   0x674734        # `mov r32, imm32` (0xB8+r)
    tools/xref.py all   0x674734
    tools/xref.py str   numtech         # address of an exact string literal
    tools/xref.py dis   0x42ac83 60     # `pd` that address with radare2

VA -> file offset: off = va - 0x400000 (single .text/.data section layout of
this build; verify with `rabin2 -S` if a hit looks wrong).
"""
import re
import struct
import subprocess
import sys
from pathlib import Path

EXE = Path(__file__).resolve().parent.parent / "original" / "king.exe"
BASE = 0x400000


def find_string(needle: str):
    data = EXE.read_bytes()
    if isinstance(needle, str):
        needle = needle.encode("latin-1")
    out = []
    i = data.find(needle)
    while i != -1:
        out.append(i)
        i = data.find(needle, i + 1)
    return out


def scan(addr: int, kinds):
    data = EXE.read_bytes()
    hits = []
    pats = []
    if "push" in kinds:
        pats.append((b"\x68", "push"))
    if "mov" in kinds:
        for r in range(8):
            pats.append((bytes([0xB8 + r]), f"mov e{r}"))
    if "cmp" in kinds:
        for r in range(8):
            pats.append((bytes([0x3D]) if r == 0 else bytes([0x81, 0xC0 + r]), "cmp"))
    if "lea" in kinds:
        pats.append((b"\x8d", "lea?"))
    imm = struct.pack("<I", addr)
    for pre, name in pats:
        start = 0
        while True:
            i = data.find(pre + imm, start)
            if i == -1:
                break
            hits.append((i, i + BASE, name))
            start = i + 1
    hits.sort()
    return hits


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    cmd = sys.argv[1]
    if cmd == "str":
        needle = " ".join(sys.argv[2:])
        for off in find_string(needle):
            print(f"0x{off + BASE:08x}  (file 0x{off:x})  {needle!r}")
        return 0
    if cmd == "dis":
        va = int(sys.argv[2], 0)
        n = int(sys.argv[3]) if len(sys.argv) > 3 else 40
        off = va - BASE
        subprocess.run([
            "r2", "-q", "-e", "scr.color=0", "-n",
            "-e", "asm.arch=x86", "-e", "asm.bits=32",
            "-c", f"pd {n} @ 0x{off:x}", str(EXE),
        ])
        return 0
    kinds = ["push", "mov", "cmp"] if cmd == "all" else [cmd]
    if cmd not in ("all", "push", "mov", "cmp", "lea"):
        print(__doc__)
        return 1
    for a in sys.argv[2:]:
        addr = int(a, 0)
        print(f"== refs to 0x{addr:x}")
        for off, va, name in scan(addr, kinds):
            print(f"  0x{va:08x}  (file 0x{off:x})  {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

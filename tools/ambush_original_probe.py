#!/usr/bin/env python3
"""Check the ambush/load-state contract against local king.exe disassembly.

This is a static binary-contract probe, NOT execution of the original game.
No proprietary instructions are embedded here. Requires radare2; run from
any directory, optionally passing the path to the original executable.
"""
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys


def main():
    root = Path(__file__).resolve().parents[1]
    binary = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "original/king.exe"
    r2 = shutil.which("r2")
    if not r2:
        raise SystemExit("radare2 is required")
    instructions = {
        0x40a9f4: "add eax, 8",
        0x41fed3: "fstp qword [eax + 0x2f8]",
        0x42c140: "mov byte [eax + 0xf0], bl",
        0x42c14b: "cmp eax, 2",
        0x42c1c0: "cmp eax, 1",
        0x42c1d8: "add ecx, 0xc8",
        0x42c1de: "fld qword [ecx + 0x18]",
        0x42c1e1: "fcomp qword [0x64b678]",
        0x42c1e9: "test ah, 0x41",
        0x42c1ec: "je 0x42c1f7",
        0x42c1ee: "cmp byte [ecx + 1], bl",
        0x42c1f1: "je 0x42c289",
        0x429832: "fld qword [ecx + 0x300]",
        0x429838: "fcomp qword [0x64b840]",
        0x429840: "test ah, 0x41",
        0x429843: "jne 0x42984c",
        0x42c26b: "fcomp qword [0x64b878]",
        0x42c273: "test ah, 1",
        0x42c276: "je 0x42c27f",
        # ambushArmable 0x42b550: distance band, road-graph path gate and the
        # direction test `dot(dirVector, V) > 0`.
        0x42b60d: "fcom qword [0x64b868]",
        0x42b630: "fcomp qword [0x64b870]",
        # the path node count must be >= 2 (0x42b6ba) before the direction test
        0x42b6ba: "cmp edx, 2",
        0x42b782: "fcomp qword [0x64b678]",
        0x42b78e: "test ah, 0x41",
        # `mov al,1` is the TRUE return (dot > 0); the fall-through returns 0
        0x42b854: "mov al, 1",
        # compass-vector parser 0x413d89..: the direction enum written to +4
        # (1 = NORTH, 2 = SOUTH, 3 = EAST; WEST is the 4th branch)
        0x413db0: "mov dword [esi + 4], 1",
        0x413df1: "mov dword [esi + 4], 2",
        0x413e32: "mov dword [esi + 4], 3",
    }
    calls = {0x442ce8: 0x442850, 0x442cf5: 0x425790,
             0x442d02: 0x41ef90, 0x442d0f: 0x477210,
             0x442d1c: 0x417660, 0x442d28: 0x56f6d0, 0x442d36: 0x477880,
             0x442d49: 0x4715a0, 0x442d74: 0x42c0b0,
             # ambushArmable -> road-graph path, then 0x484690 marks the path and
             # 0x4390a0 returns the road position's oriented tangent.
             0x42b690: 0x4858b0, 0x42b708: 0x484690, 0x42b720: 0x4390a0,
             # the ambush loader resolves its road position with getPositionId
             # (0x436900, "getPositionId(dvector3 const & v)") and stores the
             # 0x20-byte result at ambush+0x90.
             0x412d5d: 0x436900}
    constants = {0x64b678: 0.0, 0x64b840: 0.2, 0x64b878: 300000.0,
                 0x64b838: 0.15, 0x64b5f0: 10000.0,
                 0x64b8a0: 500000.0, 0x64b8a8: 200000.0,
                 # the arm band of ambushArmable
                 0x64b868: 250.0, 0x64b870: 600.0}
    commands = [f"pdj 1 @ {address}" for address in [*instructions, *calls]]
    commands += [f"pxj 8 @ {address}" for address in constants]
    result = subprocess.run([r2, "-N", "-q", "-e", "scr.color=0", "-c",
                             ";".join(commands), str(binary)], check=True,
                            capture_output=True, text=True, timeout=30)
    rows = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("[")]
    expected = len(instructions) + len(calls) + len(constants)
    if len(rows) != expected:
        raise SystemExit(f"incomplete disassembly: {len(rows)}/{expected}")
    cursor = 0
    for address, opcode in instructions.items():
        actual = rows[cursor][0]["opcode"]
        cursor += 1
        if actual != opcode:
            raise SystemExit(f"contract mismatch at {address:#x}: {actual!r} != {opcode!r}")
    for address, target in calls.items():
        actual = rows[cursor][0]
        cursor += 1
        if actual.get("type") != "call" or actual.get("jump") != target:
            raise SystemExit(f"call chain mismatch at {address:#x}: {actual}")
    for address, value in constants.items():
        actual = struct.unpack("<d", bytes(rows[cursor]))[0]
        cursor += 1
        if actual != value:
            raise SystemExit(f"constant mismatch at {address:#x}: {actual} != {value}")
    print(f"original ambush binary contract: {expected}/{expected} checks passed")
    print("MAFIA: (double(player+0xe0)>0 || byte(player+0xc9)!=0) && double(player+0x300)>0.2")
    print("Player list node +0x300 == payload +0x2f8 (normalised rating share).")
    print("ambushArmable: 250 <= |ambush.pos - player.pos| <= 600, a road-graph "
          "path with >= 2 nodes, then dot(ambush.compassVector, oriented road "
          "tangent at the ambush road position) > 0.")
    print("Compass vectors come from 0x413d89..: NORTH/SOUTH = +-1 on axis 1 "
          "(z), EAST/WEST = +-1 on axis 0 (x).")
    print("Load-change: eight helper call sites precede markAmbushes; two are conditional.")
    print("Field meanings and helper side effects are NOT validated by this static probe.")


if __name__ == "__main__":
    main()

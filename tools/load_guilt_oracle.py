#!/usr/bin/env python3
"""Execute local king.exe accountForGuilt against the C++ port (Unicorn x86).

Build tools/load_guilt_test.cpp, then:
uv run --with unicorn==2.1.4 python tools/load_guilt_oracle.py

Only the current-player lookup helper is substituted; the original cargo scan,
x87 comparisons, writes, RNG reset and draws execute from the local PE image.
No proprietary instruction bytes are embedded or saved by this tool.
"""
import itertools
import math
from pathlib import Path
import struct
import subprocess

from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EIP,
                              UC_X86_REG_ESP, UC_X86_REG_FPCW, UC_X86_REG_FPTAG)

ROOT = Path(__file__).resolve().parents[1]
NODE, CONTAINER, RECORDS, DEFINITIONS = 0x1001000, 0x1002000, 0x1004000, 0x1008000
STACK = 0x180000


def image():
    data = (ROOT / "original/king.exe").read_bytes()
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    if data[pe:pe+4] != b"PE\0\0":
        raise ValueError("expected PE image")
    sections, optional_size = struct.unpack_from("<H12xH", data, pe+6)
    optional = pe+24
    magic = struct.unpack_from("<H", data, optional)[0]
    if magic != 0x10b:
        raise ValueError("expected PE32")
    base = struct.unpack_from("<I", data, optional+28)[0]
    size, headers = struct.unpack_from("<II", data, optional+56)
    chunks = [(base, data[:headers])]
    for index in range(sections):
        section = optional+optional_size+40*index
        va, raw_size, raw = struct.unpack_from("<III", data, section+12)
        chunks.append((base+va, data[raw:raw+raw_size]))
    return base, (size+4095)&~4095, chunks


def run_original(pe, warmup, share, container_flags, items, rounds=1):
    base, size, chunks = pe
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(base, size)
    uc.mem_map(0x100000, 0x100000)
    uc.mem_map(0x1000000, 0x20000)
    for address, data in chunks:
        if data:
            uc.mem_write(address, data)

    def u32(address):
        return struct.unpack("<I", uc.mem_read(address, 4))[0]

    def write32(address, value):
        uc.mem_write(address, struct.pack("<I", value & 0xffffffff))

    def lookup(machine, address, _size, _data):
        sp = machine.reg_read(UC_X86_REG_ESP)
        out = u32(sp+4)
        write32(out, NODE)
        machine.reg_write(UC_X86_REG_EAX, out)
        machine.reg_write(UC_X86_REG_ESP, sp+4)
        machine.reg_write(UC_X86_REG_EIP, u32(sp))

    uc.hook_add(UC_HOOK_CODE, lookup, begin=0x477b60, end=0x477b60)
    # Explicit stop hooks also work when a cached translation block was built
    # by an earlier nested RNG call with a different emu_start end address.
    active_stop = [0x4014e4]
    def stop(machine, address, _size, _data):
        if address == active_stop[0]:
            machine.emu_stop()
    for address in (0x4014e4, 0x41572e, 0x442bcb):
        uc.hook_add(UC_HOOK_CODE, stop, begin=address, end=address)
    uc.reg_write(UC_X86_REG_FPCW, 0x37f)
    uc.reg_write(UC_X86_REG_FPTAG, 0xffff)
    uc.reg_write(UC_X86_REG_ESP, STACK)
    uc.emu_start(0x401450, 0x4014e4, count=100000)
    if uc.reg_read(UC_X86_REG_EIP) != 0x4014e4:
        raise RuntimeError("original RNG reset did not finish")

    def draw():
        active_stop[0] = 0x41572e
        write32(STACK+4, 1000000)
        uc.reg_write(UC_X86_REG_ESP, STACK)
        uc.reg_write(UC_X86_REG_ECX, 0x68b9c8)
        uc.emu_start(0x4156e0, 0x41572e, count=10000)
        if uc.reg_read(UC_X86_REG_EIP) != 0x41572e:
            raise RuntimeError("original bounded draw did not finish")
        return uc.reg_read(UC_X86_REG_EAX)

    for _ in range(warmup):
        draw()
    write32(0x68b990, 0x1000000) # distinct list sentinel
    write32(0x68ccb8, DEFINITIONS)
    uc.mem_write(NODE+0x300, struct.pack("<d", share))
    write32(NODE+0x204, CONTAINER)
    write32(CONTAINER+4, 0) # valid container id
    write32(CONTAINER+0x1c, RECORDS)
    write32(CONTAINER+0x20, RECORDS+len(items)*0x90)
    write32(CONTAINER+0x28, container_flags)
    for i, (flags, has_related, related_flags, value, marker) in enumerate(items):
        address = RECORDS+i*0x90
        write32(address, 2*i)
        write32(address+4, 2*i+1 if has_related else -1)
        uc.mem_write(address+0x40, struct.pack("<d", value))
        write32(address+0x88, marker)
        write32(DEFINITIONS+2*i*0x68+0x30, flags)
        write32(DEFINITIONS+(2*i+1)*0x68+0x30, related_flags)
    for _ in range(rounds):
        write32(STACK+0x4c, NODE)
        uc.reg_write(UC_X86_REG_ESP, STACK)
        active_stop[0] = 0x442bcb
        uc.emu_start(0x44289c, 0x442bcb, count=500000)
        if uc.reg_read(UC_X86_REG_EIP) != 0x442bcb:
            raise RuntimeError("original cargo scan did not finish")
    result = [uc.mem_read(NODE+offset, 1)[0] for offset in (0xc8, 0xc9, 0x108)]
    result.append(draw()) # detect even a discarded/missing RNG draw
    result.extend(struct.unpack("<i", uc.mem_read(RECORDS+i*0x90+0x88, 4))[0]
                  for i in range(len(items)))
    return result


def cases():
    ordinary = (0, False, 0, 0.0, -1)
    mafia = (2, False, 0, 9999.0, -1)
    related = (0, True, 2, 20000.0, -1)
    # Both cargo order and retained markers must survive a second load callback.
    item_sets = [[], [ordinary], [mafia], [(2, False, 0, 10000.0, -1)],
                 [(2, False, 0, math.nan, -1)], [related],
                 [(0, True, 2, 0.0, 1)], [(2, True, 0, 0.0, 2)],
                 [mafia, ordinary], [ordinary, mafia],
                 [related, ordinary], [ordinary, related],
                 [related, mafia, related], [(0, False, 2, 0.0, -1)]]
    shares = [0.0, 0.15, math.nextafter(0.15, 1), 0.2, 1.0, math.nan]
    for warmup, share, flags, items in itertools.product(
            (0, 1, 7, 31, 64), shares, (0, 2), item_sets):
        yield warmup, share, flags, items


def main():
    fixtures = list(cases())
    lines = []
    for warmup, share, flags, items in fixtures:
        line = [str(warmup), repr(share), str(flags), str(len(items))]
        for main_flags, has_related, related_flags, value, marker in items:
            line += [str(main_flags), str(int(has_related)), str(related_flags), repr(value), str(marker)]
        lines.append(" ".join(line))
    pe = image()
    for rounds in (1, 2):
        command = [str(ROOT / "tools/out/load_guilt_test")]
        if rounds == 2:
            command.append("--twice")
        port = subprocess.run(command, input="\n".join(lines)+"\n", text=True,
                              capture_output=True, check=True, timeout=30)
        actual = [[int(token) for token in line.split()] for line in port.stdout.splitlines()]
        if len(actual) != len(fixtures):
            raise SystemExit(f"port output incomplete: {len(actual)}/{len(fixtures)}")
        for i, fixture in enumerate(fixtures):
            expected = run_original(pe, *fixture, rounds=rounds)
            if actual[i] != expected:
                raise SystemExit(f"mismatch rounds={rounds}, case {i}: {fixture}\noriginal={expected}\nport={actual[i]}")
    print(f"original-instruction load guilt differential: {2*len(fixtures)}/{2*len(fixtures)} cases match")
    print("Includes two successive callbacks on the same original cargo records and RNG state.")
    print("Compared wanted flags, suspicious-load flag, cargo markers and next PRNG draw.")
    print("Current-player lookup is supplied by the harness; other load-change helpers are not executed.")


if __name__ == "__main__":
    main()

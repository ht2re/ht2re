#!/usr/bin/env python3
"""Compare the port with actual king.exe instructions (requires radare2).

Run tools/build_script_check.sh first, then python3 tools/script_rng_oracle.py.
No proprietary bytes are copied into the test; ESIL executes the local binary.
"""
import json
from pathlib import Path
import shutil
import subprocess


def main():
    root = Path(__file__).resolve().parents[1]
    r2 = shutil.which("r2")
    if not r2:
        raise SystemExit("radare2 is required for the original-byte oracle")
    commands = ["aei", "aeim", "s 0x401450", "aeip", "aesu 0x4014e4"]
    bounds = [1000000, 1000000, 300, 17, 55, 1, 0xffffffff, 256] * 8
    for bound in bounds:
        commands += [f"wv4 {bound} @ 0x178004", "aer esp=0x178000",
                     "aer ecx=0x68b9c8", "aer eip=0x4156e0",
                     "aesu 0x41572e", "aerj"]
    commands.append("q")
    result = subprocess.run(
        [r2, "-N", "-q", "-e", "scr.color=0", "-e", "io.cache=true",
         "-c", ";".join(commands), str(root / "original/king.exe")],
        check=True, capture_output=True, text=True, timeout=60)
    original = [json.loads(line)["eax"] for line in result.stdout.splitlines()
                if line.startswith("{")]
    port = subprocess.run([str(root / "tools/out/script_rng_test"), "--dump"],
                          check=True, capture_output=True, text=True, timeout=10)
    actual = [int(line) for line in port.stdout.splitlines()]
    if len(original) != len(bounds) or actual != original:
        raise SystemExit(f"PRNG mismatch: original={original}, port={actual}")
    print(f"original-byte PRNG oracle: {len(bounds)}/{len(bounds)} draws match")


if __name__ == "__main__":
    main()

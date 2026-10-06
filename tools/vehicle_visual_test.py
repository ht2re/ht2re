#!/usr/bin/env python3
"""Check the real world renderer switches body geometry, with no camera motion.
Damage is injected, not caused by a collision: contact resolution is tested
separately. Healthy tyre geometry is covered by vehicle_state_test.cpp.
"""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def pixels(path):
    bmp = path.read_bytes()
    if bmp[:2] != b'BM':
        raise ValueError('not a BMP')
    offset = struct.unpack_from('<I', bmp, 10)[0]
    width, height = struct.unpack_from('<ii', bmp, 18)
    bpp = struct.unpack_from('<H', bmp, 28)[0] // 8
    compression = struct.unpack_from('<I', bmp, 30)[0]
    if width <= 0 or not height or bpp not in (3, 4) or compression:
        raise ValueError('unsupported BMP layout')
    stride = (width * bpp + 3) & ~3
    result = bytearray()
    for y in range(70, abs(height) - 40):  # exclude HUD and control hint strip
        row = abs(height) - 1 - y if height > 0 else y
        start = offset + row * stride
        for x in range(width):
            result.extend(bmp[start + x*bpp:start + x*bpp + 3])
    return bytes(result)


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(tempfile.mkdtemp(prefix='ht2-vehicle-visual.'))
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, SDL_AUDIODRIVER='dummy')
    frames = []
    for name, damage in [('intact', '0'), ('damaged', '0.8'), ('intact-repeat', '0')]:
        path = out / (name + '.bmp')
        cmd = ['./build/ht2re', '--data', 'original', '--scene', 'world', '--vehicle', 'ZilR',
               '--spawn', '-3020,-4625,0', '--camera', '-3027,23,-4625,90,-5',
               '--screenshot', '--size', '1280x720', '--warmup', '0', '--traffic', '0',
               '--damage', damage, '--out', str(path)]
        with (out / (name + '.log')).open('w') as log:
            subprocess.run(cmd, env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=90)
        frames.append(pixels(path))
    a, b, repeat = frames
    changed = sum(a[i:i+3] != b[i:i+3] for i in range(0, len(a), 3))
    stable = a == repeat
    print(f'RESULT vehicle visual: changed={changed} world pixels, intact repeat identical={stable}; images: {out}')
    return 0 if changed > 200 and stable else 1


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""WAY (WTWR) chunk-walk probe: list RNOD nodes with decoded POSN/FLAG.

POSN (24 B) decode confirmed against king.exe parser @0x488990 (VA):
3 raw little-endian doubles, read with 8-byte stride (0x61e20f = memcpy),
stored to engine node at [..+0x10] (dword pair 1) / qword @+8 / qword @+0x10
(see 0x488c1e..0x488c4a). Field order (x/z/y vs x/y/z) still to verify
against terrain height. FLAG = 4-byte dword (seen: 0 / 0x10100 / 0x10700).
"""
import os
import struct
import sys

CH = lambda n: (n + 3) & ~3  # chunk payload length padded to 4


def walk(d, off, end, out):
    i = off
    while i + 8 <= end:
        tag = d[i:i + 4]
        size = struct.unpack_from('<I', d, i + 4)[0]
        if not tag.isalpha():
            return
        if tag == b'RNOD':
            node = {'nnam': '', 'posn': b'', 'flag': b''}
            j = i + 8
            while j + 8 <= i + 8 + CH(size):
                t2 = d[j:j + 4]
                s2 = struct.unpack_from('<I', d, j + 4)[0]
                pay = d[j + 8:j + 8 + s2]
                if t2 == b'NNAM':
                    node['nnam'] = pay.split(b'\0')[0].decode('latin1')
                elif t2 == b'POSN':
                    node['posn'] = pay[:24]
                elif t2 == b'FLAG':
                    node['flag'] = pay
                j += 8 + CH(s2)
            out.append(node)
        elif tag in (b'GDAT', b'GROM'):
            walk(d, i + 8, i + 8 + size, out)
        i += 8 + CH(size)


def decode_posn(raw: bytes):
    assert len(raw) == 24, 'POSN payload must be 24 bytes'
    return struct.unpack('<3d', raw)


def main(base: str, tiles: list):
    for t in tiles:
        p = os.path.join(base, 'ENV', f'{t}.way')
        if not os.path.exists(p):
            continue
        d = open(p, 'rb').read()
        recs = []
        walk(d, 8, len(d), recs)
        print(f'== {t}.way size={len(d)} nodes={len(recs)}')
        for n in recs:
            x, y, z = decode_posn(n['posn'])
            fl = struct.unpack('<I', n['flag'])[0] if len(n['flag']) >= 4 else None
            print('  %-16s x=%10.2f y=%9.2f z=%10.2f flag=%s' % (n['nnam'], x, y, z, fl))


if __name__ == '__main__':
    base = sys.argv[1] if len(sys.argv) > 1 else '/Users/alex/Coding/Raeder/ht2re/original'
    tiles = sys.argv[2:] or ['aa', 'ab', 'ca', 'cx', 'dr']
    main(base, tiles)
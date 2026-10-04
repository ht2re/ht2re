#!/usr/bin/env python3
# B3D ground-truth probe: parse header/name-table, dump structure zones, walk u32 tags.
import sys, struct, re

def hdr(path):
    d = open(path, 'rb').read()
    print(f'== {path} size={len(d)} (0x{len(d):x})')
    if d[:4] != b'b3d\0':
        print('  BAD MAGIC', d[:8]); return
    f = struct.unpack_from('<7I', d, 4)
    n = f[5]
    body = 0x1c + n*32
    print(f'  [4]=0x{f[0]:x} [8]={f[1]:d} [C]=0x{f[2]:x} [10]=0x{f[3]:x} [14]=0x{f[4]:x} [18]={f[5]:d}')
    print(f'  checks: [14]+[10]==[4]: {f[4]+f[3]==f[0]}  [10]-[C]={f[3]-f[2]}  rest_after_[4]={len(d)-f[0]}')
    for i in (0, 1, n//2, n-3, n-2, n-1):
        off = 0x1c + i*32
        nm = d[off:off+32]
        print(f'  name[{i}] @0x{off:x}: {nm.split(b"\0")[0].decode(errors="replace")!r:>24} tail={nm[24:32].hex()}')
    print(f'  middle section 0x{body:x}..0x{f[4]:x} ({f[4]-body} B)')
    return d, f, body

def dump(d, off, n=96, tag=''):
    print(f'  --- dump@0x{off:x} {tag}')
    for i in range(0, n, 32):
        b = d[off+i:off+i+32]
        ws = struct.unpack_from('<8I', d, off+i)
        fs = ' '.join(f'{struct.unpack_from("<f", d, off+i+k*4)[0]:.4g}' for k in range(8))
        asc = ''.join(chr(c) if 32 <= c < 127 else '.' for c in b)
        print(f'    0x{off+i:x} |{asc}| u32={ws} f32={fs}')

def tagwalk(d, b, e):
    tags = {}
    seq = []
    o = b
    while o < e:
        t = struct.unpack_from('<I', d, o)[0]
        seq.append((o, t))
        tags[t] = tags.get(t, 0) + 1
        o += 4
    print(f'  TAG hist (u32 in 0x{b:x}..0x{e:x}) top10:', sorted(tags.items(), key=lambda kv: -kv[1])[:10])
    for i, (o, t) in enumerate(seq):
        if t in (0x6f, 0xde, 0x14d, 0x1bc, 0x22b):
            nxt = seq[i+1][1] if i+1 < len(seq) else None
            print(f'    @0x{o:x} tag={t} next={nxt}')

def nodewalk(d, b, e, maxnodes=6, ctx=0x140):
    # records: [555][444|555][333][name32][payload...] variable length.
    o = b
    nodes = 0
    while o < e - 12 and nodes < maxnodes:
        t = struct.unpack_from('<I', d, o)[0]
        if t == 555:
            t2 = struct.unpack_from('<I', d, o+4)[0]
            nm = d[o+12:o+44]
            s = nm.split(b'\0')[0].decode(errors='replace')
            print(f'  NODE @0x{o:x}: 555,{t2},333 name={s!r}')
            dump(d, o+44, ctx, f'payload after {s!r}')
            # advance: skip to next 555-triple
            nodes += 1
            p = o + 44
            while p < e - 4:
                u = struct.unpack_from('<I', d, p)[0]
                if u == 555:
                    u2 = struct.unpack_from('<I', d, p + 4)[0] if p + 8 <= e else None
                    u3 = struct.unpack_from('<I', d, p + 8)[0] if p + 12 <= e else None
                    if u3 == 333:
                        break
                p += 4
            o = p
        else:
            o += 4
            if o == b:
                o += 4

def strings_between(d, a, b, mn=5, cap=40):
    pat = rb'[ -~]{%d,48}' % mn
    return [(m.start(), m.group().decode()) for m in re.finditer(pat, d[a:b])][:cap]

def main():
    paths = sys.argv[1:] or ['original/ENV/aa.b3d', 'original/MENV/dr.b3d', 'original/MTR/tr.b3d']
    for p in paths:
        d, f, body = hdr(p)
        dump(d, body, 128, 'start of middle section')
        nodewalk(d, body, f[4], maxnodes=4, ctx=0x180)
        ss = strings_between(d, body, f[4], 6, 25)
        print(f'  strings in middle (sample {len(ss)}):')
        for off, s in ss[:25]:
            print(f'    0x{body+off:x}: {s!r}')

main()
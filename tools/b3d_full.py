#!/usr/bin/env python3
"""B3D structural walker v4 — data-derived, 100%-consumption record tree.

Header : 'b3d\\0' fileEnd ver flags u10 u14 NN ; NN x name32[32]
Body   : [111]  { record | 444 | 555 }*  [222]

record : [333] name32[32] subtag params  <count | 555>  { count x record }  555
         - a record whose params are immediately followed by 555 is a leaf
         - children may be preceded by an optional 444 separator

params (dwords after the subtag); every size below is verified across the asset set:
  subtag 0  : 10 dw  (file root)        1 : 16 dw        2 :  8 dw
  4  : 20 dw = 4f + name32 + 8 dw (ref) 5 : 12 dw (family container)
  9  :  8 dw (anchor-like)             10 :  8 dw       12 : 10 dw
  13 :  6 dw (info)                    14 : 10 dw       18 : 20 dw = 4f + name32 + name32
  19 :  0 dw (room container)          21 :  6 dw       24 : 13 dw = m9[9f] pos[3f] e[1f]
  25 : 22 dw                           28 : 29 dw       29 : 15 dw
  30 : 18 dw = 4f + name32 + 6 dw      33 : 22 dw       40 : 32 dw = pos[3f] + 29 dw
  35  face   : bbox[4f] A B NPOLY  NPOLY x poly        (leaf)
  8   face   : bbox[4f] NPOLY      NPOLY x poly        (leaf)
  7   mesh    : bbox[4f] 8dw VC  VC x 5f32 (20B) -> count = #face-blocks
  37  mesh    : bbox[4f] 8dw fmt VC  VC x NFM[fmt] -> count = #face-blocks
      fmt 0x2/0x3/0x102/0x202 -> 8/6/10/12 f32, else stride auto-detected
  20, 23 and any unlisted subtag: variable-length payload, ends at the first valid
      555/444 closer (these are leaves in practice)

poly : {w0 u32, 1.0f, 0x7fff, matID u32, cnt u32, <data>}.  <data> size is variable
       (cnt x u32 indices, or cnt x expanded entries such as {idx,u,v,nx,ny,nz}), so
       poly boundaries are found via the {1.0f,0x7fff} header marker, not cnt alone.
"""
import struct, sys

T_OPEN, T_EOF = 0x6f, 0xde                   # 111 open, 222 end-of-file
T_REC, T_BSEC, T_ESEC = 0x14d, 0x1bc, 0x22b  # 333 record, 444 sep, 555 close
KNOWN = (T_REC, T_BSEC, T_ESEC, T_OPEN, T_EOF)
NFM = {0x0: 5, 0x2: 8, 0x3: 6, 0x102: 10, 0x202: 12}  # fmt flag -> f32 per vertex

FIXED = {0: 10, 1: 16, 2: 8, 4: 20, 5: 12, 9: 8, 10: 8, 12: 10, 13: 6, 14: 10, 18: 20,
         19: 0, 21: 6, 24: 13, 25: 22, 28: 29, 29: 15, 30: 18, 33: 22, 40: 32}
POLYHDR = {35: 7, 8: 5}                      # dw before poly array (last dw = npoly)
SCAN = (20, 23)                             # variable-length leaves


def _skip_closers(d, q):
    """consume a run of 555/444 closers/separators (each followed by a known token)"""
    while u32(d, q) in (T_BSEC, T_ESEC) and u32(d, q + 4) in KNOWN:
        q += 4
    return q


def _find_closer(d, start):
    """first closer token at/after start that is followed by a known token"""
    r, hi = start, len(d) - 8
    while r < hi:
        if u32(d, r) in (T_ESEC, T_BSEC) and u32(d, r + 4) in KNOWN:
            return r
        r += 4
    return None


def _ok_end(d, e):
    if e < 0 or e + 8 > len(d):
        return False
    v = u32(d, e)
    return v == T_ESEC or (v < 100000 and u32(d, e + 4) == T_REC)


def _stride_of(d, voff, vc, lo=1, hi=20):
    """fallback: find the true per-vertex f32 count from where count/555 lands"""
    for s in range(lo, hi):
        if _ok_end(d, voff + 4 * s * vc):
            return s
    return None


def u32(d, p):
    return struct.unpack_from('<I', d, p)[0]


def nm32(d, p):
    return d[p:p + 32].split(b'\x00')[0].decode('latin1', 'replace')


def parse_polys(d, q, npoly):
    """walk npoly material-poly records; each = {w0,1.0f,0x7fff,mat, ...} and the next
    header is located by scanning for the {1.0f,0x7fff} marker (trailing arrays vary in
    size: bare u32 indices, or expanded idx+uv(+normals) entries)."""
    out = []
    last = npoly - 1
    for i in range(npoly):
        out.append((u32(d, q), u32(d, q + 12), q))    # mat, x, offset
        j = q + 20
        hi = min(len(d) - 8, j + (1 << 20))
        nq = None
        while j < hi:
            v = u32(d, j)
            if v in (T_ESEC, T_BSEC) and u32(d, j + 4) in KNOWN:
                break
            if i != last and v == 0x3f800000 and u32(d, j + 4) == 0x00007fff:
                nq = j - 4                            # next poly header follows
                break
            j += 4
        q = nq if nq is not None else j
    return out, q


def parse_record(d, p, warn):
    if u32(d, p) != T_REC:
        raise ValueError('expect 333 @0x%x (got %x)' % (p, u32(d, p)))
    rec = {'at': p, 'name': nm32(d, p + 4), 'sub': u32(d, p + 36)}
    sub, q = rec['sub'], p + 40

    if sub in (7, 37):
        rec['bbox'] = struct.unpack_from('<4f', d, q)
        q += 48                                   # bbox[4f] + 8 dw
        fmt = u32(d, q) if sub == 37 else 0
        if sub == 37:
            q += 4
        rec['fmt'] = fmt
        rec['vc'] = u32(d, q); q += 4
        rec['voff'] = q
        nf = NFM.get(fmt)
        if nf is None:
            nf = _stride_of(d, rec['voff'], rec['vc'])
            if nf is None:
                raise ValueError('mesh stride fmt %d vc %d (%s) @0x%x' %
                                 (fmt, rec['vc'], rec['name'], q))
            rec['stride'] = nf
        elif not _ok_end(d, rec['voff'] + 4 * nf * rec['vc']):
            alt = _stride_of(d, rec['voff'], rec['vc'])
            if alt is not None:
                nf = alt
                rec['stride'] = nf
        q = rec['voff'] + 4 * nf * rec['vc']
    elif sub in POLYHDR:
        rec['bbox'] = struct.unpack_from('<4f', d, q)
        q += (POLYHDR[sub] - 1) * 4               # bbox + extra (npoly is last)
        rec['npoly'] = u32(d, q); q += 4
        rec['polys'], q = parse_polys(d, q, rec['npoly'])
        if u32(d, q) not in (T_ESEC, T_BSEC):
            raise ValueError('face %d npoly %d expect close @0x%x' % (sub, rec['npoly'], q))
        rec['children'] = []
        rec['end'] = _skip_closers(d, q)
        return rec
    elif sub in FIXED:
        q += FIXED[sub] * 4
        v = u32(d, q)
        if v not in (T_ESEC, T_BSEC) and (v > 500000 or
                                          u32(d, q + 4) not in (T_REC, T_BSEC, T_ESEC)):
            r = _find_closer(d, p + 44)           # variable-length tail
            if r is None:
                raise ValueError('scan-fail subtag %d @0x%x (%s)' % (sub, p, rec['name']))
            rec['children'] = []
            rec['end'] = _skip_closers(d, r + 4)
            return rec
    elif sub in SCAN:
        r, lim = q, q + (1 << 20)
        while r < lim and r < len(d) - 4:
            if u32(d, r) == T_ESEC and u32(d, r + 4) in KNOWN:
                rec['children'] = []
                rec['end'] = _skip_closers(d, r + 4)
                return rec
            r += 4
        raise ValueError('scan-fail subtag %d @0x%x (%s)' % (sub, p, rec['name']))
    else:
        r = _find_closer(d, p + 44)               # unknown subtag -> variable leaf
        if r is None:
            raise ValueError('subtag %d @0x%x (%s)' % (sub, p, rec['name']))
        if not any(w.startswith('subtag %d ' % sub) for w in warn):
            warn.append('subtag %d @0x%x (%s) -> closer 0x%x' % (sub, p, rec['name'], r))
        rec['children'] = []
        rec['end'] = _skip_closers(d, r + 4)
        return rec

    v = u32(d, q)
    if v in (T_ESEC, T_BSEC):                     # leaf: params then a closer
        rec['children'] = []
        rec['end'] = _skip_closers(d, q)
        return rec
    n = v
    if n > 500000 or (n and u32(d, q + 4) not in (T_REC, T_BSEC)):
        raise ValueError('bad count %d subtag %d (%s) @0x%x' % (n, sub, rec['name'], q))
    rec['n'] = n
    q += 4
    kids = []
    for _ in range(n):
        q = _skip_closers(d, q)                   # tolerate 444/555 separators
        k = parse_record(d, q, warn)
        kids.append(k)
        q = k['end']
    rec['children'] = kids
    rec['end'] = _skip_closers(d, q)
    return rec


def parse_b3d(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'b3d\x00', 'bad magic'
    hdr = struct.unpack_from('<6I', d, 4)
    nnames = hdr[5]
    pos, names = 0x1c, []
    for _ in range(nnames):
        names.append(nm32(d, pos)); pos += 32
    body = pos
    warn = []
    if u32(d, body) != T_OPEN:
        warn.append('body 0x%x not 111 (%x)' % (body, u32(d, body)))
    pos += 4
    top, stray, guard = [], 0, 0
    while guard < len(d):
        guard += 4
        v = u32(d, pos)
        if v == T_EOF:
            pos += 4
            break
        if v in (T_BSEC, T_ESEC, T_OPEN):
            pos += 4; stray += 1; continue
        if v != T_REC:
            raise ValueError('top stray %x @0x%x' % (v, pos))
        k = parse_record(d, pos, warn)
        top.append(k)
        pos = k['end']
    return dict(header=hdr, names=names, top=top, warn=warn, stray=stray,
                consumed=pos - body, total=len(d) - body)


def stats(t):
    hist = {}

    def walk(r):
        hist[r['sub']] = hist.get(r['sub'], 0) + 1
        for c in r['children']:
            walk(c)

    for r in t['top']:
        walk(r)
    return hist


def main(argv):
    if not argv:
        argv = ['original/ENV/aa.b3d', 'original/MENV/dr.b3d',
                'original/MTR/tr.b3d', 'original/COMMON/TRUCKS.B3D']
    for path in argv:
        try:
            t = parse_b3d(path)
            h = stats(t)
            print('%-26s OK %6.2f%%  consumed=%d/%d  top=%d names=%d subs=%s' %
                  (path, 100.0 * t['consumed'] / t['total'], t['consumed'], t['total'],
                   len(t['top']), len(t['names']), dict(sorted(h.items()))))
            for w in t['warn'][:5]:
                print('    warn:', w)
        except Exception as e:
            print('%-26s FAIL %s' % (path, e))


if __name__ == '__main__':
    main(sys.argv[1:])

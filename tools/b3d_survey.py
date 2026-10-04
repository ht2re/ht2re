#!/usr/bin/env python3
"""Survey every [skip][333][name32][subtag] record head in a B3D body;
per-subtag payload size stats + examples."""
import struct, sys, collections

def is_name(b):
    if b[0] < 0x21 or b[0] > 0x7e: return False
    z = b.find(b'\x00')
    if z < 2 or z > 24: return False
    if not all(0x20 <= c <= 0x7e for c in b[:z]): return False
    return all(c == 0 for c in b[z:])

def survey(path):
    d = open(path, 'rb').read()
    nnames = struct.unpack_from('<I', d, 0x18)[0]
    body = 0x1c + nnames*32
    rec = []  # (pos_of_333, name, subtag)
    for p in range(body, len(d)-40, 4):
        if struct.unpack_from('<I', d, p)[0] != 0x14d: continue
        nm = d[p+4:p+36]
        if not is_name(nm): continue
        prev = struct.unpack_from('<I', d, p-4)[0]
        if prev not in (0x6f, 0x1bc, 0x22b): continue
        subtag = struct.unpack_from('<I', d, p+36)[0]
        rec.append((p, nm.split(b'\x00')[0].decode('latin1'), subtag))
    # payload size to next head
    sizes = collections.defaultdict(list)
    ex = {}
    for i, (p, nm, s) in enumerate(rec):
        nxt = rec[i+1][0] if i+1 < len(rec) else len(d)
        end = nxt-8  # exclude trailing [term][skip]
        end = nxt-8 if nxt > p+40 else len(d)
        if end <= p+40: end = min(len(d), nxt)
        n = end-(p+40)
        sizes[s].append(n)
        if s not in ex: ex[s] = (hex(p), nm, n)
    print('%-26s heads=%d subtags=%s' % (path, len(rec), sorted(sizes)))
    for s in sorted(sizes):
        ns = sizes[s]
        mn, mx = min(ns), max(ns)
        median = sorted(ns)[len(ns)//2]
        uni = '(var)' if mn != mx else ''
        print('  subtag %-3d n=%-5d size=%d..%d med=%d %s ex=%s/%s' %
              (s, len(ns), mn, mx, median, uni, ex[s][0], ex[s][1]))
    # dump raw example payload for var subtags
    for s in sorted(sizes):
        ns = sizes[s]
        if len(ns) > 1 or (ns[0] % 4):
            p, nm, _ = ex[s]
            words = struct.unpack_from('<%dI' % min(20, (ns[0])//4 if False else 20), d, p+40)
            words2 = struct.unpack_from('<%df' % 12, d, p+40)
            print('  --- subtag %d ex @%s name=%r payload:' % (s, hex(p), nm))
            for i in range(0, len(words), 6):
                print('    ', ' '.join('%08x' % w for w in words[i:i+6]))
    return rec

if __name__ == '__main__':
    for path in sys.argv[1:]:
        try: survey(path)
        except Exception as e: print('%s FAIL %s' % (path, e))
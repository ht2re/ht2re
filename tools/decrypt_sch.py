#!/usr/bin/env python3
"""Decryptor for Hard Truck 2 .SCH/.CNF encrypted scheme scripts.

Format: 4-byte magic 'SDTE' followed by stream-ciphered body.
Cipher: subtractive lagged-Fibonacci PRNG (55 dwords, lags 31),
keystream byte = low byte of updated state word XORed with ciphertext.
Key: constant 0x5cb0 (same for all game scripts).

Recovered from king.exe: init @0x40ee0, stream @0xb5640, port reader @0xb54b0.
"""
import sys

MASK32 = 0xFFFFFFFF
KEY = 0x5CB0


def seed(key=KEY):
    """Exact port of fcn.00040ee0 (55-slot table, stride 21, 4 passes)."""
    ctx = [0] * 57
    ctx[54] = key & MASK32
    ebx = key
    ecx = 1
    edi = 0
    while True:
        edx = edi % 55
        ebx = (ebx - ecx) & MASK32
        edi += 21
        ctx[edx] = ecx
        ecx = ebx
        ebx = ctx[edx]
        if edi >= 0x46E:
            break
    for _ in range(4):
        for k in range(55):
            ctx[k] = (ctx[k] - ctx[(k + 31) % 55]) & MASK32
    ctx[55] = 0
    ctx[56] = 31
    return ctx


def decrypt(data, key=KEY):
    if data[:4] != b'SDTE':
        raise ValueError('not SDTE encrypted')
    ctx = seed(key)
    i, j = 0, 31
    out = bytearray()
    for b in data[4:]:
        i = (i + 1) % 55
        j = (j + 1) % 55
        ctx[i] = (ctx[i] - ctx[j]) & MASK32
        out.append(b ^ (ctx[i] & 0xFF))
    return bytes(out)


def __main__():
    if len(sys.argv) < 2:
        print('usage: decrypt_sch.py <file> [outfile]')
        return 1
    data = open(sys.argv[1], 'rb').read()
    try:
        out = decrypt(data)
    except ValueError:
        sys.stdout.buffer.write(data)  # plaintext passthrough
        return 0
    if len(sys.argv) > 2:
        open(sys.argv[2], 'wb').write(out)
    else:
        sys.stdout.buffer.write(out)
    return 0


if __name__ == '__main__':
    sys.exit(__main__())

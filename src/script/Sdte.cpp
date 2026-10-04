#include "Sdte.h"
#include <cstring>

namespace ht2 {

bool Sdte::isEncrypted(const uint8_t* data, size_t n) {
    return n >= 4 && data[0] == 'S' && data[1] == 'D' && data[2] == 'T' && data[3] == 'E';
}

// exact port of fcn.00040ee0: 55-slot table, stride 21, four mixing passes
static void seed(uint32_t ctx[57], uint32_t key) {
    memset(ctx, 0, sizeof(uint32_t) * 57);
    ctx[54] = key;
    uint32_t ebx = key, ecx = 1;
    int edi = 0;
    while (true) {
        int edx = edi % 55;
        ebx = ebx - ecx;
        edi += 21;
        ctx[edx] = ecx;
        ecx = ebx;
        ebx = ctx[edx];
        if (edi >= 0x46E) break;
    }
    for (int pass = 0; pass < 4; ++pass)
        for (int k = 0; k < 55; ++k)
            ctx[k] = ctx[k] - ctx[(k + 31) % 55];
    ctx[55] = 0;    // i
    ctx[56] = 31;   // j
}

std::vector<uint8_t> Sdte::decrypt(const uint8_t* data, size_t n, uint32_t key) {
    if (!isEncrypted(data, n)) return std::vector<uint8_t>(data, data + n);
    uint32_t ctx[57];
    seed(ctx, key);
    int i = 0, j = 31;
    std::vector<uint8_t> out;
    out.reserve(n - 4);
    for (size_t k = 4; k < n; ++k) {
        i = (i + 1) % 55;
        j = (j + 1) % 55;
        ctx[i] = ctx[i] - ctx[j];
        out.push_back((uint8_t)(data[k] ^ (ctx[i] & 0xFF)));
    }
    return out;
}

} // namespace ht2

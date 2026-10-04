#include "Mask.h"
#include "../util/Log.h"
#include <cstring>
#include <cmath>

namespace ht2 {

static const size_t MASK_HEADER = 0x308;   // 4 tag + 4 w/h + 768 palette
static const size_t PALETTE_BYTES = 768;

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
// mask==0 -> 255 (opaque); otherwise extract and rescale to 0..255
static uint8_t expandChannel(uint16_t v, uint32_t mask) {
    if (!mask) return 255;
    uint32_t shift = 0;
    while (!((mask >> shift) & 1u)) ++shift;
    uint32_t m = mask >> shift;
    uint32_t bits = 0;
    while ((m >> bits) & 1u) ++bits;
    uint32_t raw = (v & mask) >> shift;
    uint32_t maxv = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    return (uint8_t)(raw * 255u / (maxv ? maxv : 1u));
}

// The RLE from king.exe @0x1bc1a9.
static bool rleExpand(const uint8_t* src, size_t n, int width, int height, int depth,
                      std::vector<uint8_t>& out, size_t& consumed) {
    out.assign((size_t)width * height * depth, 0);
    size_t i = 0, pos = 0;
    for (int y = 0; y < height; ++y) {
        int x = 0;
        while (x < width) {
            if (i >= n) return false;
            uint8_t c = src[i++];
            uint32_t cnt = c & 0x7Fu;
            size_t run = (size_t)cnt * depth;
            if (c & 0x80u) {
                x += (int)cnt;          // already zero-filled
                pos += run;
            } else {
                if (i + run > n) return false;
                if (pos + run <= out.size()) memcpy(out.data() + pos, src + i, run);
                i += run;
                x += (int)cnt;
                pos += run;
            }
        }
    }
    consumed = i;
    return true;
}

bool Mask::decode(const uint8_t* blob, size_t size, DecodedImage& out) {
    if (size < MASK_HEADER) return false;
    int depth;
    if (!memcmp(blob, "MASK", 4) || !memcmp(blob, "MSKR", 4) || !memcmp(blob, "MSK8", 4)) depth = 1;
    else if (!memcmp(blob, "MS16", 4)) depth = 2;
    else return false;

    out = DecodedImage();
    int w = rd16(blob + 4), h = rd16(blob + 6);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return false;
    const uint8_t* pal = blob + 8;
    const uint8_t* src = blob + MASK_HEADER;
    size_t n = size - MASK_HEADER;

    std::vector<uint8_t> raw;
    size_t consumed = 0;
    if (!rleExpand(src, n, w, h, depth, raw, consumed)) return false;

    out.width = w; out.height = h;
    out.consumed = consumed;
    out.total = n;
    out.rgba.assign((size_t)w * h * 4, 0);

    if (depth == 1) {
        for (size_t p = 0; p < (size_t)w * h; ++p) {
            uint8_t idx = raw[p];
            const uint8_t* c = pal + (size_t)idx * 3;
            out.rgba[p * 4 + 0] = c[0];
            out.rgba[p * 4 + 1] = c[1];
            out.rgba[p * 4 + 2] = c[2];
            out.rgba[p * 4 + 3] = idx == 0 ? 0 : 255;   // palette entry 0 = key
        }
        out.format = "8bpp paletted";
    } else {
        // "PFRM" chunk with channel masks follows the pixel data, then "ENDR"
        uint32_t rmask = 0x7C00, gmask = 0x03E0, bmask = 0x001F, amask = 0;
        size_t rest = consumed;
        if (rest + 8 <= n && !memcmp(src + rest, "PFRM", 4)) {
            uint32_t plen = rd32(src + rest + 4);
            if (plen >= 4 && rest + 8 + plen <= n) {
                rmask = rd32(src + rest + 8);
                if (plen >= 8) gmask = rd32(src + rest + 12);
                if (plen >= 12) bmask = rd32(src + rest + 16);
                if (plen >= 16) amask = rd32(src + rest + 20);
                out.consumed = rest + 8 + plen;
                if (out.consumed + 8 <= n && !memcmp(src + out.consumed, "ENDR", 4))
                    out.consumed += 8;
            }
        }
        for (size_t p = 0; p < (size_t)w * h; ++p) {
            uint16_t v = rd16(raw.data() + p * 2);
            out.rgba[p * 4 + 0] = expandChannel(v, rmask);
            out.rgba[p * 4 + 1] = expandChannel(v, gmask);
            out.rgba[p * 4 + 2] = expandChannel(v, bmask);
            out.rgba[p * 4 + 3] = amask ? expandChannel(v, amask) : (v == 0 ? 0 : 255);
        }
        out.format = "16bpp";
    }
    return true;
}

bool Texture::decode(const uint8_t* blob, size_t size, DecodedImage& out, int key565) {
    if (size < 32) return false;
    out = DecodedImage();
    // (a) chunked 16bpp "LOFF"
    if (rd16(blob) == 12 && rd16(blob + 2) == 2 && !memcmp(blob + 18, "LOFF", 4)) {
        int w = rd16(blob + 12), h = rd16(blob + 14);
        if (rd16(blob + 16) != 0x2010) return false;
        uint32_t next = rd32(blob + 26);
        if (w <= 0 || h <= 0) return false;
        if ((size_t)next != (size_t)30 + 2 * (size_t)w * h) return false;
        size_t pixBytes = 2 * (size_t)w * h;
        if (30 + pixBytes > size) return false;
        const uint8_t* pix = blob + 30;
        uint32_t rmask = 0xF800, gmask = 0x07E0, bmask = 0x001F, amask = 0;
        for (size_t q = next; q + 8 <= size; ++q) {
            if (!memcmp(blob + q, "PFRM", 4)) {
                uint32_t plen = rd32(blob + q + 4);
                if (plen >= 4 && q + 8 + plen <= size) {
                    rmask = rd32(blob + q + 8);
                    if (plen >= 8) gmask = rd32(blob + q + 12);
                    if (plen >= 12) bmask = rd32(blob + q + 16);
                    if (plen >= 16) amask = rd32(blob + q + 20);
                }
                break;
            }
        }
        // No alpha mask: the engine relies on a DirectDraw *colour key*.  The
        // key colour used by every sprite-like texture (foliage, bushes,
        // fences, glows) is RGB565 0x31C2 = RGB(49,56,16); it covers 8%..68% of
        // those images.  Terrain textures contain the same value only as a few
        // stray pixels (<=0.002%), so the key is applied only when it actually
        // covers >=1% of the image - that keeps rocks/shore intact while trees,
        // bushes and signs are cut out.
        // The key pixel value is supplied by the caller (material `ttx`); no
        // heuristic is applied, which used to punch holes in opaque art
        // (Airport_planes, crpart1..4, wall_side1, ...).
        const bool hasKey = key565 >= 0;
        const uint16_t key = (uint16_t)(key565 & 0xFFFF);
        out.width = w; out.height = h;
        out.rgba.assign((size_t)w * h * 4, 255);
        for (size_t p = 0; p < (size_t)w * h; ++p) {
            uint16_t v = rd16(pix + p * 2);
            out.rgba[p * 4 + 0] = expandChannel(v, rmask);
            out.rgba[p * 4 + 1] = expandChannel(v, gmask);
            out.rgba[p * 4 + 2] = expandChannel(v, bmask);
            out.rgba[p * 4 + 3] = amask ? expandChannel(v, amask) : (hasKey && v == key ? 0 : 255);
        }
        out.consumed = size; out.total = size;
        out.format = hasKey ? "LOFF 16bpp colour-keyed" : "LOFF 16bpp";
        return true;
    }
    // (b) paletted 8bpp bitmap
    if (rd16(blob) == 0x0100 && rd16(blob + 2) == 0x0001 && rd16(blob + 16) == 0x2008) {
        int w = rd16(blob + 12), h = rd16(blob + 14);
        if (w <= 0 || h <= 0) return false;
        size_t need = 18 + PALETTE_BYTES + (size_t)w * h;
        if (need > size) return false;
        const uint8_t* pal = blob + 18;
        const uint8_t* idx = blob + 18 + PALETTE_BYTES;
        out.width = w; out.height = h;
        out.rgba.assign((size_t)w * h * 4, 0);
        for (size_t p = 0; p < (size_t)w * h; ++p) {
            uint8_t v = idx[p];
            const uint8_t* c = pal + (size_t)v * 3;   // B,G,R
            out.rgba[p * 4 + 0] = c[2];
            out.rgba[p * 4 + 1] = c[1];
            out.rgba[p * 4 + 2] = c[0];
            out.rgba[p * 4 + 3] = v == 0 ? 0 : 255;
        }
        out.consumed = need; out.total = size;
        out.format = "8bpp paletted";
        return true;
    }
    return false;
}

} // namespace ht2

#pragma once
// .msk (MASK) / .txr (TEXTURE) blob decoding.
//
// Format derived from king.exe and validated byte-exactly over every mask and
// texture of every shipped .res file (tools/res_msk.py, docs/msk_format.md).
//
// MASK blob:
//    +0x000 char[4]  tag   "MASK" | "MSKR" | "MSK8" (8bpp) | "MS16" (16bpp)
//    +0x004 u16      width
//    +0x006 u16      height
//    +0x008 u8[768]  256 RGB triples (palette for 8bpp; zero for 16bpp)
//    +0x308 ...      RLE stream
//    [MS16 only]     "PFRM" chunk (channel masks) + "ENDR"
// RLE (king.exe 0x1bc1a9): one control byte `c` per run, n = c & 0x7f.
//    c & 0x80 -> n transparent pixels; otherwise n literal pixels followed by
//    n*depth raw bytes. Rows are decoded independently (x wraps at width).
//
// TEXTURE blobs come in two variants: chunked 16bpp "LOFF" (MENV/COMMON) and
// paletted 8bpp (the 172 LOAD.RES entries).
#include <cstdint>
#include <string>
#include <vector>

namespace ht2 {

struct DecodedImage {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;   // width*height*4
    std::string format;          // human readable
    size_t consumed = 0;         // payload bytes consumed by the decoder
    size_t total = 0;            // payload bytes available
};

class Mask {
public:
    // `blob` points at the payload (after the RES u32 size field), so blob[0..4)
    // is the ASCII tag. Returns false when the blob is not a mask / is corrupt.
    static bool decode(const uint8_t* blob, size_t size, DecodedImage& out);
};

class Texture {
public:
    // `key565` is the DirectDraw colour key the engine puts on this texture
    // surface (RGB565), or -1 for an opaque texture.  The key comes from the
    // .res MATERIALS table ("ttx N [col C]" -> palette entry C-1), not from the
    // texture data: see docs/txr_alpha.md.
    static bool decode(const uint8_t* blob, size_t size, DecodedImage& out, int key565 = -1);
};

} // namespace ht2

// rvid_pack - build the engine's decoder-free movie frame pack (.rvid) from a
// stream of raw RGB565 frames (the output of `ffmpeg -pix_fmt rgb565le`).
//
//   rvid_pack [--raw] <width> <height> <fps> <in.raw565> <out.rvid>
//
// The container is documented in docs/video.md; the short version:
//
//   off  size  field
//   0    4     magic "RVID"
//   4    4     version (1)
//   8    4     width
//   12   4     height
//   16   4     fps
//   20   4     frameCount
//   24   4     flags      bit0 = frames are RLE coded, else raw RGB565
//   28   4     tableOffset (32)
//   32   4*N   size table: u32 per frame, bit31 set = that frame is stored raw
//   32+4N      frame payloads, back to back, in display order
//
// A frame payload is either w*h RGB565 pixels (top-down, little-endian, R in
// bits 15..11) or a PackBits-style byte stream over those 16-bit pixels:
//
//   control < 0x80 : literal run of (control + 1) pixels, (control+1)*2 bytes
//   control >=0x80 : repeat run of ((control & 0x7F) + 2) pixels, 2 bytes value
//
// The encoder always picks the smaller of the two per frame, so RLE can never
// make a frame bigger than raw (a noisy frame simply stores raw).
//
// No dependency beyond the C++ standard library: make_video.sh compiles this
// file on demand with the system compiler.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>

static bool writeU32(FILE* f, uint32_t v) { return fwrite(&v, 4, 1, f) == 1; }

// Encodes one RGB565 frame (w*h pixels, host little-endian) into `out`.
// Returns false when the frame is better stored raw.
static bool rleEncode(const uint16_t* px, size_t n, std::vector<uint8_t>& out) {
    out.clear();
    size_t i = 0;
    while (i < n) {
        // length of the constant run starting at i (max 129)
        size_t j = i + 1;
        while (j < n && px[j] == px[i] && j - i < 129) ++j;
        if (j - i >= 2) {
            out.push_back((uint8_t)(0x80 | (j - i - 2)));
            out.push_back((uint8_t)(px[i] & 0xFF));
            out.push_back((uint8_t)(px[i] >> 8));
            i = j;
        } else {
            // literal run: stop just before the next run of >= 2 equal pixels
            size_t k = i;
            while (k < n && k - i < 128) {
                if (k + 1 < n && px[k + 1] == px[k]) break;
                ++k;
            }
            if (k == i) ++k;                       // never emit an empty run
            out.push_back((uint8_t)(k - i - 1));
            for (size_t m = i; m < k; ++m) {
                out.push_back((uint8_t)(px[m] & 0xFF));
                out.push_back((uint8_t)(px[m] >> 8));
            }
            i = k;
        }
    }
    return out.size() < n * 2;
}

int main(int argc, char** argv) {
    bool forceRaw = false;
    int arg = 1;
    for (; arg < argc; ++arg) {
        if (std::strcmp(argv[arg], "--raw") == 0) forceRaw = true;
        else break;
    }
    if (argc - arg != 5) {
        std::fprintf(stderr,
            "usage: rvid_pack [--raw] <width> <height> <fps> <in.raw565> <out.rvid>\n");
        return 2;
    }
    const int w = std::atoi(argv[arg + 0]);
    const int h = std::atoi(argv[arg + 1]);
    const int fps = std::atoi(argv[arg + 2]);
    if (w <= 0 || h <= 0 || fps <= 0) { std::fprintf(stderr, "rvid_pack: bad geometry\n"); return 2; }

    FILE* in = std::fopen(argv[arg + 3], "rb");
    if (!in) { std::fprintf(stderr, "rvid_pack: cannot open %s\n", argv[arg + 3]); return 1; }
    std::fseek(in, 0, SEEK_END);
    const long long bytes = std::ftell(in);
    std::fseek(in, 0, SEEK_SET);
    const size_t frameBytes = (size_t)w * (size_t)h * 2;
    if (bytes <= 0 || (size_t)bytes % frameBytes != 0) {
        std::fprintf(stderr, "rvid_pack: %s is %lld bytes, not a multiple of the %zu byte frame\n",
                     argv[arg + 3], bytes, frameBytes);
        std::fclose(in);
        return 1;
    }
    const size_t frames = (size_t)bytes / frameBytes;

    FILE* out = std::fopen(argv[arg + 4], "wb");
    if (!out) { std::fprintf(stderr, "rvid_pack: cannot write %s\n", argv[arg + 4]); std::fclose(in); return 1; }

    // header + size table (patched at the end)
    unsigned char hdr[32];
    std::memset(hdr, 0, sizeof(hdr));
    std::memcpy(hdr, "RVID", 4);
    const uint32_t u32[7] = {1u, (uint32_t)w, (uint32_t)h, (uint32_t)fps,
                             (uint32_t)frames, forceRaw ? 0u : 1u, 32u};
    std::memcpy(hdr + 4, u32, sizeof(u32));
    std::fwrite(hdr, 1, sizeof(hdr), out);
    std::vector<uint32_t> table(frames, 0);
    std::fwrite(table.data(), 4, frames, out);

    std::vector<uint16_t> px(frameBytes / 2);
    std::vector<uint8_t> enc;
    size_t total = 32 + 4 * frames;
    for (size_t f = 0; f < frames; ++f) {
        if (std::fread(px.data(), 1, frameBytes, in) != frameBytes) {
            std::fprintf(stderr, "rvid_pack: short read at frame %zu\n", f);
            std::fclose(in); std::fclose(out); return 1;
        }
        uint32_t sz;
        if (!forceRaw && rleEncode(px.data(), px.size(), enc)) {
            std::fwrite(enc.data(), 1, enc.size(), out);
            sz = (uint32_t)enc.size();
        } else {
            std::fwrite(px.data(), 1, frameBytes, out);
            sz = (uint32_t)frameBytes | 0x80000000u;
        }
        table[f] = sz;
        total += (sz & 0x7FFFFFFFu);
        if ((f % 200) == 0 || f + 1 == frames)
            std::fprintf(stderr, "\r  rvid_pack: frame %zu/%zu (%.1f MB)", f + 1, frames, total / 1e6);
    }
    std::fprintf(stderr, "\n");
    std::fseek(out, 32, SEEK_SET);               // seek also flushes the stream
    std::fwrite(table.data(), 4, frames, out);
    std::fclose(in);
    // report the real size instead of trusting our own accounting
    std::fseek(out, 0, SEEK_END);
    const long long outBytes = std::ftell(out);
    std::fclose(out);
    std::fprintf(stderr, "rvid_pack: %s  %dx%d @%dfps  %zu frames  %lld bytes (%.1f MB)\n",
                 argv[arg + 4], w, h, fps, frames, outBytes, outBytes / 1e6);
    return 0;
}

// video_check - standalone harness for the movie player and the loading screen.
//
//   tools/build_video_check.sh          (builds and runs it)
//   tools/out/video_check [mediaDir] [dataDir]
//
// It renders the loading screen and a decoded movie frame off screen
// (SDL_WINDOW_HIDDEN + Renderer::screenshot), and exercises the failure paths
// (missing pack, bogus frame index, drawing before begin()).  Exit code 0 means
// every check passed.
#include "../src/gfx/Renderer.h"
#include "../src/ui/Font.h"
#include "../src/ui/Loading.h"
#include "../src/ui/Video.h"
#include "../src/util/Log.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ht2;

static int failures = 0;

static void check(const char* what, bool ok) {
    std::printf("[%s] %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

// --- an independent encoder for the documented container --------------------
// Written from docs/video.md, not shared with tools/rvid_pack.cpp, so a mismatch
// between the two means the format documentation is wrong.
static void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back((uint8_t)(x >> (8 * i)));
}

// PackBits over 16-bit pixels, repeat runs only (plus one trailing literal).
static std::vector<uint8_t> rleRepeatOnly(const std::vector<uint16_t>& px) {
    std::vector<uint8_t> out;
    size_t i = 0;
    while (i < px.size()) {
        size_t j = i + 1;
        while (j < px.size() && px[j] == px[i] && j - i < 129) ++j;
        if (j - i >= 2) {
            out.push_back((uint8_t)(0x80 | (j - i - 2)));
            out.push_back((uint8_t)(px[i] & 0xFF));
            out.push_back((uint8_t)(px[i] >> 8));
            i = j;
        } else {
            out.push_back(0x00);                    // literal run of 1 pixel
            out.push_back((uint8_t)(px[i] & 0xFF));
            out.push_back((uint8_t)(px[i] >> 8));
            ++i;
        }
    }
    return out;
}

static bool writeSynthPack(const std::string& path, int w, int h,
                           const std::vector<std::vector<uint16_t>>& frames,
                           const std::vector<bool>& raw) {
    std::vector<uint8_t> hdr;
    hdr.push_back('R'); hdr.push_back('V'); hdr.push_back('I'); hdr.push_back('D');
    put32(hdr, 1);                                  // version
    put32(hdr, (uint32_t)w);
    put32(hdr, (uint32_t)h);
    put32(hdr, 12);                                 // fps
    put32(hdr, (uint32_t)frames.size());
    put32(hdr, 1);                                  // flags: rle coded
    put32(hdr, 32);                                 // tableOffset
    if (hdr.size() != 32) return false;

    std::vector<uint32_t> table;
    std::vector<std::vector<uint8_t>> payloads;
    for (size_t i = 0; i < frames.size(); ++i) {
        std::vector<uint8_t> pl;
        if (raw[i]) {
            for (uint16_t v : frames[i]) {
                pl.push_back((uint8_t)(v & 0xFF));
                pl.push_back((uint8_t)(v >> 8));
            }
            table.push_back((uint32_t)pl.size() | 0x80000000u);
        } else {
            pl = rleRepeatOnly(frames[i]);
            table.push_back((uint32_t)pl.size());
        }
        payloads.push_back(pl);
    }
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(hdr.data(), 1, hdr.size(), f);
    std::fwrite(table.data(), 4, table.size(), f);
    for (auto& p : payloads) std::fwrite(p.data(), 1, p.size(), f);
    std::fclose(f);
    return true;
}

static uint8_t expand5(uint16_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
static uint8_t expand6(uint16_t v) { return (uint8_t)((v << 2) | (v >> 4)); }

int main(int argc, char** argv) {
    const std::string media = argc > 1 ? argv[1] : "build/media";
    const std::string data = argc > 2 ? argv[2] : "original";

    Renderer r;
    if (!r.init(1280, 720, /*offscreen=*/true)) {
        std::printf("FAIL: Renderer::init\n");
        return 2;
    }
    Font font;
    font.initBuiltin(r);
    // the original bitmap font, when available, is what the real UI uses
    font.loadOriginal(r, data);

    // ---- synthetic pack: known pixels through both decode paths ------------
    {
        const int w = 97, h = 61;                   // odd sizes, not a multiple of 129
        const uint16_t solid = (uint16_t)((3 << 11) | (40 << 5) | 19);
        std::vector<std::vector<uint16_t>> frames(3);
        frames[0].assign((size_t)w * h, solid);     // RLE: all repeat runs
        frames[1].resize((size_t)w * h);            // RAW flag path
        frames[2].resize((size_t)w * h);            // RLE with alternating pixels
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = (size_t)y * w + x;
                frames[1][i] = (uint16_t)(((x * 31 / (w - 1)) << 11) |
                                          ((y * 63 / (h - 1)) << 5) |
                                          (uint16_t)((x + y) & 0x1F));
                // runs of 20 pixels broken by single different pixels: exercises
                // both the repeat and the literal arm of the RLE decoder
                frames[2][i] = (i % 21 == 0) ? (uint16_t)(0x0800 + (i * 13) % 4096)
                                             : (uint16_t)0x1234;
            }
        const std::vector<bool> rawFlag = {false, true, false};
        const std::string synth = "tools/out/synth.rvid";
        if (!writeSynthPack(synth, w, h, frames, rawFlag)) {
            check("synth: pack written", false);
        } else {
            Video s;
            check("synth: opens", s.open("tools/out", "synth"));
            check("synth: geometry", s.clip().w == w && s.clip().h == h && s.clip().frames == 3);
            bool allOk = true;
            for (int f = 0; f < 3; ++f) {
                if (!s.loadFrame((size_t)f)) { allOk = false; break; }
                const std::vector<uint8_t>& px = s.rgba();
                for (size_t i = 0; i < frames[f].size() && allOk; ++i) {
                    const uint16_t v = frames[f][i];
                    allOk = px[i * 4 + 0] == expand5((uint16_t)((v >> 11) & 0x1F)) &&
                            px[i * 4 + 1] == expand6((uint16_t)((v >> 5) & 0x3F)) &&
                            px[i * 4 + 2] == expand5((uint16_t)(v & 0x1F)) &&
                            px[i * 4 + 3] == 255;
                }
            }
            check("synth: raw and RLE frames decode bit-exactly", allOk);
            // a truncated pack must be refused
            std::FILE* src = std::fopen(synth.c_str(), "rb");
            std::vector<uint8_t> bytes;
            if (src) {
                uint8_t buf[4096];
                size_t n;
                while ((n = std::fread(buf, 1, sizeof(buf), src)) > 0) bytes.insert(bytes.end(), buf, buf + n);
                std::fclose(src);
                bytes.resize(bytes.size() - 3);
                std::FILE* bad = std::fopen("tools/out/synth_bad.rvid", "wb");
                if (bad) { std::fwrite(bytes.data(), 1, bytes.size(), bad); std::fclose(bad); }
                Video b;
                check("synth: truncated pack is rejected", !b.open("tools/out", "synth_bad"));
            }
        }
    }

    // ---- loading screen, normal progress -----------------------------------
    Loading ld;
    check("loading: inactive before begin()", !ld.active());
    ld.draw(r, font);                       // must be a no-op, not a crash
    check("loading: draw before begin() is a no-op", true);
    ld.begin("ЗАГРУЗКА МИРА");
    ld.update(0.35f);
    ld.step(413, 1084, "ENV/aa.b3d: 1084 кадра -> 413");
    check("loading: active after begin()", ld.active());
    check("loading: progress 38%", ld.progress() > 0.37f && ld.progress() < 0.39f);
    r.clear();
    ld.draw(r, font);
    check("loading: screenshot", r.screenshot("tools/out/loading.bmp"));

    // ---- loading screen, indeterminate ------------------------------------
    Loading marquee;
    marquee.begin("");
    marquee.update(0.9f);
    marquee.step(0, 0, "GROUNDMAP: coast tiles");
    marquee.setNote("this stage does not know its length");
    r.clear();
    marquee.draw(r, font);
    check("loading: indeterminate screenshot", r.screenshot("tools/out/loading_marquee.bmp"));
    marquee.end();
    check("loading: inactive after end()", !marquee.active());

    // ---- loading screen, nearly finished (contrast of the percent label) ---
    Loading full;
    full.begin("ЗАГРУЗКА МИРА");
    full.update(1.7f);
    full.step(1190, 1290, "ENV/trp.b3d: submesh 1190 of 1290");
    r.clear();
    full.draw(r, font);
    check("loading: 92% screenshot", r.screenshot("tools/out/loading_full.bmp"));
    full.end();

    // ---- movie playback ----------------------------------------------------
    Video v;
    check("video: missing clip returns false", !v.open(media, "does_not_exist"));
    check("video: update() without a clip returns false", !v.update(r, 1.0f / 60.0f));
    check("video: loadFrame() without a clip returns false", !v.loadFrame(0));
    check("video: not playing initially", !v.playing());

    if (!v.open(media, "kotr")) {
        std::printf("FAIL: build/media/kotr.rvid missing - run tools/make_video.sh\n");
        ++failures;
    } else {
        const Video::Clip& c = v.clip();
        std::printf("      clip %s %dx%d @%dfps %d frames (%.1fs)\n",
                    c.video.c_str(), c.w, c.h, c.fps, c.frames, v.duration());
        check("video: geometry is sane", c.w > 0 && c.h > 0 && c.fps > 0 && c.frames > 0);
        check("video: audio track found", !c.audio.empty());
        check("video: loadFrame(middle) decodes", v.loadFrame((size_t)c.frames / 2));
        check("video: rgba() is w*h*4", v.rgba().size() == (size_t)c.w * c.h * 4);
        check("video: out-of-range frame rejected", !v.loadFrame((size_t)c.frames + 10));

        // frame 900 has real content in the source (a truck on the road)
        v.play(/*withAudio=*/false);
        check("video: playing() after play()", v.playing());
        int guard = 0;
        while (v.playing() && v.frame() < 900 && guard++ < 4000)
            v.update(r, 1.0f / (float)c.fps);
        check("video: reached frame 900 while still playing",
              v.playing() && v.frame() == 900);
        check("video: screenshot", r.screenshot("tools/out/video_frame900.bmp"));

        // skipping stops playback without touching the frame that is up
        v.skip();
        check("video: skip() stops playback", !v.playing());
        check("video: update() after skip returns false", !v.update(r, 1.0f / 60.0f));

        // running past the end reports "finished"
        v.play(false);
        guard = 0;
        while (v.playing() && guard++ < c.frames + 10) v.update(r, 1.0f / (float)c.fps);
        check("video: reaching the end returns false", !v.playing());
        v.stop();
        check("video: stop() resets", !v.playing() && v.frame() == 0);
    }

    // ---- the demo clip -----------------------------------------------------
    Video d;
    if (d.open(media, "kotrDemo")) {
        const Video::Clip& c = d.clip();
        std::printf("      clip %s %dx%d @%dfps %d frames (%.1fs)\n",
                    c.video.c_str(), c.w, c.h, c.fps, c.frames, d.duration());
        check("demo: geometry is sane", c.w > 0 && c.h > 0 && c.frames > 0);
        d.play(false);
        int guard = 0;
        while (d.playing() && d.frame() < 900 && guard++ < 4000)
            d.update(r, 1.0f / (float)c.fps);
        check("demo: screenshot", r.screenshot("tools/out/video_demo_frame900.bmp"));
        d.stop();
    } else {
        std::printf("      (kotrDemo.rvid not present, skipped)\n");
    }

    r.shutdown();
    std::printf("%s (%d failure%s)\n", failures ? "FAILURES" : "ALL CHECKS PASSED",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

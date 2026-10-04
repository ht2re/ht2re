#include "Loading.h"
#include "Font.h"
#include "../gfx/Renderer.h"
#include "../util/Log.h"
#include "../res/Res.h"
#include "../res/Mask.h"
#include <vector>
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ht2 {

// The original UI is laid out on an 800x600 canvas (see Menu); the loading
// screen uses the same virtual canvas so it scales with the window the same way.
static const float VW = 800.0f, VH = 600.0f;

// amber/gold of the original's button art, plus the dark steel background
static const float kAmberR = 0.95f, kAmberG = 0.72f, kAmberB = 0.20f;
static const float kGoldR = 1.00f, kGoldG = 0.88f, kGoldB = 0.42f;

void Loading::begin(const std::string& what) {
    active_ = true;
    what_ = what.empty() ? std::string("ЗАГРУЗКА") : what;
    item_.clear();
    note_.clear();
    done_ = 0;
    total_ = 1;
    t_ = 0.0f;
}

void Loading::step(int done, int total, const std::string& item) {
    done_ = done;
    total_ = total;
    item_ = item;
}

void Loading::end() {
    active_ = false;
    item_.clear();
    note_.clear();
    done_ = 0;
    total_ = 1;
}

void Loading::update(float dt) {
    if (dt > 0.0f && dt < 1.0f) t_ += dt;   // ignore absurd deltas
}

float Loading::progress() const {
    if (total_ <= 0) return 0.0f;
    float p = (float)done_ / (float)total_;
    return std::max(0.0f, std::min(1.0f, p));
}

void Loading::frameRect(Renderer& r, unsigned tex, float x, float y, float w, float h,
                        float t, float cr, float cg, float cb, float ca) const {
    r.quad2D(tex, x, y, w, t, cr, cg, cb, ca);                 // top
    r.quad2D(tex, x, y + h - t, w, t, cr, cg, cb, ca);         // bottom
    r.quad2D(tex, x, y + t, t, h - 2 * t, cr, cg, cb, ca);     // left
    r.quad2D(tex, x + w - t, y + t, t, h - 2 * t, cr, cg, cb, ca);  // right
}

void Loading::bar(Renderer& r, float x, float y, float w, float h,
                  float pct, bool indeterminate) const {
    const unsigned white = r.whiteTex();
    // inset track
    r.quad2D(white, x, y, w, h, 0.05f, 0.06f, 0.09f, 1.0f);
    frameRect(r, white, x, y, w, h, 1.0f, 0.28f, 0.26f, 0.22f, 1.0f);

    const float ix = x + 2.0f, iy = y + 2.0f, iw = w - 4.0f, ih = h - 4.0f;
    if (iw <= 2.0f || ih <= 2.0f) return;

    float fx = ix, fw = iw * pct;
    if (indeterminate) {
        // a segment sweeping across, so a stage of unknown length still moves
        const float seg = iw * 0.28f;
        const float phase = std::fmod(t_ * 0.55f, 1.0f);
        fx = ix - seg + (iw + seg) * phase;
        if (fx < ix) { fw = std::max(0.0f, seg - (ix - fx)); fx = ix; }
        else fw = std::min(seg, ix + iw - fx);
    }
    if (fw > 0.0f) {
        // fill: amber body with a lighter top strip and a moving highlight
        r.quad2D(white, fx, iy, fw, ih, kAmberR * 0.85f, kAmberG * 0.85f, kAmberB * 0.85f, 1.0f);
        r.quad2D(white, fx, iy, fw, ih * 0.38f, kGoldR, kGoldG, kGoldB, 0.85f);
        const float sweep = std::fmod(t_ * 0.35f, 1.0f);
        const float sw = std::max(6.0f, fw * 0.10f);
        const float sx = fx + (fw + sw) * sweep - sw;
        const float x0 = std::max(fx, sx), x1 = std::min(fx + fw, sx + sw);
        if (x1 > x0) r.quad2D(white, x0, iy, x1 - x0, ih, 1.0f, 1.0f, 0.92f, 0.35f);
    }
}

// Loads the original loading art once.  The App calls draw() without a data
// path, so a few candidates are tried (the game is normally started from the
// repository root with --data original).
bool Loading::loadArt(Renderer& r, const std::string& dataDir) {
    if (bg_ && runner_) return true;
    Res res;
    const char* suffix[2] = {"/COMMON/ind16.RES", "/COMMON/ind15.RES"};
    bool ok = false;
    for (int i = 0; i < 2 && !ok; ++i) {
        std::string p = dataDir + suffix[i];
        if (!res.load(p)) continue;
        auto upload = [&](const char* name, unsigned& out) {
            const Res::Entry* e = res.find(name);
            if (!e) return false;
            DecodedImage img;
            if (e->kind == Res::Kind::Texture) {
                if (!Texture::decode(res.data(*e), e->size, img)) return false;
            } else if (e->kind == Res::Kind::Mask) {
                if (!Mask::decode(res.data(*e), e->size, img)) return false;
            } else return false;
            out = r.createTexture(img.rgba.data(), img.width, img.height);
            return out != 0;
        };
        ok = upload("m16\\loadpix.txr", bg_) && upload("m16\\loadrunner.txr", runner_);
    }
    artTried_ = true;
    if (ok) LOG("loading: original art loaded (loadpix + loadrunner)");
    else LOG("loading: COMMON/ind16.RES unavailable, using the procedural screen");
    return ok;
}

void Loading::draw(Renderer& r, Font& font) {
    if (!active_) return;                  // nothing to show
    if (!artTried_) {
        // first call: try the default locations
        loadArt(r, "original");
        if (!bg_) {
            const char* base = SDL_GetBasePath();
            if (base) loadArt(r, std::string(base) + "original");
        }
    }
    if (r.width_ <= 0 || r.height_ <= 0) return;
    if (r.whiteTex() == 0) return;         // Renderer::init() has not run yet

    const int vpW = r.width_, vpH = r.height_;
    if (bg_ && runner_) {
        r.begin2D();
        // the art is authored for 1600x1200; centre loadpix 1:1 (cropped on
        // smaller windows) and reveal the runner panorama left->right exactly
        // like the original: width = screenW * progress / target at
        // y = (screenH - 1200)/2 + 552
        const float artW = 1600.0f, artH = 1200.0f;
        const float ox = (vpW - artW) * 0.5f, oy = (vpH - artH) * 0.5f;
        r.quad2D(bg_, ox, oy, artW, artH, 1, 1, 1, 1);
        float prog = total_ > 0 ? (float)done_ / (float)total_ : 0.0f;
        prog = std::min(1.0f, std::max(0.0f, prog));
        if (prog > 0.0f) {
            const float rw = artW * prog;
            r.quad2D(runner_, 0, oy + 552.0f, rw, 97.0f,
                     1, 1, 1, 1, 0.0f, 0.0f, prog, 1.0f);
        }
        r.end2D();
        return;                             // the art carries the caption itself
    }
    const float sc = std::min((float)vpW / VW, (float)vpH / VH);
    const float offX = ((float)vpW - VW * sc) * 0.5f;
    const float offY = ((float)vpH - VH * sc) * 0.5f;
    auto X = [&](float v) { return offX + v * sc; };
    auto Y = [&](float v) { return offY + v * sc; };

    r.begin2D();
    const unsigned white = r.whiteTex();

    // 1) dark backgrounds: the whole window, then the letterboxed canvas with a
    //    subtle vertical gradient so it does not look like a flat black frame.
    r.quad2D(white, 0, 0, (float)vpW, (float)vpH, 0.02f, 0.025f, 0.035f, 1.0f);
    r.quad2D(white, X(0), Y(0), VW * sc, VH * sc, 0.06f, 0.075f, 0.105f, 1.0f);
    r.quad2D(white, X(0), Y(0), VW * sc, VH * 0.45f * sc, 0.10f, 0.12f, 0.16f, 0.55f);

    // 2) thin amber rules top and bottom, as on the original's dialogs
    r.quad2D(white, X(0), Y(VH * 0.30f), VW * sc, std::max(1.0f, 2.0f * sc), kAmberR, kAmberG, kAmberB, 0.30f);
    r.quad2D(white, X(0), Y(VH * 0.82f), VW * sc, std::max(1.0f, 2.0f * sc), kAmberR, kAmberG, kAmberB, 0.30f);

    // 3) heading
    const float headScale = 2.4f * sc;
    font.drawCentered(r, what_, X(VW * 0.5f), Y(238.0f), headScale, kGoldR, kGoldG, kGoldB, 1.0f);

    // 4) progress bar (460x26 on the 800x600 canvas) with the percent inside
    const float bx = 170.0f, by = 300.0f, bw = 460.0f, bh = 26.0f;
    const bool indeterminate = total_ <= 0;
    const float pct = progress();
    bar(r, X(bx), Y(by), bw * sc, bh * sc, pct, indeterminate);

    char pctText[32];
    if (indeterminate) std::snprintf(pctText, sizeof(pctText), "--");
    else std::snprintf(pctText, sizeof(pctText), "%d%%", (int)std::lround(pct * 100.0f));
    const float ps = 1.6f * sc;
    const float ph = font.lineHeight(ps);
    const float pcy = Y(by + bh * 0.5f) - ph * 0.5f;
    // The label is centred on the bar, so past halfway it sits on the amber
    // fill: switch to dark ink there, otherwise the white text is unreadable.
    const bool overFill = !indeterminate && pct > 0.45f;
    const float tr = overFill ? 0.12f : 1.0f, tg = overFill ? 0.08f : 1.0f,
                tb = overFill ? 0.02f : 1.0f;
    const float sr = overFill ? 1.0f : 0.0f, sg = overFill ? 0.95f : 0.0f,
                sb = overFill ? 0.75f : 0.0f, sa = overFill ? 0.45f : 0.85f;
    font.drawCentered(r, pctText, X(VW * 0.5f) + 1.0f * sc, pcy + 1.0f * sc, ps, sr, sg, sb, sa);
    font.drawCentered(r, pctText, X(VW * 0.5f), pcy, ps, tr, tg, tb, 1.0f);

    // 5) the item being loaded (truncated so a long path cannot run off screen),
    //    with the classic trailing dots so a stalled parse still looks alive
    std::string item = item_;
    if (item.size() > 60) item = "..." + item.substr(item.size() - 57);
    if (!item.empty()) {
        item += std::string(1 + ((int)(t_ * 2.0f) % 3), '.');
        font.drawCentered(r, item, X(VW * 0.5f), Y(by + bh + 14.0f), 1.3f * sc,
                          0.72f, 0.76f, 0.82f, 0.95f);
    }
    if (!note_.empty())
        font.drawCentered(r, note_, X(VW * 0.5f), Y(by + bh + 36.0f), 1.2f * sc,
                          0.55f, 0.60f, 0.68f, 0.9f);

    // 7) the original's version string, exactly as the menu draws it
    font.drawCentered(r, versionString(), X(VW * 0.5f), Y(VH - 16.0f), 1.1f * sc,
                      0.85f, 0.85f, 0.88f, 0.9f);
    //    and the project tag opposite it
    font.draw(r, "HT2:RE", X(12.0f), Y(VH - 16.0f), 1.0f * sc, 0.45f, 0.50f, 0.58f, 0.85f);

    r.end2D();
}

} // namespace ht2

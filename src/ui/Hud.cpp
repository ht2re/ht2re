#include "Hud.h"
#include "../gfx/Renderer.h"
#include "../res/Res.h"
#include "../res/Mask.h"
#include "../world/SceneTextures.h"
#include "Font.h"
#include "../util/Log.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <algorithm>

namespace ht2 {

// The strip is authored for a 1600-pixel-wide screen; the original blits it 1:1
// at a per-resolution x and picks a text base (docs/original_ui.md 2.1).  The
// reference frames were measured at 800x600, where x = -399: the first LED
// window (art x 490) lands at screen x 91 and the second (art x 701) at 300.
static const float kStripX800 = -399.0f;      // the original's 641..800 branch

// The original draws the 1600x50 strip 1:1 and lets the readouts keep their
// authored pixel size at every resolution (docs/original_ui.md 2.1/2.8).  The
// reference layout is therefore exactly the art's own scale: 1.
float Hud::layoutScale(const Renderer& r) const {
    (void)r;
    return 1.0f;
}

float Hud::layoutX(const Renderer& r) const {
    // The original's per-resolution branch (docs/original_ui.md 2.1):
    // >1204 -> 0, 801..1204 -> -479, 641..800 -> -399.  Verified against the
    // reference frames: at 800 the first LED window (art x 490) lands at 91.
    const int w = r.width();
    if (w > 1204) return 0.0f;
    if (w > 800) return -479.0f;
    return kStripX800;
}

float Hud::windowX(const Renderer& r) const {
    // The first LED window of the art is at x 490; the second at 701.
    return 490.0f + layoutX(r);
}

// The reference element table: every x/y is in the 800x600 space of the frames
// (the docs' text base x = 70 plus their per-element offsets, confirmed against
// ref_hud.png / ref_map_and_fine.png / ref_hud_helicopter.png).
static const Hud::Cell kCells[] = {
    {"nav_arrow",   70.0f,  10.0f},
    {"pos_label",   78.0f,   6.0f},   // ПОЗИЦИЯ
    {"pos_value",   90.0f,  20.0f},   // 2 км 3
    {"time_label", 143.0f,   6.0f},   // ОСТ.ВРЕМЯ
    {"time_value", 140.0f,  20.0f},   // 25:06
    {"speed",      307.0f,  22.0f},   // 82
    {"speed_unit", 335.0f,  26.0f},   // км/ч
    {"tacho_label",396.0f,   6.0f},   // АККИ
    {"tacho_value",425.0f,   6.0f},   // 5
    {"tacho_unit", 396.0f,  24.0f},   // 20x100 об/м
    {"clock_label",632.0f,   6.0f},   // ЧАС
    {"clock_value",624.0f,  20.0f},   // 09:00
    {"gear",       690.0f,  20.0f},
    {"level",      714.0f,  20.0f},
};
const Hud::Cell* Hud::cells(size_t& n) { n = sizeof kCells / sizeof kCells[0]; return kCells; }

bool Hud::load(Renderer& r, const std::string& dataDir) {
    // the strip lives in the in-game UI container (esc16.RES / esc15.RES)
    SceneTextures tex;
    if (!tex.load(r, dataDir + "/COMMON/esc16") && !tex.load(r, dataDir + "/COMMON/esc15")) {
        LOG("hud: esc16.RES not found, using the built-in dashboard");
        return false;
    }
    tablo_ = tex.textureFor("tablo");
    if (!tablo_) { LOG("hud: m16\\tablo.txr missing"); return false; }

    // the message window and its "new message" icon are masks, not textures
    Res res;
    if (res.load(dataDir + "/COMMON/esc16.RES")) {
        auto upload = [&](const char* name, unsigned& out) {
            const Res::Entry* e = res.find(name);
            if (!e || e->kind != Res::Kind::Mask) return;
            DecodedImage img;
            if (!Mask::decode(res.data(*e), e->size, img)) return;
            out = r.createTexture(img.rgba.data(), img.width, img.height);
            if (out) owned_.push_back(out);
        };
        upload("m16\\pager_p.msk", pagerTex_);
        upload("m16\\pager_a.msk", pagerATex_);
    }
    (void)pager_; (void)pagerA_;
    // the camera flash sprite of the fine panel (COMMON.RES, 4:4:4:4)
    {
        SceneTextures common;
        if (common.load(r, dataDir + "/COMMON/COMMON")) {
            flashTex_ = common.textureFor("flash");
            if (flashTex_) owned_.push_back(flashTex_);
        }
    }
    // the original HUD fonts (loose files next to the data)
    if (labelFont_.loadFon(r, dataDir + "/matrix.fon")) {
        labelFont_.setCodeOffset(0x10);
        label_ = &labelFont_;
    }
    if (bigFont_.loadFon(r, dataDir + "/icon.fon")) {
        bigFont_.setCodeOffset(0x10);
        big_ = &bigFont_;
    }
    LOG("hud: original strip loaded (tablo %s, pager %s, fonts %s/%s)",
        tablo_ ? "yes" : "no", pagerTex_ ? "yes" : "no",
        label_ ? "matrix.fon" : "-", big_ ? "icon.fon" : "-");
    return true;
}

void Hud::draw(Renderer& r, Font& font, const Values& v) {
    if (!tablo_) return;
    const float s = layoutScale(r);
    const float dx = layoutX(r);
    const float W = (float)r.width();

    // The original blits the 1600x50 strip 1:1 (clipped by the screen) - see
    // the layout comment in Hud.h; the windows of the art then sit under the
    // readouts at exactly the reference positions.
    r.quad2D(tablo_, dx, 0.0f, kArtWidth, kArtHeight, 1, 1, 1, 1);

    const float win = 490.0f + dx;                 // first LED window
    char buf[128];
    const float inkR = 0.90f, inkG = 0.92f, inkB = 0.55f;    // the LED ink
    auto put = [&](const char* t, float x, float y, float scale, bool big, bool dark) {
        Font& f = (big && big_) ? *big_ : (label_ ? *label_ : font);
        const bool orig = (big && big_) || label_ != nullptr;
        const float k = orig ? 1.0f : 0.62f;       // built-in fallback is bigger
        if (dark) f.draw(r, t, win + x * s, y * s, scale * s * k, 0.06f, 0.09f, 0.06f, 0.85f);
        f.draw(r, t, win + x * s, y * s, scale * s * k, inkR, inkG, inkB, 1.0f);
    };
    const float lscale = 1.0f;

    // --- left LED window: ПОЗИЦИЯ (destination) and ОСТ.ВРЕМЯ (time left) ---
    put("ПОЗИЦИЯ", 0.0f, 5.0f, lscale, false, false);
    put("ОСТ.ВРЕМЯ", 65.0f, 5.0f, lscale, false, false);
    if (v.haveDest) {
        // «2 км 3»: kilometres integer + the "км" unit + the fractional digit
        const float km = v.distanceToDest / 1000.0f;
        const int whole = (int)km;
        const int frac = (int)((km - whole) * 10.0f + 0.5f);
        snprintf(buf, sizeof buf, "%d", whole);
        put(buf, 0.0f, 19.0f, 0.72f, true, false);
        put("км", 22.0f, 27.0f, lscale, false, false);
        snprintf(buf, sizeof buf, "%d", frac);
        put(buf, 44.0f, 19.0f, 0.72f, true, false);
    } else {
        put("-", 0.0f, 19.0f, 0.72f, true, false);
    }
    if (v.jobTime && *v.jobTime) {
        put(v.jobTime, 50.0f, 19.0f, 0.80f, true, v.jobOverdue);
    } else {
        put("--:--", 50.0f, 19.0f, 0.80f, true, false);
    }

    // --- centre LED window: speed, АККИ tacho, the unit marks ---------------
    snprintf(buf, sizeof buf, "%d", (int)(v.speedKmh + 0.5f));
    put(buf, 216.0f, 17.0f, 0.80f, true, false);
    put("км/ч", 244.0f, 26.0f, lscale, false, false);

    put("АККИ", 305.0f, 5.0f, lscale, false, false);
    snprintf(buf, sizeof buf, "%d", (int)(v.rpm / 400.0f + 0.5f));   // gear-like step
    put(buf, 334.0f, 5.0f, lscale, false, false);
    // «20x100 об/м» = the tacho reading in hundreds of revolutions per minute
    snprintf(buf, sizeof buf, "%dx100", (int)(v.rpm / 100.0f + 0.5f));
    put(buf, 305.0f, 23.0f, lscale, false, false);
    put("об/м", 352.0f, 26.0f, lscale, false, false);

    // --- right of the fuel window: the digital clock ------------------------
    int hh = (int)v.clockHours, mm = (int)((v.clockHours - hh) * 60.0f + 0.5f);
    if (mm > 59) mm = 59;
    put("ЧАС", 541.0f, 5.0f, lscale, false, false);
    snprintf(buf, sizeof buf, "%02d:%02d", hh, mm);
    put(buf, 533.0f, 19.0f, 0.78f, true, false);

    // gear + damage level ride at the end of the strip (no original window
    // there on a 16:9 screen, so they are small and unobtrusive)
    if (v.gear == 0) snprintf(buf, sizeof buf, "N");
    else if (v.gear < 0) snprintf(buf, sizeof buf, "R");
    else snprintf(buf, sizeof buf, "%d", v.gear);
    put(buf, 599.0f, 19.0f, 0.78f, true, false);
    snprintf(buf, sizeof buf, "P%d", std::min(9, std::max(0, v.level)));
    put(buf, 623.0f, 21.0f, lscale, false, false);

    // --- message window (the original's pager) ------------------------------
    const bool showMsg = !v.message.empty() && v.messageTimer > 0.0f;
    if (showMsg)
        drawMessage(r, font, v.message, std::min(1.0f, v.messageTimer / 0.6f),
                    v.messageNew, v.messageAge);

    char rep[256];
    snprintf(rep, sizeof rep,
             "hud: strip x %.0f scale %.2f win %.0f | pos %s time %s speed %d "
             "rpm %d clock %02d:%02d msg %s",
             dx, s, win, v.haveDest ? "yes" : "no", (v.jobTime && *v.jobTime) ? v.jobTime : "-",
             (int)(v.speedKmh + 0.5f), (int)(v.rpm + 0.5f), hh, mm,
             showMsg ? "shown" : "hidden");
    report_ = rep;
}

// ---------------------------------------------------------------------------
// Speed camera / fine panel
// ---------------------------------------------------------------------------
void Hud::drawFine(Renderer& r, const Fine& f) {
    if (f.timer <= 0.0f) return;
    const float s = layoutScale(r);
    const float W = (float)r.width();
    // the reference frame (shots/ref/ref_fine_panel_mirror.png) puts the panel
    // in the upper right, just under the strip
    const float pw = 236.0f * s, ph = 356.0f * s;
    const float px = W - pw - 8.0f * s, py = 54.0f * s;
    const float fade = std::min(1.0f, f.timer / 0.6f);

    // The panel is composed from the strip's own chrome colours (the original
    // ships no violation mask - see Hud.h) plus the real camera flash sprite.
    unsigned white = r.whiteTex();
    auto box = [&](float x, float y, float w, float h, float cr, float cg, float cb, float a) {
        r.quad2D(white, px + x * s, py + y * s, w * s, h * s, cr, cg, cb, a);
    };
    box(0, 0, pw / s, ph / s, 0.30f, 0.32f, 0.36f, 0.96f * fade);          // chrome
    box(3, 3, 230, 350, 0.06f, 0.05f, 0.05f, 0.98f * fade);                // dark face
    box(5, 5, 226, 22, 0.62f, 0.08f, 0.09f, 0.98f * fade);                 // red header
    box(11, 31, 214, 18, 0.30f, 0.03f, 0.04f, 0.98f * fade);
    box(11, 52, 214, 18, 0.30f, 0.03f, 0.04f, 0.98f * fade);
    // the "radar in a circle" plate the original draws around the photo
    box(30, 76, 176, 190, 0.13f, 0.15f, 0.36f, 0.98f * fade);
    box(36, 82, 164, 178, 0.02f, 0.02f, 0.03f, 0.98f * fade);
    box(20, 274, 196, 26, 0.55f, 0.06f, 0.07f, 0.98f * fade);              // ШТРАФ bar
    box(20, 306, 196, 34, 0.04f, 0.04f, 0.05f, 0.98f * fade);

    if (!label_) return;                       // the panel needs the original font
    Font& mf = *label_;
    auto bigText = [&](const char* t, float cx, float y, float sc, float cr, float cg, float cb) {
        // icon.fon has no Cyrillic (its glyph codes stop at ASCII+0x10), so the
        // panel's words are drawn with matrix.fon at a larger scale - the same
        // trick the original's centred popups use for their headers.
        if (big_) big_->drawCentered(r, t, px + cx * s, py + y * s, sc * s, cr, cg, cb, fade);
        else mf.drawCentered(r, t, px + cx * s, py + y * s, sc * s * 1.5f, cr, cg, cb, fade);
    };
    auto small = [&](const char* t, float cx, float y) {
        const float tw = mf.textWidth(t, s);
        mf.draw(r, t, px + cx * s - tw * 0.5f, py + y * s, s, 1.0f, 0.92f, 0.35f, fade);
    };

    // «ЗАРЕГИСТРИРОВАНО ПРЕВЫШЕНИЕ СКОРОСТИ» (the two-line red header)
    small("ЗАРЕГИСТРИРОВАНО", 118, 6);
    small("ПРЕВЫШЕНИЕ СКОРОСТИ", 118, 15);
    char buf[64];
    snprintf(buf, sizeof buf, "ОГРАНИЧЕНИЕ: %d КМ/Ч", f.limit);
    small(buf, 118, 33);
    snprintf(buf, sizeof buf, "ВАША СКОРОСТЬ: %d КМ/Ч", f.speed);
    small(buf, 118, 54);

    // the photo: the real flash sprite over a dark frame, brightest right after
    // the trigger (the original snaps a still and flashes it)
    if (flashTex_) {
        const float fx = 52.0f, fy = 96.0f, fw = 132.0f, fh = 150.0f;
        r.quad2D(white, px + (fx - 3) * s, py + (fy - 3) * s, (fw + 6) * s, (fh + 6) * s,
                 0.55f, 0.58f, 0.62f, 0.95f * fade);
        r.quad2D(white, px + fx * s, py + fy * s, fw * s, fh * s,
                 0.04f, 0.05f, 0.07f, 0.98f * fade);
        r.quad2D(flashTex_, px + (fx + 30) * s, py + (fy + 10) * s, 70.0f * s, 130.0f * s,
                 1, 1, 1, 0.55f * fade);
        const float flash = std::max(0.0f, 1.0f - f.age * 1.6f);
        if (flash > 0.0f) {
            r.quad2D(white, px + (fx - 4) * s, py + (fy - 4) * s, (fw + 8) * s,
                     (fh + 8) * s, 1, 1, 1, 0.75f * flash * fade);
            r.quad2D(white, px + (fx + 10) * s, py + (fy + 20) * s, 60 * s, 60 * s,
                     1, 1, 1, 0.9f * flash * fade);
        }
    }

    // «ШТРАФ» + the amount (matrix.fon again: icon.fon is digits only)
    {
        const char* w = "ШТРАФ";
        const float tw = mf.textWidth(w, s * 1.4f);
        mf.draw(r, w, px + 118 * s - tw * 0.5f, py + 276 * s, s * 1.4f, 1.0f, 0.9f, 0.25f, fade);
    }
    snprintf(buf, sizeof buf, "$%ld", f.amount);
    if (big_) big_->drawCentered(r, buf, px + 118 * s, py + 306 * s, 0.7f * s,
                                 1.0f, 0.9f, 0.25f, fade);
    else mf.drawCentered(r, buf, px + 118 * s, py + 306 * s, s * 1.4f,
                         1.0f, 0.9f, 0.25f, fade);

    LOG("fine: panel shown (limit %d, speed %d, $%ld, %s art)",
        f.limit, f.speed, f.amount, flashTex_ ? "flash" : "no");
}

// The script-message window of the reference frames (ref_message_window.png):
// the pager plate just below the strip, with the blinking "new message" icon
// and the text scrolling when it overflows.
void Hud::drawMessage(Renderer& r, Font& font, const std::string& text, float alpha,
                      bool isNew, float age) {
    if (text.empty() || alpha <= 0.0f) return;
    // The original's pager plate lives in the strip band: docs/original_ui.md 2.4
    // puts it at y 11 on a wide screen and at y 50 (just under the 50-px strip)
    // below 801 px, and the retail frame shows it as a centred plate at the top
    // of the band (shots/ref/ref_message_window.png).  Centred in the band here,
    // which is where both the original branches land on a 16:9 window.
    const float s = layoutScale(r);
    const float mw = 300.0f * s, mh = 30.0f * s;
    const float mx = ((float)r.width() - mw) * 0.5f;
    const float my = std::max(0.0f, (kArtHeight - mh) * 0.5f);
    Font& mf = label_ ? *label_ : font;
    const float k = label_ ? 1.0f : 0.62f;
    const float tx = mx + 34.0f * s, ty = my + 8.0f * s;
    const float maxW = 247.0f * s;
    if (pagerTex_)
        r.quad2D(pagerTex_, mx, my, mw, mh, 1, 1, 1, 0.95f * alpha);
    else
        r.quad2D(r.whiteTex(), mx, my, mw, mh, 0.10f, 0.13f, 0.11f, 0.95f * alpha);
    if (pagerATex_ && isNew) {
        const bool on = (((int)(age * 4.0f)) & 1) == 0;
        if (on)
            r.quad2D(pagerATex_, mx + 6.0f * s, my + 5.0f * s, 21.0f * s, 19.0f * s,
                     1, 1, 1, alpha);
    }
    const float tw = mf.textWidth(text, s * k);
    if (tw > maxW) {
        const float step = std::max(1.0f, (float)mf.cellW() * s * k);
        const int total = std::max(1, (int)((tw - maxW) / step) + 1);
        const int phase = (int)(age * 6.0f) % total;
        mf.draw(r, text, tx - (float)phase * step, ty, s * k, 1.0f, 1.0f, 0.85f, alpha);
    } else {
        mf.draw(r, text, tx, ty, s * k, 1.0f, 1.0f, 0.85f, alpha);
    }
}

} // namespace ht2

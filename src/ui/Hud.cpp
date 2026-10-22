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
    // king.exe 0x169ad7: >1204 -> 0, 801..1204 -> -479, 641..800 -> -399,
    // ≤640 -> -479. At 800 the first LED window (art x 490) lands at 91.
    const int w = r.width();
    if (w > 1204) return 0.0f;
    if (w > 800) return -479.0f;
    if (w > 640) return kStripX800;
    return -479.0f;
}

// Text column origin, the `ebp` of the same width test (0x169ae1..0x169b4d).
static float textBase(const Renderer& r) {
    const int w = r.width();
    if (w > 1204) return 469.0f;
    if (w > 800) return -10.0f;
    if (w > 640) return 70.0f;
    return -10.0f;
}

float Hud::windowX(const Renderer& r) const {
    // The first LED window of the art is at x 490; the second at 701.
    return 490.0f + layoutX(r);
}

// The reference element table: every x/y is in the 800x600 space of the frames
// (the docs' text base x = 70 plus their per-element offsets, confirmed against
// ref_hud.png / ref_map_and_fine.png / ref_hud_helicopter.png).
// 800-px screen positions: text base 70 plus the immediates in 0x169de1..0x16a245.
// y is the bottom of the glyph, the value 0x1ea170 receives.
static const Hud::Cell kCells[] = {
    {"pos_label",   96.0f,  23.0f},   // ПОЗИЦИЯ          +0x1a
    {"pos_cur",     96.0f,  43.0f},   // city index
    {"pos_of",     120.0f,  38.0f},   // «из», centred on +0x32
    {"pos_total",  133.0f,  43.0f},   // city count
    {"time_label", 155.0f,  23.0f},   // ОСТ.ВРЕМЯ        +0x55
    {"time_value", 155.0f,  43.0f},   // MM:SS
    {"speed",      357.0f,  38.0f},   // right edge       +0x11f
    {"speed_unit", 361.0f,  40.0f},   // км/ч             +0x123
    {"gear",       446.0f,  23.0f},   // АКПП  %c         +0x169+15
    {"rpm",        456.0f,  43.0f},   // right edge       +0x182
    {"rpm_unit",   456.0f,  40.0f},   // x100 об/м
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
        upload("m16\\pause.msk", pauseTex_);
        upload("m16\\mess16.msk", fineTex_);
    }
    (void)pager_; (void)pagerA_;
    // the original HUD fonts (loose files next to the data)
    if (labelFont_.loadFon(r, dataDir + "/matrix.fon")) {
        // matrix.fon is indexed by the actual byte value (including cp1251).
        // Only icon.fon's large digits live at ASCII + 0x10.  Applying that
        // offset here shifted every Russian label into unrelated glyph cells.
        labelFont_.setCodeOffset(0);
        label_ = &labelFont_;
    }
    if (bigFont_.loadFon(r, dataDir + "/icon.fon")) {
        bigFont_.setCodeOffset(0x10);
        big_ = &bigFont_;
    }
    LOG("hud: original strip loaded (tablo %s, pager %s, pause %s, fonts %s/%s)",
        tablo_ ? "yes" : "no", pagerTex_ ? "yes" : "no", pauseTex_ ? "yes" : "no",
        label_ ? "matrix.fon" : "-", big_ ? "icon.fon" : "-");
    return true;
}

void Hud::drawPause(Renderer& r) {
    if (!pauseTex_) return;
    r.quad2D(pauseTex_, layoutX(r), 0.0f, kArtWidth, kArtHeight, 1, 1, 1, 1);
}

void Hud::draw(Renderer& r, Font& font, const Values& v) {
    if (!tablo_) return;
    const float dx = layoutX(r);
    const float s = layoutScale(r);

    // The original blits the 1600x50 strip 1:1 (clipped by the screen) - see
    // the layout comment in Hud.h; the windows of the art then sit under the
    // readouts at exactly the reference positions.
    r.quad2D(tablo_, dx, 0.0f, kArtWidth, kArtHeight, 1, 1, 1, 1);

    const float win = 490.0f + dx;                 // first LED window (the log)
    const float base = textBase(r);
    char buf[128];
    // The atlas colour is still open (0x1ea1b0 passes 0,0 into the blitter and
    // the RGB map was not recovered). The retail LEDs are this yellow-green.
    const float inkR = 0.90f, inkG = 0.92f, inkB = 0.55f;
    // matrix.fon glyph is 12 px, icon.fon glyph is 30 px. codeY is the bottom.
    auto putAt = [&](const char* t, float x, float codeY, bool big, bool right) {
        const bool useBig = big && big_;
        Font& f = useBig ? *big_ : (label_ ? *label_ : font);
        const float gh = useBig ? 30.0f : (label_ ? 12.0f : 8.0f);
        const float sc = (useBig || label_) ? 1.0f : 0.62f;
        const float tw = f.textWidth(t, sc);
        const float px = right ? (base + x - tw) : (base + x);
        f.draw(r, t, px, codeY - gh, sc, inkR, inkG, inkB, 1.0f);
    };
    auto led = [&](const char* t) {
        if (!t || !*t || !big_) return false;
        for (const char* p = t; *p; ++p)
            if (!((*p >= '0' && *p <= '9') || *p == ':')) return false;
        return true;
    };

    // The 8-sector arrow (0x16a6f3, raw cells at ebp, y=40) shares its origin
    // with the ПОЗИЦИЯ label. The retail frame does not show it in the window,
    // so the strip keeps the digits.

    // «ПОЗИЦИЯ» + city index «из» count. Skipped when the index is negative,
    // which is the exe's jl at 0x169ddb.
    if (v.posCur >= 0 && v.posTotal >= 0) {
        putAt("ПОЗИЦИЯ", 0x1a, 23.0f, false, false);
        snprintf(buf, sizeof buf, "%d", v.posCur);
        putAt(buf, 0x1a, 43.0f, true, false);
        if (label_) {
            const float tw = label_->textWidth("из", 1.0f);
            label_->draw(r, "из", base + 0x32 - tw * 0.5f, 38.0f - 12.0f,
                         1.0f, inkR, inkG, inkB, 1.0f);
        }
        snprintf(buf, sizeof buf, "%d", v.posTotal);
        putAt(buf, 0x3f, 43.0f, true, false);
    }

    // «ОСТ.ВРЕМЯ»: ftol of the seconds double, then /60 and %60 (0x169f19).
    putAt("ОСТ.ВРЕМЯ", 0x55, 23.0f, false, false);
    const char* timeText = (v.jobTime && *v.jobTime) ? v.jobTime : "00:00";
    if (v.jobOverdue && label_ && !led(timeText)) {
        label_->draw(r, timeText, base + 0x55, 43.0f - 12.0f, 1.0f,
                     0.45f, 0.20f, 0.12f, 1.0f);
    } else {
        putAt(timeText, 0x55, 43.0f, led(timeText), false);
    }

    // Speed is right-aligned to +0x11f (0x16a0b6: base - width + 0x11f).
    // «км/ч» is string-table 0x5d, drawn at +0x123. The port is metric.
    snprintf(buf, sizeof buf, "%d", (int)(v.speedKmh + 0.5f));
    putAt(buf, 0x11f, 38.0f, true, true);
    putAt("км/ч", 0x123, 40.0f, false, false);

    // Gear. The port simulates the automatic box, so the format is string
    // 0x5b «АКПП  %c» and the x is +0x169+15 (0x16a1a0 ands the auto flag
    // with 15). The character is N, R, or the gear digit.
    char gch = 'N';
    if (v.gear < 0) gch = 'R';
    else if (v.gear > 0) gch = (char)('0' + std::min(v.gear, 9));
    snprintf(buf, sizeof buf, "АКПП  %c", gch);
    putAt(buf, 0x169 + 15, 23.0f, false, false);

    // Tachometer: the integer sits with its right edge on +0x182, and the
    // static label «x100 об/м» (string 0x5e) starts on the same x. rpm() is
    // already revolutions per minute; the exe multiplies a different field
    // by 0.6 before ftol, so that factor is not applied here.
    int hundreds = (int)(v.rpm / 100.0f + 0.5f);
    if (hundreds < 0) hundreds = 0;
    snprintf(buf, sizeof buf, "%d", hundreds);
    putAt(buf, 0x182, 43.0f, true, true);
    putAt("x100 об/м", 0x182, 40.0f, false, false);

    int hh = (int)v.clockHours, mm = (int)((v.clockHours - hh) * 60.0f + 0.5f);
    if (mm > 59) mm = 59;

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
    if (f.timer <= 0.0f || !fineTex_) return;
    // 0x56b6af: x = screenW - mask width, y = 50. mess16.msk is 220×230.
    const float px = (float)r.width() - 220.0f;
    const float py = 50.0f;
    r.quad2D(fineTex_, px, py, 220.0f, 230.0f, 1, 1, 1, 1);
    if (!label_) {
        LOG("fine: panel shown (limit %d, speed %d, $%ld, mask only)",
            f.limit, f.speed, f.amount);
        return;
    }
    Font& mf = *label_;
    const float gh = (float)mf.glyphH();
    // The blitter y is the baseline. matrix.fon's drawn box is glyphH rows,
    // so the top sits glyphH above that y. Yellow matches the mask's ink.
    const float yr = 1.0f, yg = 0.87f, yb = 0.03f;
    char buf[32];
    snprintf(buf, sizeof buf, "%d", f.limit);
    mf.draw(r, buf, px + 137.0f, 142.0f - gh, 1.0f, yr, yg, yb, 1.0f);
    snprintf(buf, sizeof buf, "%d", f.speed);
    mf.draw(r, buf, px + 137.0f, 158.0f - gh, 1.0f, yr, yg, yb, 1.0f);
    snprintf(buf, sizeof buf, "$%ld", f.amount);
    mf.drawCentered(r, buf, px + 110.0f, 261.0f - gh, 1.0f, yr, yg, yb, 1.0f);
    LOG("fine: panel shown (limit %d, speed %d, $%ld, mess16)",
        f.limit, f.speed, f.amount);
}

// The script-message window of the reference frames (ref_message_window.png):
// the pager plate just below the strip, with the blinking "new message" icon
// and the text scrolling when it overflows.
void Hud::drawMessage(Renderer& r, Font& font, const std::string& text, float alpha,
                      bool isNew, float age) {
    if (text.empty() || alpha <= 0.0f) return;
    // king.exe 0x169ae1: the pager origin is (1161,11) / (682,11) / (250,50) /
    // (170,50) on the same width bands as the strip. The plate is 300x30 and
    // the text is at +(34, 23) with the glyph hanging above that y.
    const float s = layoutScale(r);
    const float mw = 300.0f * s, mh = 30.0f * s;
    const int w = r.width();
    float mx = 1161.0f, my = 11.0f;
    if (w <= 640) { mx = 170.0f; my = 50.0f; }
    else if (w <= 800) { mx = 250.0f; my = 50.0f; }
    else if (w <= 1204) { mx = 682.0f; my = 11.0f; }
    Font& mf = label_ ? *label_ : font;
    const float k = label_ ? 1.0f : 0.62f;
    const float tx = mx + 34.0f * s;
    const float ty = my + (23.0f - (label_ ? 12.0f : 8.0f)) * s;
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

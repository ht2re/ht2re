#include "Menu.h"
#include "Font.h"
#include "../gfx/Renderer.h"
#include "../res/Mask.h"
#include "../util/Log.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ht2 {

static const float PI = 3.14159265358979f;

// The original main menu: eight 125x125 buttons arranged around the transparent
// centre circle of common.msk (the truck front).  ids, socket positions and the
// internal button ids below were recovered from king.exe @0x1282f8..0x1283f4:
//   push Y / push X / push <id> / push "m\<name>" / call 0x10e500
struct DefaultItem { const char* id; const char* label; int socketId; int x, y; };
static const DefaultItem kDefaults[] = {
    {"graph", "ГРАФИКА",           107,  79,  82},   // left column, top
    {"contr", "УПРАВЛЕНИЕ",        103,  27, 202},
    {"demo",  "ДЕМО",              106,  27, 332},
    {"recor", "ВИДЕОПЛЕЕР",        101,  79, 452},
    {"singl", "ИГРА",              102, 596,  82},   // right column, top
    {"multi", "СЕТЕВАЯ ИГРА",      104, 648, 202},
    {"chang", "СМЕНИТЬ ВОДИТЕЛЯ",  105, 648, 332},
    {"exit",  "ВЫХОД ИЗ ИГРЫ",     108, 596, 452},
};
static const int kDefaultSel = 4;                // "SINGLE PLAYER"
static const int kPerColumn = 4;

bool Menu::load(const std::string& dataDir) {
    std::string resPath = dataDir + "/LOAD.RES";
    bool haveRes = res_.load(resPath);
    items_.clear();
    for (const auto& d : kDefaults) {
        Item it;
        it.id = d.id;
        it.label = d.label;
        it.socketId = d.socketId;
        it.x = d.x; it.y = d.y;
        it.maskBase = std::string("m\\") + d.id;
        if (haveRes) it.available = res_.find(it.maskBase + "_a.msk") != nullptr;
        if (!it.available) LOG("menu: button '%s' art missing", it.id.c_str());
        items_.push_back(it);
    }
    sel_ = kDefaultSel < (int)items_.size() ? kDefaultSel : 0;
    return true;
}

void Menu::loadArt(Renderer& r) {
    if (artLoaded_ || !res_.loaded()) return;
    artLoaded_ = true;
    auto upload = [&](const Res::Entry* e, const char* what) -> unsigned {
        if (!e) return 0;
        DecodedImage img;
        if (!Mask::decode(res_.data(*e), e->size, img)) {
            LOG("menu: %s decode failed (%s)", what, e->name.c_str());
            return 0;
        }
        return r.createTexture(img.rgba.data(), img.width, img.height);
    };
    frameTex_ = upload(res_.find("common.msk"), "frame");
    {
        const Res::Entry* ce = res_.find("cursor.msk");
        cursorTex_ = upload(ce, "cursor");
        if (cursorTex_ && ce) {
            DecodedImage ci;
            if (Mask::decode(res_.data(*ce), ce->size, ci)) { cursorW_ = ci.width; cursorH_ = ci.height; }
        }
    }
    if (frameTex_) {
        const Res::Entry* e = res_.find("common.msk");
        DecodedImage img;
        if (Mask::decode(res_.data(*e), e->size, img)) { frameW_ = img.width; frameH_ = img.height; }
    }
    int decoded = 0;
    for (auto& it : items_) {
        it.texA  = upload(res_.find(it.maskBase + "_a.msk"),  "button");
        it.texAP = upload(res_.find(it.maskBase + "_ap.msk"), "button");
        it.texP  = upload(res_.find(it.maskBase + "_p.msk"),  "button");
        if (it.texA) ++decoded;
    }
    LOG("menu: art loaded (frame=%u, buttons=%d/8)", frameTex_, decoded);
}

float Menu::cellRect(int i, float& x, float& y, float& w, float& h) const {
    if (i < 0 || i >= (int)items_.size()) { x = y = w = h = 0; return 0; }
    x = (float)items_[i].x;
    y = (float)items_[i].y;
    w = h = 125.0f;
    return 125.0f;
}

int Menu::hitTest(float mx, float my) const {
    for (size_t i = 0; i < items_.size(); ++i) {
        float x, y, w, h;
        cellRect((int)i, x, y, w, h);
        if (mx >= x && mx <= x + w && my >= y && my <= y + h) return (int)i;
    }
    return -1;
}

void Menu::handleKey(int key) {
    const int n = (int)items_.size();
    if (n == 0) return;
    switch (key) {
        case SDLK_UP:    sel_ = (sel_ - 1 + n) % n; break;
        case SDLK_DOWN:  sel_ = (sel_ + 1) % n; break;
        case SDLK_LEFT:  sel_ = (sel_ - kPerColumn + n) % n; break;
        case SDLK_RIGHT: sel_ = (sel_ + kPerColumn) % n; break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            activated_ = items_[sel_].id;
            break;
        default:
            // the original also binds the internal button ids ('e'..'l' = 101..108)
            // as hot keys (recovered from the king.exe table)
            for (auto& it : items_)
                if (key == it.socketId) { activated_ = it.id; break; }
            break;
    }
}

void Menu::handleMouse(float mx, float my, bool click) {
    mouseX_ = mx;
    mouseY_ = my;
    float vx = (mx - offX_) / scale_;
    float vy = (my - offY_) / scale_;
    int hit = hitTest(vx, vy);
    if (hit >= 0) {
        sel_ = hit;
        if (click) activated_ = items_[hit].id;
    }
}

void Menu::update(float dt) {
    time_ += dt;
    if (messageT_ > 0.0f) {
        messageT_ -= dt;
        if (messageT_ <= 0.0f) message_.clear();
    }
}

void Menu::setMessage(const std::string& text, float seconds) {
    message_ = text;
    messageT_ = seconds;
}

std::string Menu::takeActivated() {
    std::string s = activated_;
    activated_.clear();
    return s;
}

void Menu::camera(float& x, float& y, float& z, float& yaw, float& pitch) const {
    // slow orbit around the MENV/dr.b3d city block (bbox x -139..776,
    // z -340..517, height -3..42); the scene shows through the transparent
    // centre circle of common.msk.
    const float cx = 330.0f, cz = 60.0f;
    const float R = 430.0f;
    float a = 0.75f + time_ * 0.045f;
    x = cx + cosf(a) * R;
    z = cz + sinf(a) * R;
    y = 72.0f + 12.0f * sinf(time_ * 0.11f);
    float dx = cx - x, dz = cz - z;
    float l = sqrtf(dx * dx + dz * dz);
    yaw = atan2f(dx / l, -dz / l) * 180.0f / PI;
    pitch = -8.0f + 2.0f * sinf(time_ * 0.09f);
}

void Menu::draw(Renderer& r, Font& font) {
    vpW_ = r.width_;
    vpH_ = r.height_;
    scale_ = std::min((float)vpW_ / VW, (float)vpH_ / VH);
    offX_ = (vpW_ - VW * scale_) * 0.5f;
    offY_ = (vpH_ - VH * scale_) * 0.5f;
    auto X = [&](float vx) { return offX_ + vx * scale_; };
    auto Y = [&](float vy) { return offY_ + vy * scale_; };

    r.begin2D();
    unsigned white = r.whiteTex();

    // 0) letterbox: the 3D scene fills the window but the UI canvas is 4:3, so
    //    cover everything outside the canvas to keep the scene inside the frame.
    if (offX_ > 0.5f) {
        r.quad2D(white, 0, 0, offX_, (float)vpH_, 0, 0, 0, 1);
        r.quad2D(white, offX_ + VW * scale_, 0, offX_, (float)vpH_, 0, 0, 0, 1);
    }
    if (offY_ > 0.5f) {
        r.quad2D(white, 0, 0, (float)vpW_, offY_, 0, 0, 0, 1);
        r.quad2D(white, 0, offY_ + VH * scale_, (float)vpW_, offY_, 0, 0, 0, 1);
    }

    // 1) the original frame: common.msk is the 800x600 truck-front overlay with
    //    a transparent centre circle, through which the 3D scene shows.
    if (frameTex_) {
        r.quad2D(frameTex_, X(0), Y(0), VW * scale_, VH * scale_, 1, 1, 1, 1, 0, 0, 1, 1);
    } else {
        r.quad2D(white, X(0), Y(0), VW * scale_, VH * scale_, 0.03f, 0.05f, 0.09f, 0.35f);
    }

    // 2) buttons (original 125x125 art; state a / ap / p)
    float pulse = 0.5f + 0.5f * sinf(time_ * 4.0f);
    for (size_t i = 0; i < items_.size(); ++i) {
        const Item& it = items_[i];
        float x, y, w, h;
        cellRect((int)i, x, y, w, h);
        bool on = ((int)i == sel_);
        if (it.texA) {
            unsigned tex = on ? (it.texAP ? it.texAP : it.texA) : it.texA;
            r.quad2D(tex, X(x), Y(y), w * scale_, h * scale_, 1, 1, 1, 1, 0, 0, 1, 1);
            if (on) {
                // selection halo behind the art
                float t = 2.0f * scale_;
                r.quad2D(white, X(x), Y(y), w * scale_, t, 1.0f, 0.85f, 0.25f, 0.55f + 0.35f * pulse);
                r.quad2D(white, X(x), Y(y + h) - t, w * scale_, t, 1.0f, 0.85f, 0.25f, 0.55f + 0.35f * pulse);
                r.quad2D(white, X(x), Y(y), t, h * scale_, 1.0f, 0.85f, 0.25f, 0.55f + 0.35f * pulse);
                r.quad2D(white, X(x + w) - t, Y(y), t, h * scale_, 1.0f, 0.85f, 0.25f, 0.55f + 0.35f * pulse);
            }
        } else {
            // placeholder until the art decodes
            r.quad2D(white, X(x), Y(y), w * scale_, h * scale_, 0.10f, 0.14f, 0.22f, on ? 0.34f : 0.20f);
            float t = 2.0f * scale_;
            float fr = on ? 1.0f : 0.55f, fg = on ? 0.82f + 0.18f * pulse : 0.62f, fb = on ? 0.30f : 0.68f;
            r.quad2D(white, X(x), Y(y), w * scale_, t, fr, fg, fb, 1);
            r.quad2D(white, X(x), Y(y + h) - t, w * scale_, t, fr, fg, fb, 1);
            r.quad2D(white, X(x), Y(y), t, h * scale_, fr, fg, fb, 1);
            r.quad2D(white, X(x + w) - t, Y(y), t, h * scale_, fr, fg, fb, 1);
            float ls = 1.6f * scale_;
            float lh = font.lineHeight(ls);
            std::string lab = it.label;
            size_t sp = lab.rfind(' ');
            std::string l1 = lab, l2;
            if (font.textWidth(lab, ls) > (w - 18.0f) * scale_ && sp != std::string::npos) {
                l1 = lab.substr(0, sp);
                l2 = lab.substr(sp + 1);
            }
            if (l2.empty())
                font.drawCentered(r, l1, X(x + w * 0.5f), Y(y + h * 0.5f) - lh * 0.5f, ls, 1, 1, 1, 1);
            else {
                font.drawCentered(r, l1, X(x + w * 0.5f), Y(y + h * 0.5f) - lh, ls, 1, 1, 1, 1);
                font.drawCentered(r, l2, X(x + w * 0.5f), Y(y + h * 0.5f), ls, 1, 1, 1, 1);
            }
        }
    }

    // 3) version string (as printed by the original: king.exe string @0x6782xx)
    font.drawCentered(r, "v 8.0 30.01.03", X(VW * 0.5f), Y(VH - 16), 1.1f * scale_,
                      0.85f, 0.85f, 0.88f, 0.9f);

    // transient status line (placeholder screens)
    if (!message_.empty() && messageT_ > 0.0f) {
        float a = messageT_ < 0.5f ? messageT_ * 2.0f : 1.0f;
        r.quad2D(white, X(150), Y(VH - 60), 500 * scale_, 26 * scale_, 0.05f, 0.07f, 0.12f, 0.75f * a);
        font.drawCentered(r, message_, X(VW * 0.5f), Y(VH - 56), 1.4f * scale_,
                          1.0f, 0.9f, 0.5f, a);
    }

    // 4) cursor: the original cursor.msk bitmap (hotspot = top-left)
    if (mouseX_ >= 0) {
        if (cursorTex_) {
            r.quad2D(cursorTex_, mouseX_, mouseY_,
                     cursorW_ * scale_, cursorH_ * scale_, 1, 1, 1, 1, 0, 0, 1, 1);
        } else {
            r.line2D(white, mouseX_ - 10, mouseY_, mouseX_ + 10, mouseY_, 2.0f, 1, 1, 1, 1);
            r.line2D(white, mouseX_, mouseY_ - 10, mouseX_, mouseY_ + 10, 2.0f, 1, 1, 1, 1);
        }
    }
    r.end2D();
}

} // namespace ht2

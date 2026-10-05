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

// The screen each front-end button opens, from the recovered entry points
// (docs/original_ui.md 5.2).  "" = the port has no such screen yet; the App
// turns the id into its "not implemented" message.
const char* Menu::screenFor(const std::string& id) {
    if (id == "graph") return "graph";       // g\backGrafOpt.txr
    if (id == "contr") return "control";     // c\backCtrlOpt.txr (EscMenu "control")
    if (id == "demo")  return "demo";        // runs the demo
    if (id == "recor") return "records";     // r\backRcrd.txr -> rec | replay
    if (id == "singl") return "single";      // s\backSing.txr -> new | load
    if (id == "multi") return "multi";       // n\backMult.txr -> conn -> host
    if (id == "chang") return "driver";      // d\backDriv.txr
    if (id == "exit")  return "exitconf";    // e\backExitDoit.txr (the confirm)
    return nullptr;
}

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
    pages_.clear();
    auto page = [&](const char* name, const char* bg, const char* over = "",
                    int ox = 0, int oy = 0) {
        Page p; p.name = name; p.backdrop = bg; p.overlay = over;
        p.overlayX = ox; p.overlayY = oy; pages_.push_back(p);
    };
    page("graph",    "g\\backGraf.txr", "g\\backGrafOpt.txr", 310, 218);
    page("control",  "c\\backCtrl.txr", "c\\backCtrlOpt.txr", 110, 153);
    page("records",  "r\\backRcrd.txr");
    page("single",   "s\\backSing.txr");
    page("multi",    "n\\backMult.txr", "n\\backMultConn.txr", 230, 120);
    page("driver",   "d\\backDriv.txr", "d\\backDrivCurr.msk", 228, 125);
    page("exitconf", "e\\backExit.txr", "e\\backExitDoit.txr", 155, 128);
    page_.clear();
    return true;
}

void Menu::loadArt(Renderer& r) {
    if (artLoaded_ || !res_.loaded()) return;
    artLoaded_ = true;
    auto decode = [&](const Res::Entry* e, DecodedImage& img) -> bool {
        if (!e) return false;
        return Mask::decode(res_.data(*e), e->size, img) ||
               Texture::decode(res_.data(*e), e->size, img);
    };
    auto upload = [&](const Res::Entry* e, const char* what) -> unsigned {
        if (!e) return 0;
        DecodedImage img;
        if (!decode(e, img)) {
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
            if (decode(ce, ci)) { cursorW_ = ci.width; cursorH_ = ci.height; }
        }
    }
    if (frameTex_) {
        const Res::Entry* e = res_.find("common.msk");
        DecodedImage img;
        if (decode(e, img)) { frameW_ = img.width; frameH_ = img.height; }
    }
    int decoded = 0;
    for (auto& it : items_) {
        it.texA  = upload(res_.find(it.maskBase + "_a.msk"),  "button");
        it.texAP = upload(res_.find(it.maskBase + "_ap.msk"), "button");
        it.texP  = upload(res_.find(it.maskBase + "_p.msk"),  "button");
        if (it.texA) ++decoded;
    }
    for (Page& p : pages_) {
        const Res::Entry* be = res_.find(p.backdrop);
        p.backdropTex = upload(be, p.name.c_str());
        if (be && p.backdropTex) {
            DecodedImage img;
            if (decode(be, img)) {
                p.backdropW = img.width; p.backdropH = img.height;
            }
        }
        if (!p.overlay.empty()) {
            const Res::Entry* oe = res_.find(p.overlay);
            p.overlayTex = upload(oe, p.name.c_str());
            if (oe && p.overlayTex) {
                DecodedImage img;
                if (decode(oe, img)) {
                    p.overlayW = img.width; p.overlayH = img.height;
                }
            }
        }
    }
    exitOkTex_ = upload(res_.find("e\\ok_a.txr"), "exit OK");
    exitOkSelTex_ = upload(res_.find("e\\ok_ap.txr"), "exit OK selected");
    exitCancelTex_ = upload(res_.find("e\\canc_a.txr"), "exit cancel");
    exitCancelSelTex_ = upload(res_.find("e\\canc_ap.txr"), "exit cancel selected");
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
    // The exit confirmation is modal: only its two answers react, exactly like
    // the original's e\backExitDoit screen (e\ok / e\canc).
    if (confirm_ >= 0) {
        switch (key) {
            case SDLK_LEFT: case SDLK_RIGHT: case SDLK_TAB:
                confirmChoice_ = 1 - confirmChoice_; break;
            case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
                if (confirmChoice_ == 0) activated_ = items_[confirm_].id;
                confirm_ = -1;
                page_.clear();
                break;
            case SDLK_ESCAPE:
                confirm_ = -1;                       // e\canc
                page_.clear();
                break;
            default: break;
        }
        return;
    }
    if (!page_.empty()) {
        if (key == SDLK_ESCAPE) {
            LOG("menu: page '%s' closed", page_.c_str());
            page_.clear();
            return;
        }
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) {
            if (page_ == "single") activated_ = "singl";
            else if (page_ == "records") activated_ = "recor";
            else if (page_ == "multi") { activated_ = "multi"; page_.clear(); }
            else page_.clear();
        }
        return;
    }
    switch (key) {
        case SDLK_UP:    sel_ = (sel_ - 1 + n) % n; break;
        case SDLK_DOWN:  sel_ = (sel_ + 1) % n; break;
        case SDLK_LEFT:  sel_ = (sel_ - kPerColumn + n) % n; break;
        case SDLK_RIGHT: sel_ = (sel_ + kPerColumn) % n; break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            activate(sel_);
            break;
        default:
            // the original also binds the internal button ids ('e'..'l' = 101..108)
            // as hot keys (recovered from the king.exe table)
            for (size_t i = 0; i < items_.size(); ++i)
                if (key == items_[i].socketId) { activate((int)i); break; }
            break;
    }
}

// One place where a selection becomes an action, so the keyboard, the mouse and
// the gamepad all take the same route (the original's button constructor gives
// every source the same id).
// A page is only usable when its backdrop art actually decoded (several of the
// original screens are not shipped in LOAD.RES; opening one showed a black frame).
bool Menu::pageAvailable(const std::string& id) const {
    const char* pg = screenFor(id);
    if (!pg) return false;
    for (const auto& p : pages_)
        if (p.name == pg) return p.backdropTex != 0;
    return false;
}

void Menu::activate(int index) {
    if (index < 0 || index >= (int)items_.size()) return;
    sel_ = index;
    if (items_[index].id == "exit") {
        // m\exit -> e\backExitDoit.txr: never quit on the first press
        confirm_ = index;
        confirmChoice_ = 1;
        confirmText_ = "\u0412\u042b\u0419\u0422\u0418 \u0418\u0417 \u0418\u0413\u0420\u042b?";
        openPage("exitconf");
        return;
    }
    if (items_[index].id == "demo") {
        activated_ = "demo";
        return;
    }
    // Until the NEW/LOAD and RECORD/REPLAY controls are implemented, do not
    // strand the player on a decorative page: preserve the old useful action.
    if (items_[index].id == "singl" || items_[index].id == "recor") {
        activated_ = items_[index].id;
        return;
    }
    const char* target = screenFor(items_[index].id);
    // Only open a page whose backdrop art really exists; otherwise hand the id to
    // the App, which either runs the action (ИГРА -> the world) or shows its
    // "not implemented" note.  Without this a missing backdrop showed a black frame.
    const bool available = target && *target && pageAvailable(items_[index].id);
    LOG("menu: route '%s' -> '%s' (%s)", items_[index].id.c_str(),
        target ? target : "-", available ? "art ready" : "art unavailable");
    if (available) openPage(target);
    else activated_ = items_[index].id;
}

const Menu::Page* Menu::currentPage() const {
    for (const Page& p : pages_) if (p.name == page_) return &p;
    return nullptr;
}

void Menu::openPage(const std::string& name) {
    page_ = name;
    message_.clear();
    messageT_ = 0.0f;
    LOG("menu: page '%s'", name.c_str());
}

void Menu::handlePad(PadButton b) {
    // the pad drives the same navigation the keys do, so the two are
    // interchangeable (docs/original_ui.md 5.3 "keyboard/gamepad/mouse parity")
    switch (b) {
        case PadUp:    handleKey(SDLK_UP); break;
        case PadDown:  handleKey(SDLK_DOWN); break;
        case PadLeft:  handleKey(SDLK_LEFT); break;
        case PadRight: handleKey(SDLK_RIGHT); break;
        case PadA:
        case PadStart: handleKey(SDLK_RETURN); break;
        case PadB:     handleKey(SDLK_ESCAPE); break;
    }
}

void Menu::handleMouse(float mx, float my, bool click) {
    mouseX_ = mx;
    mouseY_ = my;
    float vx = (mx - offX_) / scale_;
    float vy = (my - offY_) / scale_;
    if (confirm_ >= 0) {
        // The exit art is shown as a centred card inside common.msk.  Convert
        // its original 800x600 button coordinates to that card's local scale.
        const Page* p = currentPage();
        float fit = 1.0f, px = 0.0f, py = 0.0f;
        if (p && p->overlayW > 0 && p->overlayH > 0) {
            fit = std::min(1.0f, std::min(420.0f / p->overlayW, 400.0f / p->overlayH));
            px = (VW - p->overlayW * fit) * 0.5f;
            py = (VH - p->overlayH * fit) * 0.5f;
        }
        const float yesX = px + (288.0f - (p ? p->overlayX : 0)) * fit;
        const float noX = px + (381.0f - (p ? p->overlayX : 0)) * fit;
        const float by = py + (204.0f - (p ? p->overlayY : 0)) * fit;
        const bool yes = vx >= yesX && vx <= yesX + 75.0f * fit &&
                         vy >= by && vy <= by + 49.0f * fit;
        const bool no = vx >= noX && vx <= noX + 129.0f * fit &&
                        vy >= by && vy <= by + 49.0f * fit;
        if (yes) confirmChoice_ = 0;
        else if (no) confirmChoice_ = 1;
        if (click) {
            if (yes || no) {
                confirmChoice_ = yes ? 0 : 1;
                if (yes) activated_ = items_[confirm_].id;
                confirm_ = -1;
                page_.clear();
            }
        }
        return;
    }
    if (!page_.empty()) return;
    int hit = hitTest(vx, vy);
    if (hit >= 0) {
        sel_ = hit;
        if (click) activate(hit);
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

bool Menu::activateName(const std::string& id) {
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].id != id) continue;
        activate((int)i);
        return true;
    }
    return false;
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

      if (const Page* p = currentPage()) {
          // Keep the primary truck frame and put the selected page into the
          // central opening where the rotating 3D scene normally lives.
          if (offX_ > 0.5f) {
              r.quad2D(white, 0, 0, offX_, (float)vpH_, 0, 0, 0, 1);
              r.quad2D(white, offX_ + VW * scale_, 0, offX_, (float)vpH_, 0, 0, 0, 1);
          }
          if (offY_ > 0.5f) {
              r.quad2D(white, 0, 0, (float)vpW_, offY_, 0, 0, 0, 1);
              r.quad2D(white, 0, offY_ + VH * scale_, (float)vpW_, offY_, 0, 0, 0, 1);
          }
          if (frameTex_)
              r.quad2D(frameTex_, X(0), Y(0), VW * scale_, VH * scale_, 1, 1, 1, 1, 0, 0, 1, 1);
          // The card replaces the central 3D view only; retain all menu buttons.
          for (size_t i = 0; i < items_.size(); ++i) {
              const Item& it = items_[i];
              float x, y, w, h;
              cellRect((int)i, x, y, w, h);
              unsigned tex = (int)i == sel_ && it.texAP ? it.texAP : it.texA;
              if (tex) r.quad2D(tex, X(x), Y(y), w * scale_, h * scale_, 1, 1, 1, 1);
          }

          unsigned card = p->overlayTex ? p->overlayTex : p->backdropTex;
          int cw = p->overlayTex ? p->overlayW : p->backdropW;
          int ch = p->overlayTex ? p->overlayH : p->backdropH;
          float fit = (cw > 0 && ch > 0)
                    ? std::min(1.0f, std::min(420.0f / cw, 400.0f / ch)) : 1.0f;
          float pw = cw * fit, ph = ch * fit;
          float px = (VW - pw) * 0.5f, py = (VH - ph) * 0.5f;
          r.quad2D(white, X(185), Y(90), 430 * scale_, 420 * scale_, 0, 0, 0, 0.60f);
          if (card)
              r.quad2D(card, X(px), Y(py), pw * scale_, ph * scale_, 1, 1, 1, 1);
          if (p->name == "exitconf") {
              unsigned ok = confirmChoice_ == 0 && exitOkSelTex_ ? exitOkSelTex_ : exitOkTex_;
              unsigned no = confirmChoice_ == 1 && exitCancelSelTex_ ? exitCancelSelTex_ : exitCancelTex_;
              const float bx = px + (288.0f - p->overlayX) * fit;
              const float by = py + (204.0f - p->overlayY) * fit;
              if (ok) r.quad2D(ok, X(bx), Y(by), 75 * fit * scale_, 49 * fit * scale_, 1, 1, 1, 1);
              if (no) r.quad2D(no, X(px + (381.0f - p->overlayX) * fit), Y(by),
                               129 * fit * scale_, 49 * fit * scale_, 1, 1, 1, 1);
          }
          if (mouseX_ >= 0 && cursorTex_)
              r.quad2D(cursorTex_, mouseX_, mouseY_, cursorW_ * scale_, cursorH_ * scale_,
                       1, 1, 1, 1, 0, 0, 1, 1);
          r.end2D();
          return;
      }

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

    // 2b) the exit confirmation (e\backExitDoit.txr): a centred plate with the
    //     question and a yes/no pair; the safe answer is preselected.
      if (confirm_ >= 0) {
        const float pw = 420.0f, ph = 130.0f;
        float px = X((VW - pw) * 0.5f), py = Y((VH - ph) * 0.5f);
        r.quad2D(white, X(0), Y(0), (float)VW * scale_, (float)VH * scale_,
                 0, 0, 0, 0.45f);
        r.quad2D(white, px, py, pw * scale_, ph * scale_, 0.08f, 0.10f, 0.14f, 0.96f);
        float t = 2.0f * scale_;
        r.quad2D(white, px, py, pw * scale_, t, 0.85f, 0.72f, 0.30f, 1);
        r.quad2D(white, px, py + ph * scale_ - t, pw * scale_, t, 0.85f, 0.72f, 0.30f, 1);
        font.drawCentered(r, confirmText_, px + pw * 0.5f * scale_, py + 30 * scale_,
                          1.8f * scale_, 1, 1, 1, 1);
        const char* answers[2] = {"\u0414\u0410", "\u041d\u0415\u0422"};
        for (int a = 0; a < 2; ++a) {
            float bx = px + (pw * 0.5f - 150.0f + a * 150.0f) * scale_;
            bool on = (a == confirmChoice_);
            r.quad2D(white, bx, py + 70 * scale_, 110 * scale_, 34 * scale_,
                     on ? 0.20f : 0.12f, on ? 0.34f : 0.18f, on ? 0.20f : 0.12f, 1);
            font.drawCentered(r, answers[a], bx + 55 * scale_, py + 78 * scale_,
                              1.6f * scale_, on ? 1.0f : 0.75f, on ? 0.95f : 0.75f,
                              on ? 0.6f : 0.75f, 1);
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

#include "ModelEditor.h"
#include "gfx/Renderer.h"
#include "gfx/Math3D.h"
#include "ui/Font.h"
#include "util/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ht2 {

namespace {

// 3x3 matrices in the engine's column convention (p' = M p), row-major storage
// m[r*3+c] - the same layout B3D::Anchor::m uses.
void mul3(const float a[9], const float b[9], float out[9]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            float v = 0;
            for (int k = 0; k < 3; ++k) v += a[r * 3 + k] * b[k * 3 + c];
            out[r * 3 + c] = v;
        }
}

void rotYdeg(float deg, float m[9]) {
    const float a = deg * 3.14159265f / 180.0f, c = cosf(a), s = sinf(a);
    const float r[9] = {c, 0, s, 0, 1, 0, -s, 0, c};
    memcpy(m, r, sizeof r);
}
void rotXdeg(float deg, float m[9]) {
    const float a = deg * 3.14159265f / 180.0f, c = cosf(a), s = sinf(a);
    const float r[9] = {1, 0, 0, 0, c, -s, 0, s, c};
    memcpy(m, r, sizeof r);
}
void rotZdeg(float deg, float m[9]) {
    const float a = deg * 3.14159265f / 180.0f, c = cosf(a), s = sinf(a);
    const float r[9] = {c, -s, 0, s, c, 0, 0, 0, 1};
    memcpy(m, r, sizeof r);
}

bool projectPoint(const Mat4& proj, const Mat4& view, float x, float y, float z,
                  int w, int h, float& sx, float& sy) {
    const float vx = view.m[0] * x + view.m[4] * y + view.m[8] * z + view.m[12];
    const float vy = view.m[1] * x + view.m[5] * y + view.m[9] * z + view.m[13];
    const float vz = view.m[2] * x + view.m[6] * y + view.m[10] * z + view.m[14];
    const float vw = view.m[3] * x + view.m[7] * y + view.m[11] * z + view.m[15];
    const float cxx = proj.m[0] * vx + proj.m[4] * vy + proj.m[8] * vz + proj.m[12] * vw;
    const float cyy = proj.m[1] * vx + proj.m[5] * vy + proj.m[9] * vz + proj.m[13] * vw;
    const float cw = proj.m[3] * vx + proj.m[7] * vy + proj.m[11] * vz + proj.m[15] * vw;
    if (cw <= 0.001f) return false;
    sx = ((cxx / cw) * 0.5f + 0.5f) * (float)w;
    sy = (1.0f - ((cyy / cw) * 0.5f + 0.5f)) * (float)h;
    return true;
}

} // namespace

bool ModelEditor::load(Renderer& r, const std::string& dataDir, const std::string& model,
                       Overlay& ov) {
    dataDir_ = dataDir;
    model_ = model;
    ov_ = &ov;
    std::string path = dataDir + "/COMMON/TRUCKS.B3D";
    if (!b3d_.load(path)) {
        LOG("editor: cannot load %s", path.c_str());
        return false;
    }
    // Anchor edits are part of the model, so they must land before the scene is
    // assembled; part/material overrides after it.
    ov_->applyAnchors(b3d_, model_);
    if (!b3d_.buildNamedScene(model_)) {
        LOG("editor: vehicle '%s' not found in %s", model_.c_str(), path.c_str());
        return false;
    }
    pristine_ = b3d_.submeshes();
    // Seed the editor's own state from the overlay so a re-open shows the file.
    for (const Overlay::PartRec& p : ov_->partRecs)
        if (p.model.empty() || p.model == model_) hidden_[p.part] = p.hidden;
    for (const Overlay::MatRec& m : ov_->matRecs)
        if (m.model.empty() || m.model == model_) mats_[m.part] = m.material;
    derive();
    tex_.load(r, dataDir + "/COMMON/TRUCKS");
    textured_ = true;
    r.setSceneMeshes(&subs_, &tex_);
    dirty_ = false;
    fitCamera(1280, 720);
    LOG("editor: vehicle %s: %zu submeshes, %zu parts, %zu anchors from %s",
        model_.c_str(), pristine_.size(), parts_.size(), b3d_.anchorList.size(), path.c_str());
    return true;
}

void ModelEditor::rebuild(Renderer& r, bool anchors) {
    if (anchors && ov_) {
        ov_->applyAnchors(b3d_, model_);
        b3d_.buildNamedScene(model_);
        pristine_ = b3d_.submeshes();
    }
    derive();
    if (r.whiteTex()) r.setSceneMeshes(&subs_, &tex_);
    dirty_ = false;
}

void ModelEditor::derive() {
    subs_.clear();
    subs_.reserve(pristine_.size());
    for (const B3D::SubMesh& sm : pristine_) {
        auto m = mats_.find(sm.name);
        const bool hide = [&] {
            auto h = hidden_.find(sm.name);
            return h != hidden_.end() && h->second;
        }();
        if (hide) continue;
        subs_.push_back(sm);
        if (m != mats_.end()) subs_.back().material = m->second;
    }
    // group by node name
    parts_.clear();
    std::map<std::string, int> index;
    for (size_t i = 0; i < pristine_.size(); ++i) {
        const B3D::SubMesh& sm = pristine_[i];
        auto it = index.find(sm.name);
        int pi;
        if (it == index.end()) {
            pi = (int)parts_.size();
            index[sm.name] = pi;
            parts_.emplace_back();
            parts_.back().name = sm.name;
            parts_.back().bb[0] = parts_.back().bb[2] = parts_.back().bb[4] = 1e30f;
            parts_.back().bb[1] = parts_.back().bb[3] = parts_.back().bb[5] = -1e30f;
        } else pi = it->second;
        PartInfo& p = parts_[(size_t)pi];
        p.sub.push_back((int)i);
        p.tris += (int)(sm.idx.size() / 3);
        if (std::find(p.materials.begin(), p.materials.end(), sm.material) == p.materials.end())
            p.materials.push_back(sm.material);
        for (size_t v = 0; v + 2 < sm.pos.size(); v += 3)
            for (int k = 0; k < 3; ++k) {
                p.bb[k * 2 + 0] = std::min(p.bb[k * 2 + 0], sm.pos[v + (size_t)k]);
                p.bb[k * 2 + 1] = std::max(p.bb[k * 2 + 1], sm.pos[v + (size_t)k]);
            }
        auto h = hidden_.find(sm.name);
        p.hidden = h != hidden_.end() && h->second;
    }
    for (PartInfo& p : parts_)
        if (p.bb[0] > p.bb[1]) { for (int k = 0; k < 6; ++k) p.bb[k] = 0; }
    buildAnchorView();
}

void ModelEditor::buildAnchorView() {
    anchorView_.clear();
    if (b3d_.usedAnchors.empty()) {
        for (size_t i = 0; i < b3d_.anchorList.size(); ++i) anchorView_.push_back((int)i);
    } else {
        for (size_t i = 0; i < b3d_.anchorList.size(); ++i) {
            const std::string& nm = b3d_.anchorList[i].first;
            for (const std::string& u : b3d_.usedAnchors)
                if (u == nm) { anchorView_.push_back((int)i); break; }
        }
    }
    if (selAnchor_ >= (int)anchorView_.size()) selAnchor_ = (int)anchorView_.size() - 1;
}

size_t ModelEditor::triangleCount() const {
    size_t n = 0;
    for (const B3D::SubMesh& sm : subs_) n += sm.idx.size() / 3;
    return n;
}

// ---------------------------------------------------------------------------
// selection
// ---------------------------------------------------------------------------
const ModelEditor::PartInfo* ModelEditor::part(int i) const {
    if (i < 0 || i >= (int)parts_.size()) return nullptr;
    return &parts_[(size_t)i];
}

const std::pair<std::string, B3D::Anchor>* ModelEditor::anchor(int i) const {
    if (i < 0 || i >= (int)anchorView_.size()) return nullptr;
    return &b3d_.anchorList[(size_t)anchorView_[(size_t)i]];
}

bool ModelEditor::selectPart(const std::string& name) {
    for (size_t i = 0; i < parts_.size(); ++i)
        if (parts_[i].name == name) { selPart_ = (int)i; return true; }
    return false;
}

void ModelEditor::selectPartIndex(int i) { selPart_ = i; }

void ModelEditor::cyclePart(int d) {
    if (parts_.empty()) return;
    int i = selPart_ < 0 ? 0 : selPart_;
    i = (i + d) % (int)parts_.size();
    if (i < 0) i += (int)parts_.size();
    selPart_ = i;
}

bool ModelEditor::selectAnchor(const std::string& name) {
    for (size_t i = 0; i < anchorView_.size(); ++i)
        if (b3d_.anchorList[(size_t)anchorView_[i]].first == name) {
            selAnchor_ = (int)i;
            return true;
        }
    // still allow selecting an anchor of another model (it is in the container)
    for (size_t i = 0; i < b3d_.anchorList.size(); ++i)
        if (b3d_.anchorList[i].first == name) {
            anchorView_.push_back((int)i);
            selAnchor_ = (int)anchorView_.size() - 1;
            return true;
        }
    return false;
}

void ModelEditor::cycleAnchor(int d) {
    if (anchorView_.empty()) return;
    int i = selAnchor_ < 0 ? 0 : selAnchor_;
    i = (i + d) % (int)anchorView_.size();
    if (i < 0) i += (int)anchorView_.size();
    selAnchor_ = i;
}

// ---------------------------------------------------------------------------
// editing
// ---------------------------------------------------------------------------
bool ModelEditor::setPartHidden(const std::string& name, bool hide) {
    bool known = false;
    for (const PartInfo& p : parts_) if (p.name == name) known = true;
    if (!known) { LOG("editor: part '%s' not in vehicle %s", name.c_str(), model_.c_str()); return false; }
    hidden_[name] = hide;
    if (ov_) ov_->setPart(model_, name, hide);
    derive();
    dirty_ = true;
    return true;
}

bool ModelEditor::setPartMaterial(const std::string& name, const std::string& material) {
    bool known = false;
    for (const PartInfo& p : parts_) if (p.name == name) known = true;
    if (!known) { LOG("editor: part '%s' not in vehicle %s", name.c_str(), model_.c_str()); return false; }
    mats_[name] = material;
    if (ov_) ov_->setPartMaterial(model_, name, material);
    derive();
    dirty_ = true;
    return true;
}

bool ModelEditor::moveAnchor(Renderer& r, const std::string& name, double dx, double dy,
                             double dz) {
    B3D::Anchor a;
    if (!b3d_.anchorOf(name, a)) { LOG("editor: anchor '%s' not found", name.c_str()); return false; }
    a.t[0] += (float)dx;
    a.t[1] += (float)dy;
    a.t[2] += (float)dz;
    if (!b3d_.setAnchor(name, a)) return false;
    if (ov_) ov_->setAnchorT(model_, name, a.t);
    rebuild(r, true);
    return true;
}

bool ModelEditor::setAnchorPos(Renderer& r, const std::string& name, double x, double y,
                               double z) {
    B3D::Anchor a;
    if (!b3d_.anchorOf(name, a)) { LOG("editor: anchor '%s' not found", name.c_str()); return false; }
    a.t[0] = (float)x; a.t[1] = (float)y; a.t[2] = (float)z;
    if (!b3d_.setAnchor(name, a)) return false;
    if (ov_) ov_->setAnchorT(model_, name, a.t);
    rebuild(r, true);
    return true;
}

bool ModelEditor::rotateAnchor(Renderer& r, const std::string& name, double dyaw,
                               double dpitch, double droll) {
    B3D::Anchor a;
    if (!b3d_.anchorOf(name, a)) { LOG("editor: anchor '%s' not found", name.c_str()); return false; }
    float ry[9], rx[9], rz[9], t[9], rot[9];
    rotYdeg((float)dyaw, ry);
    rotXdeg((float)dpitch, rx);
    rotZdeg((float)droll, rz);
    mul3(ry, rx, t);
    mul3(t, rz, rot);
    float out[9];
    mul3(rot, a.m, out);
    memcpy(a.m, out, sizeof out);
    if (!b3d_.setAnchor(name, a)) return false;
    if (ov_) ov_->setAnchorM(model_, name, a.m);
    rebuild(r, true);
    return true;
}

// ---------------------------------------------------------------------------
// view
// ---------------------------------------------------------------------------
void ModelEditor::setAngles(float yaw, float pitch) {
    yaw_ = yaw;
    pitch_ = std::max(-89.0f, std::min(89.0f, pitch));
}

void ModelEditor::setDistance(float d) { dist_ = std::max(1.0f, d); }

void ModelEditor::fitCamera(int w, int h) {
    viewW_ = w; viewH_ = h;
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (const B3D::SubMesh& sm : subs_)
        for (size_t v = 0; v + 2 < sm.pos.size(); v += 3)
            for (int k = 0; k < 3; ++k) {
                mn[k] = std::min(mn[k], sm.pos[v + (size_t)k]);
                mx[k] = std::max(mx[k], sm.pos[v + (size_t)k]);
            }
    if (mn[0] > mx[0]) { for (int k = 0; k < 3; ++k) { mn[k] = -1; mx[k] = 1; } }
    // b3d (x, z, height) -> world (x, y = height, z = z)
    centre_[0] = 0.5f * (mn[0] + mx[0]);
    centre_[1] = 0.5f * (mn[2] + mx[2]);
    centre_[2] = 0.5f * (mn[1] + mx[1]);
    ext_ = std::max(mx[0] - mn[0], std::max(mx[1] - mn[1], mx[2] - mn[2]));
    if (ext_ < 1.0f) ext_ = 1.0f;
    dist_ = ext_ * 2.4f;
}

void ModelEditor::draw(Renderer& r, Font& f, int w, int h) {
    viewW_ = w; viewH_ = h;
    if (dirty_ && r.whiteTex()) { r.setSceneMeshes(&subs_, &tex_); dirty_ = false; }

    const float yaw = yaw_ * 3.14159265f / 180.0f;
    const float pitch = pitch_ * 3.14159265f / 180.0f;
    const float fx = sinf(yaw) * cosf(pitch), fy = sinf(pitch), fz = -cosf(yaw) * cosf(pitch);
    const float ex = centre_[0] - fx * dist_;
    const float ey = centre_[1] - fy * dist_;
    const float ez = centre_[2] - fz * dist_;

    r.setFog(std::max(200.0f, dist_ * 4.0f));
    r.setFogColor(0.12f, 0.13f, 0.16f);
    r.setSkyZenith(0.22f, 0.26f, 0.34f);
    r.setSun(0.45f, 0.80f, 0.35f);
    r.setAmbient(0.62f);
    r.clear();
    r.drawSky(pitch_);
    r.drawScene(ex, ey, ez, yaw_, pitch_);

    const unsigned white = r.whiteTex();
    r.begin2D();

    // anchor markers: t is b3d (x, z, height) -> world (x, height, z).  The
    // container holds the anchors of *every* vehicle, so only the ones this
    // model's links actually placed are drawn (B3D::usedAnchors).
    Mat4 proj = Mat4::perspective(60.0f, (float)w / (float)h, 1.0f,
                                  std::max(200.0f, dist_ * 4.0f));
    proj.m[0] = -proj.m[0];
    Mat4 view = Mat4::lookAt(ex, ey, ez,
                             ex + fx, ey + fy, ez + fz);
    for (size_t i = 0; i < anchorView_.size(); ++i) {
        const std::string& nm = b3d_.anchorList[(size_t)anchorView_[i]].first;
        const B3D::Anchor& a = b3d_.anchorList[(size_t)anchorView_[i]].second;
        float sx, sy;
        if (!projectPoint(proj, view, a.t[0], a.t[2], a.t[1], w, h, sx, sy)) continue;
        const bool sel = ((int)i == selAnchor_);
        const float s = sel ? 8.0f : 5.0f;
        const float cr = sel ? 1.0f : 0.35f, cg = sel ? 0.30f : 0.95f,
                    cb = sel ? 0.30f : 0.55f;
        r.line2D(white, sx - s, sy, sx + s, sy, 2.0f, cr, cg, cb, 1);
        r.line2D(white, sx, sy - s, sx, sy + s, 2.0f, cr, cg, cb, 1);
        if (sel) f.draw(r, nm, sx + 10, sy - 6, 1.1f, 1, 0.6f, 0.6f, 1);
    }

    // --- chrome -----------------------------------------------------------
    r.quad2D(white, 0, 0, (float)w, 34, 0.05f, 0.06f, 0.08f, 0.85f);
    char line[512];
    snprintf(line, sizeof line,
             "ht2editor  MODEL  %s  parts=%zu  submeshes=%zu  tris=%zu  anchors=%zu  "
             "yaw=%.0f pitch=%.0f",
             model_.c_str(), parts_.size(), subs_.size(), triangleCount(),
             anchorView_.size(), yaw_, pitch_);
    f.draw(r, line, 8, 8, 1.4f, 1, 1, 1, 1);

    // parts panel (scrolled so the selection stays visible)
    const float px0 = 8, py0 = 44, rowH = 15.0f;
    const int maxRows = std::max(1, (int)((h - 130) / rowH));
    int first = 0;
    if (selPart_ >= maxRows) first = selPart_ - maxRows + 1;
    if (first > (int)parts_.size() - maxRows) first = std::max(0, (int)parts_.size() - maxRows);
    const int shown = std::min((int)parts_.size() - first, maxRows);
    r.quad2D(white, px0 - 4, py0 - 4, 344, rowH * (float)shown + 8,
             0.05f, 0.06f, 0.08f, 0.72f);
    for (int k = 0; k < shown; ++k) {
        const int i = first + k;
        const PartInfo& p = parts_[(size_t)i];
        const bool sel = (i == selPart_);
        char row[256];
        snprintf(row, sizeof row, "%s%-30s %5d tri %s", p.hidden ? "[x] " : "    ",
                 p.name.c_str(), p.tris,
                 p.materials.empty() ? "-" : p.materials.front().c_str());
        f.draw(r, row, px0, py0 + rowH * (float)k, 1.1f, sel ? 1.0f : 0.85f,
               sel ? 1.0f : 0.88f, sel ? 0.6f : 0.92f, 1);
    }

    // anchors panel (right), same scroll rule
    const float ax0 = (float)w - 300, ay0 = 44;
    int afirst = 0;
    if (selAnchor_ >= maxRows) afirst = selAnchor_ - maxRows + 1;
    if (afirst > (int)anchorView_.size() - maxRows)
        afirst = std::max(0, (int)anchorView_.size() - maxRows);
    const int ashown = std::min((int)anchorView_.size() - afirst, maxRows);
    r.quad2D(white, ax0 - 4, ay0 - 4, 296, rowH * (float)ashown + 8,
             0.05f, 0.06f, 0.08f, 0.72f);
    for (int k = 0; k < ashown; ++k) {
        const int i = afirst + k;
        const bool sel = (i == selAnchor_);
        const B3D::Anchor& a = b3d_.anchorList[(size_t)anchorView_[(size_t)i]].second;
        char row[256];
        snprintf(row, sizeof row, "%-22s t %.2f %.2f %.2f",
                 b3d_.anchorList[(size_t)anchorView_[(size_t)i]].first.c_str(),
                 (double)a.t[0], (double)a.t[1], (double)a.t[2]);
        f.draw(r, row, ax0, ay0 + rowH * (float)k, 1.1f, sel ? 1.0f : 0.85f,
               sel ? 1.0f : 0.88f, sel ? 0.6f : 0.92f, 1);
    }

    if (const PartInfo* p = part(selPart_)) {
        snprintf(line, sizeof line,
                 "part %s  tris=%d  bbox x[%.1f %.1f] z[%.1f %.1f] h[%.1f %.1f]  hidden=%d",
                 p->name.c_str(), p->tris, (double)p->bb[0], (double)p->bb[1],
                 (double)p->bb[2], (double)p->bb[3], (double)p->bb[4], (double)p->bb[5],
                 p->hidden ? 1 : 0);
        f.draw(r, line, 8, (float)h - 70, 1.2f, 0.85f, 0.95f, 1.0f, 1);
    }
    if (const std::pair<std::string, B3D::Anchor>* a = anchor(selAnchor_)) {
        snprintf(line, sizeof line, "anchor %s  t %.3f %.3f %.3f", a->first.c_str(),
                 (double)a->second.t[0], (double)a->second.t[1], (double)a->second.t[2]);
        f.draw(r, line, 8, (float)h - 52, 1.2f, 0.85f, 0.95f, 0.7f, 1);
    }
    if (!status_.empty()) f.draw(r, status_, 8, (float)h - 34, 1.2f, 1.0f, 0.9f, 0.4f, 1);
    f.draw(r, "drag orbit  wheel zoom  TAB next part  H hide  A next anchor  "
              "arrows move anchor  Q/E rotate anchor", 8, (float)h - 16, 1.1f, 0.7f, 0.75f, 0.8f, 1);
    r.end2D();
}

bool ModelEditor::event(Renderer& r, const SDL_Event& e, int w, int h) {
    viewW_ = w; viewH_ = h;
    switch (e.type) {
        case SDL_MOUSEMOTION:
            mouseX_ = (float)e.motion.x;
            mouseY_ = (float)e.motion.y;
            if (dragging_) {
                yaw_ += e.motion.xrel * 0.4f;
                pitch_ = std::max(-89.0f, std::min(89.0f, pitch_ + e.motion.yrel * 0.4f));
            }
            return true;
        case SDL_MOUSEBUTTONDOWN:
            if (e.button.button == SDL_BUTTON_LEFT) {
                // click the parts panel to select
                if (e.button.x < 352 && e.button.y > 44) {
                    int i = (int)((e.button.y - 44) / 15.0f);
                    if (i >= 0 && i < (int)parts_.size()) { selPart_ = i; return true; }
                }
                if (e.button.x > w - 308 && e.button.y > 44) {
                    int i = (int)((e.button.y - 44) / 15.0f);
                    if (i >= 0 && i < (int)anchorView_.size()) { selAnchor_ = i; return true; }
                }
                dragging_ = true;
            }
            return true;
        case SDL_MOUSEBUTTONUP:
            dragging_ = false;
            return true;
        case SDL_MOUSEWHEEL:
            dist_ = std::max(ext_ * 0.2f, std::min(ext_ * 20.0f,
                             dist_ * (e.wheel.y > 0 ? 0.87f : 1.15f)));
            return true;
        case SDL_KEYDOWN:
            switch (e.key.keysym.sym) {
                case SDLK_TAB: cyclePart(e.key.keysym.mod & KMOD_SHIFT ? -1 : 1); return true;
                case SDLK_a: cycleAnchor(e.key.keysym.mod & KMOD_SHIFT ? -1 : 1); return true;
                case SDLK_h: {
                    const PartInfo* p = part(selPart_);
                    if (p) setPartHidden(p->name, !p->hidden);
                    return true;
                }
                case SDLK_UP: if (anchor(selAnchor_)) moveAnchor(r, anchor(selAnchor_)->first, 0, 0, -1); return true;
                case SDLK_DOWN: if (anchor(selAnchor_)) moveAnchor(r, anchor(selAnchor_)->first, 0, 0, 1); return true;
                case SDLK_LEFT: if (anchor(selAnchor_)) moveAnchor(r, anchor(selAnchor_)->first, -1, 0, 0); return true;
                case SDLK_RIGHT: if (anchor(selAnchor_)) moveAnchor(r, anchor(selAnchor_)->first, 1, 0, 0); return true;
                case SDLK_q: if (anchor(selAnchor_)) rotateAnchor(r, anchor(selAnchor_)->first, -5, 0, 0); return true;
                case SDLK_e: if (anchor(selAnchor_)) rotateAnchor(r, anchor(selAnchor_)->first, 5, 0, 0); return true;
                default: break;
            }
            return false;
        default: return false;
    }
}

// ---------------------------------------------------------------------------
// reports
// ---------------------------------------------------------------------------
void ModelEditor::printList() const {
    LOG("editor: vehicle %s: %zu submeshes, %zu parts, %zu anchors (%zu in the container)",
        model_.c_str(), pristine_.size(), parts_.size(), anchorView_.size(),
        b3d_.anchorList.size());
    for (size_t i = 0; i < pristine_.size(); ++i) {
        const B3D::SubMesh& sm = pristine_[i];
        // the *effective* state (overlay applied), so a re-open shows the file
        auto m = mats_.find(sm.name);
        auto h = hidden_.find(sm.name);
        const std::string mat = m != mats_.end() ? m->second : sm.material;
        const bool hid = h != hidden_.end() && h->second;
        float bb[6] = {1e30f, -1e30f, 1e30f, -1e30f, 1e30f, -1e30f};
        for (size_t v = 0; v + 2 < sm.pos.size(); v += 3)
            for (int k = 0; k < 3; ++k) {
                bb[k * 2 + 0] = std::min(bb[k * 2 + 0], sm.pos[v + (size_t)k]);
                bb[k * 2 + 1] = std::max(bb[k * 2 + 1], sm.pos[v + (size_t)k]);
            }
        char bbs[128];
        snprintf(bbs, sizeof bbs, "bbox x[%.2f %.2f] z[%.2f %.2f] h[%.2f %.2f]",
                 (double)bb[0], (double)bb[1], (double)bb[2], (double)bb[3],
                 (double)bb[4], (double)bb[5]);
        LOG("editor: submesh %-24s material=%-22s tris=%4zu r=%.2f hidden=%d %s", sm.name.c_str(),
            mat.c_str(), sm.idx.size() / 3, (double)sm.radius, hid ? 1 : 0, bbs);
    }
    for (const PartInfo& p : parts_)
        LOG("editor: part %-24s submeshes=%zu tris=%d hidden=%d materials=%s", p.name.c_str(),
            p.sub.size(), p.tris, p.hidden ? 1 : 0,
            p.materials.empty() ? "-" : p.materials.front().c_str());
    for (size_t i = 0; i < b3d_.anchorList.size(); ++i) {
        const B3D::Anchor& a = b3d_.anchorList[i].second;
        LOG("editor: anchor %-24s t=(%.3f %.3f %.3f) m=[%.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f]",
            b3d_.anchorList[i].first.c_str(), (double)a.t[0], (double)a.t[1], (double)a.t[2],
            (double)a.m[0], (double)a.m[1], (double)a.m[2], (double)a.m[3], (double)a.m[4],
            (double)a.m[5], (double)a.m[6], (double)a.m[7], (double)a.m[8]);
    }
}

void ModelEditor::printReport() const {
    LOG("editor: mode=model vehicle=%s parts=%zu submeshes=%zu tris=%zu anchors=%zu sel_part=%s sel_anchor=%s",
        model_.c_str(), parts_.size(), subs_.size(), triangleCount(), anchorView_.size(),
        part(selPart_) ? part(selPart_)->name.c_str() : "-",
        anchor(selAnchor_) ? anchor(selAnchor_)->first.c_str() : "-");
}

} // namespace ht2

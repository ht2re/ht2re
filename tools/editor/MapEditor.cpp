#include "MapEditor.h"
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

// Column-major 4x4 (Math3D.h) times a point; returns false when the point is
// behind the camera.  This replicates Renderer::drawScene()'s projection
// exactly (same FOV, same Direct3D->OpenGL x mirror) so the 2D wireframe lands
// on top of the 3D geometry the same renderer drew.
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
    const float nx = cxx / cw, ny = cyy / cw;
    sx = (nx * 0.5f + 0.5f) * (float)w;
    sy = (1.0f - (ny * 0.5f + 0.5f)) * (float)h;
    return true;
}

void segColor(int attr, float& r, float& g, float& b) {
    // ATTR is not decoded yet (docs mention the FLAG dword); colour by the low
    // bits so the different classes are at least distinguishable.
    switch ((unsigned)attr & 0x7u) {
        case 0: r = 0.95f; g = 0.80f; b = 0.25f; break;   // default main road
        case 1: r = 0.35f; g = 0.85f; b = 0.95f; break;
        case 2: r = 0.95f; g = 0.45f; b = 0.25f; break;
        case 3: r = 0.55f; g = 0.95f; b = 0.45f; break;
        case 4: r = 0.85f; g = 0.55f; b = 0.95f; break;
        case 5: r = 0.95f; g = 0.95f; b = 0.95f; break;
        case 6: r = 0.55f; g = 0.65f; b = 0.95f; break;
        default: r = 0.90f; g = 0.60f; b = 0.20f; break;
    }
}

} // namespace

// ---------------------------------------------------------------------------
bool MapEditor::load(Renderer& r, const std::string& dataDir, const std::string& tile,
                     Overlay& ov) {
    (void)r;
    dataDir_ = dataDir;
    tile_ = tile;
    ov_ = &ov;
    std::string path = dataDir + "/ENV/" + tile + ".way";
    if (!pristine_.load(path)) {
        LOG("editor: cannot load %s", path.c_str());
        return false;
    }
    rebuildWorking();
    return true;
}

void MapEditor::rebuildWorking() {
    // way_ is always derived: shipped .way + the overlay records that apply.
    // Structural edits (add/delete node, add/delete segment) therefore need no
    // independent bookkeeping in the editor - the overlay *is* the model.
    way_ = pristine_;
    if (ov_) ov_->applyWay(way_, tile_);
    rebuildNodeIndex();
    if (!selNodeName_.empty()) {
        for (size_t i = 0; i < nodes_.size(); ++i) {
            const Way::Node* n = node((int)i);
            if (n && n->name == selNodeName_) { selNode_ = (int)i; break; }
        }
    }
    if (selNode_ >= (int)nodes_.size()) selNode_ = (int)nodes_.size() - 1;
}

void MapEditor::rebuildNodeIndex() {
    nodes_.clear();
    for (size_t r = 0; r < way_.rooms.size(); ++r)
        for (size_t n = 0; n < way_.rooms[r].nodes.size(); ++n)
            nodes_.push_back({(int)r, (int)n});
}

const Way::Node* MapEditor::node(int i) const {
    if (i < 0 || i >= (int)nodes_.size()) return nullptr;
    const NRef& ref = nodes_[(size_t)i];
    if (ref.room < 0 || ref.room >= (int)way_.rooms.size()) return nullptr;
    const Way::Room& room = way_.rooms[(size_t)ref.room];
    if (ref.node < 0 || ref.node >= (int)room.nodes.size()) return nullptr;
    return &room.nodes[(size_t)ref.node];
}

const Way::Seg* MapEditor::seg(int shippedIndex) const {
    if (shippedIndex < 0 || shippedIndex >= (int)pristine_.segs.size()) return nullptr;
    int e = effSegIndex(shippedIndex);
    if (e < 0 || e >= (int)way_.segs.size()) return nullptr;
    return &way_.segs[(size_t)e];
}

// Shipped segments survive in order (deleted ones are removed), added ones
// land at the end - exactly what Overlay::applyWay() builds, so the mapping is
// a running "how many were deleted before me".
int MapEditor::effSegIndex(int shippedIndex) const {
    if (shippedIndex < 0 || shippedIndex >= (int)pristine_.segs.size()) return -1;
    int shift = 0;
    for (int i = 0; i < shippedIndex; ++i)
        if (ov_ && ov_->touchesTile(tile_)) {
            for (const Overlay::SegDel& d : ov_->segDels)
                if ((d.tile.empty() || d.tile == "*" || d.tile == tile_) && d.index == i) {
                    ++shift;
                    break;
                }
        }
    int e = shippedIndex - shift;
    return e < (int)way_.segs.size() ? e : -1;
}

// ---------------------------------------------------------------------------
// selection
// ---------------------------------------------------------------------------
bool MapEditor::selectNode(const std::string& name) {
    for (size_t i = 0; i < nodes_.size(); ++i) {
        const Way::Node* n = node((int)i);
        if (n && n->name == name) {
            selNode_ = (int)i;
            selNodeName_ = name;
            return true;
        }
    }
    return false;
}

void MapEditor::selectNodeIndex(int i) {
    selNode_ = i;
    const Way::Node* n = node(i);
    selNodeName_ = n ? n->name : std::string();
}

void MapEditor::selectSegIndex(int shippedIndex) {
    selSeg_ = (shippedIndex >= 0 && shippedIndex < (int)pristine_.segs.size())
                  ? shippedIndex : -1;
}

void MapEditor::cycleNode(int delta) {
    if (nodes_.empty()) return;
    int i = selNode_ < 0 ? 0 : selNode_;
    i = (i + delta) % (int)nodes_.size();
    if (i < 0) i += (int)nodes_.size();
    selectNodeIndex(i);
}

// ---------------------------------------------------------------------------
// editing
// ---------------------------------------------------------------------------
bool MapEditor::moveNodeAbs(int i, double x, double y, double z) {
    const Way::Node* n = node(i);
    if (!n || !ov_) return false;
    Way::Node copy = *n;
    copy.pos.x = x; copy.pos.y = y; copy.pos.z = z;
    ov_->setNodePos(tile_, copy);
    selNodeName_ = copy.name;
    rebuildWorking();
    return true;
}

bool MapEditor::nudgeNode(int i, double dx, double dy, double dz) {
    const Way::Node* n = node(i);
    if (!n) return false;
    return moveNodeAbs(i, n->pos.x + dx, n->pos.y + dy, n->pos.z + dz);
}

bool MapEditor::setNodeFlags(int i, uint32_t flags) {
    const Way::Node* n = node(i);
    if (!n || !ov_) return false;
    ov_->setNodeFlags(tile_, n->name, flags);
    rebuildWorking();
    return true;
}

bool MapEditor::addNode(const std::string& name, double x, double y, double z,
                        const std::string& room) {
    if (!ov_ || name.empty()) return false;
    for (size_t i = 0; i < nodes_.size(); ++i) {
        const Way::Node* n = node((int)i);
        if (n && n->name == name) { LOG("editor: node '%s' already exists", name.c_str()); return false; }
    }
    Way::Node n;
    n.name = name;
    n.pos.x = x; n.pos.y = y; n.pos.z = z;
    std::string rm = room;
    if (rm.empty()) rm = way_.rooms.empty() ? std::string("editor") : way_.rooms.front().name;
    ov_->addNode(tile_, rm, n);
    selNodeName_ = name;
    rebuildWorking();
    return true;
}

bool MapEditor::deleteNode(const std::string& name) {
    if (!ov_ || name.empty()) return false;
    ov_->delNode(tile_, name);
    if (selNodeName_ == name) selNodeName_.clear();
    rebuildWorking();
    return true;
}

bool MapEditor::connectNodes(const std::string& a, const std::string& b, double width) {
    if (!ov_) return false;
    const Way::Node* na = nullptr;
    const Way::Node* nb = nullptr;
    for (size_t i = 0; i < nodes_.size(); ++i) {
        const Way::Node* n = node((int)i);
        if (!n) continue;
        if (n->name == a) na = n;
        if (n->name == b) nb = n;
    }
    if (!na || !nb) {
        LOG("editor: connect: node '%s' or '%s' not found", a.c_str(), b.c_str());
        return false;
    }
    Overlay::SegAdd s;
    s.tile = tile_;
    s.name = a + "__" + b;
    s.attr = 0;
    s.width = width > 0 ? width : 8.0;
    s.width1 = 0.0;
    s.pts.push_back(na->pos);
    s.pts.push_back(nb->pos);
    ov_->addSeg(s);
    rebuildWorking();
    return true;
}

bool MapEditor::setSegFields(int shippedIndex, int attr, double width, double width1) {
    if (!ov_ || shippedIndex < 0 || shippedIndex >= (int)pristine_.segs.size()) return false;
    ov_->setSeg(tile_, shippedIndex, attr, width, width1);
    rebuildWorking();
    return true;
}

bool MapEditor::deleteSeg(int shippedIndex) {
    if (!ov_ || shippedIndex < 0 || shippedIndex >= (int)pristine_.segs.size()) return false;
    ov_->delSeg(tile_, shippedIndex);
    if (selSeg_ == shippedIndex) selSeg_ = -1;
    rebuildWorking();
    return true;
}

// ---------------------------------------------------------------------------
// view
// ---------------------------------------------------------------------------
void MapEditor::bounds(double& x0, double& z0, double& x1, double& z1) const {
    x0 = z0 = 1e30;
    x1 = z1 = -1e30;
    auto add = [&](double x, double z) {
        x0 = std::min(x0, x); x1 = std::max(x1, x);
        z0 = std::min(z0, z); z1 = std::max(z1, z);
    };
    for (size_t i = 0; i < nodes_.size(); ++i) {
        const Way::Node* n = node((int)i);
        if (n) add(n->pos.x, n->pos.z);
    }
    for (const Way::Seg& s : way_.segs)
        for (const Way::Pos& p : s.pts) add(p.x, p.z);
    if (x0 > x1) { x0 = z0 = -100; x1 = z1 = 100; }
}

void MapEditor::fitView(int w, int h) {
    viewW_ = w; viewH_ = h;
    halfW_ = w * 0.5f;
    halfH_ = h * 0.5f;
    double x0, z0, x1, z1;
    bounds(x0, z0, x1, z1);
    cx_ = 0.5 * (x0 + x1);
    cz_ = 0.5 * (z0 + z1);
    double dx = std::max(1.0, x1 - x0), dz = std::max(1.0, z1 - z0);
    double sx = (w * 0.86) / dx, sz = (h * 0.86) / dz;
    scale_ = (float)std::min(sx, sz);
    // 3D camera framing: above one corner looking down at the centre.
    float cy = 0;
    for (size_t i = 0; i < nodes_.size(); ++i) {
        const Way::Node* n = node((int)i);
        if (n) cy = std::max(cy, (float)n->pos.y);
    }
    const float ext = (float)std::max(dx, dz);
    focus_[0] = (float)cx_;
    focus_[1] = cy;
    focus_[2] = (float)cz_;
    cam3_[0] = (float)cx_;
    cam3_[1] = cy + ext * 0.9f + 40.0f;
    cam3_[2] = (float)cz_ + ext * 0.9f;
    cam3_[3] = 0.0f;
    cam3_[4] = -(float)(std::atan2(cam3_[1] - cy, cam3_[2] - (float)cz_) * 180.0 / 3.14159265358979);
}

void MapEditor::setCamera(const float cam[5]) {
    for (int i = 0; i < 5; ++i) cam3_[i] = cam[i];
}

bool MapEditor::prepare3D(Renderer& r) {
    if (groundBuilt_) return true;
    std::string base = dataDir_ + "/ENV/" + tile_;
    envLoaded_ = env_.load(base + ".b3d");
    if (!envLoaded_) { LOG("editor: no %s.b3d (3D context)", base.c_str()); return false; }
    env_.buildScene();
    // The ground the game derives from the tile geometry: same Terrain + the
    // same GroundMap::build()/setRoadSegments()/buildRoadSurface() sequence
    // App::loadWorldTiles() runs for the whole island, here for the one tile.
    terrain_.load(dataDir_ + "/MTR/terrain0.raw");
    std::vector<float> roadPts, roadSegs;
    for (size_t s = 0; s < way_.segs.size(); ++s)
        for (const Way::Pos& p : way_.segs[s].pts) {
            roadPts.push_back((float)p.x);
            roadPts.push_back((float)p.z);
            roadPts.push_back((float)p.y);
            roadSegs.push_back((float)p.x);
            roadSegs.push_back((float)p.z);
            roadSegs.push_back((float)p.y);
            roadSegs.push_back((float)s);
        }
    ground_.build(env_.submeshes(), roadPts, 12.0f, terrain_.loaded() ? &terrain_ : nullptr);
    ground_.setRoadSegments(roadSegs);
    ground_.buildRoadSurface(env_.submeshes());
    groundBuilt_ = true;
    r.setTerrain(&terrain_);
    r.setGroundMap(&ground_);
    r.setRoads(&way_);
    LOG("editor: 3D context %s.b3d: %zu submeshes, ground %zu road cells",
        base.c_str(), env_.submeshes().size(), ground_.roadCells());
    return true;
}

bool MapEditor::texture3D(Renderer& r) {
    if (texLoaded_) return true;
    if (!envLoaded_ && !prepare3D(r)) return false;
    std::string base = dataDir_ + "/ENV/" + tile_;
    tex_.load(r, base);
    r.setSceneMeshes(&env_.submeshes(), &tex_);
    texLoaded_ = true;
    LOG("editor: 3D textures from %s.res: %zu decoded", base.c_str(), tex_.decoded());
    return true;
}

// ---------------------------------------------------------------------------
// picking / input
// ---------------------------------------------------------------------------
int MapEditor::pickNode(float mx, float my, float radius) const {
    int best = -1;
    float bestD = radius;
    for (size_t i = 0; i < nodes_.size(); ++i) {
        const Way::Node* n = node((int)i);
        if (!n) continue;
        float dx = px(n->pos.x) - mx, dy = pz(n->pos.z) - my;
        float d = sqrtf(dx * dx + dy * dy);
        if (d <= bestD) { bestD = d; best = (int)i; }
    }
    return best;
}

bool MapEditor::event(Renderer& r, const SDL_Event& e, int w, int h) {
    (void)r;
    viewW_ = w; viewH_ = h;
    halfW_ = w * 0.5f;
    halfH_ = h * 0.5f;
    switch (e.type) {
        case SDL_MOUSEMOTION:
            mouseX_ = (float)e.motion.x;
            mouseY_ = (float)e.motion.y;
            if (dragging_) {
                if (view3d_) {
                    cam3_[3] -= (mouseX_ - dragMx_) * 0.3f;
                    cam3_[4] = std::max(-89.0f, std::min(89.0f,
                                        cam3_[4] - (mouseY_ - dragMy_) * 0.3f));
                } else if (selNode_ >= 0) {
                    const Way::Node* n = node(selNode_);
                    if (n) moveNodeAbs(selNode_, wx(mouseX_), n->pos.y, wz(mouseY_));
                }
                dragMx_ = (int)mouseX_;
                dragMy_ = (int)mouseY_;
            } else if (panning_) {
                if (view3d_) {
                    // world units per pixel at the focus distance
                    float d = std::max(1.0f, std::sqrt(
                        (cam3_[0] - focus_[0]) * (cam3_[0] - focus_[0]) +
                        (cam3_[1] - focus_[1]) * (cam3_[1] - focus_[1]) +
                        (cam3_[2] - focus_[2]) * (cam3_[2] - focus_[2])));
                    float k = d * 0.0016f;
                    cam3_[0] -= e.motion.xrel * k;
                    cam3_[2] -= e.motion.yrel * k;
                } else {
                    cx_ -= (e.motion.xrel) / scale_;
                    cz_ -= (e.motion.yrel) / scale_;
                }
            }
            return true;
        case SDL_MOUSEBUTTONDOWN:
            if (e.button.button == SDL_BUTTON_LEFT) {
                panning_ = false;
                dragMx_ = e.button.x;
                dragMy_ = e.button.y;
                if (view3d_) {
                    dragging_ = true;                 // orbit
                } else {
                    int i = pickNode((float)e.button.x, (float)e.button.y, 14.0f);
                    if (i >= 0) { selectNodeIndex(i); dragging_ = true; }
                }
            } else if (e.button.button == SDL_BUTTON_RIGHT ||
                       e.button.button == SDL_BUTTON_MIDDLE) {
                panning_ = true;
                dragging_ = false;
            }
            return true;
        case SDL_MOUSEBUTTONUP:
            dragging_ = false;
            panning_ = false;
            return true;
        case SDL_MOUSEWHEEL: {
            float f = e.wheel.y > 0 ? 1.15f : 0.87f;
            if (view3d_) {                            // move the eye toward the focus
                for (int i = 0; i < 3; ++i)
                    cam3_[i] = focus_[i] + (cam3_[i] - focus_[i]) / f;
            } else {
                scale_ = std::max(1e-4f, std::min(20.0f, scale_ * f));
            }
            return true;
        }
        case SDL_KEYDOWN:
            switch (e.key.keysym.sym) {
                case SDLK_TAB: cycleNode(e.key.keysym.mod & KMOD_SHIFT ? -1 : 1); return true;
                case SDLK_v: view3d_ = !view3d_; return true;
                case SDLK_f: fitView(w, h); return true;
                case SDLK_LEFT:  nudgeNode(selNode_, -1, 0, 0); return true;
                case SDLK_RIGHT: nudgeNode(selNode_, 1, 0, 0); return true;
                case SDLK_UP:    nudgeNode(selNode_, 0, 0, -1); return true;
                case SDLK_DOWN:  nudgeNode(selNode_, 0, 0, 1); return true;
                case SDLK_PAGEUP:   nudgeNode(selNode_, 0, 1, 0); return true;
                case SDLK_PAGEDOWN: nudgeNode(selNode_, 0, -1, 0); return true;
                case SDLK_DELETE: {
                    const Way::Node* n = node(selNode_);
                    if (n) deleteNode(n->name);
                    return true;
                }
                default: break;
            }
            return false;
        default: return false;
    }
}

// ---------------------------------------------------------------------------
// drawing
// ---------------------------------------------------------------------------
void MapEditor::draw(Renderer& r, Font& f, int w, int h) {
    viewW_ = w; viewH_ = h;
    halfW_ = w * 0.5f;
    halfH_ = h * 0.5f;
    const unsigned white = r.whiteTex();

    if (view3d_) {
        r.setFog(std::max(2000.0f, (float)((cam3_[2] - cz_) * 3.0f) + 2000.0f));
        r.setFogColor(0.10f, 0.12f, 0.16f);
        r.setSkyZenith(0.18f, 0.24f, 0.34f);
        r.clear();
        r.drawSky(cam3_[4]);
        // Terrain + the tile's road ribbon (Renderer::draw), then the tile's own
        // b3d geometry (Renderer::drawScene) - the same two passes the game's
        // world scene uses.
        r.draw(cam3_[0], cam3_[1], cam3_[2], cam3_[3], cam3_[4]);
        if (envLoaded_) r.drawScene(cam3_[0], cam3_[1], cam3_[2], cam3_[3], cam3_[4]);
    } else {
        r.setFogColor(0.09f, 0.10f, 0.13f);
        r.clear();
    }

    r.begin2D();
    // --- the wireframe itself -------------------------------------------
    Mat4 proj, view;
    if (view3d_) {
        float yaw = cam3_[3] * 3.14159265f / 180.0f;
        float pitch = cam3_[4] * 3.14159265f / 180.0f;
        float tx = cam3_[0] + sinf(yaw) * cosf(pitch);
        float ty = cam3_[1] + sinf(pitch);
        float tz = cam3_[2] - cosf(yaw) * cosf(pitch);
        proj = Mat4::perspective(60.0f, (float)w / (float)h, 2.0f,
                                 std::max(800.0f, r.fogDistance() * 2.0f));
        proj.m[0] = -proj.m[0];            // Renderer's Direct3D x mirror
        view = Mat4::lookAt(cam3_[0], cam3_[1], cam3_[2], tx, ty, tz);
    }

    auto toScreen = [&](const Way::Pos& p, float& sx, float& sy) -> bool {
        if (!view3d_) { sx = px(p.x); sy = pz(p.z); return true; }
        return projectPoint(proj, view, (float)p.x, (float)p.y + 0.6f, (float)p.z, w, h, sx, sy);
    };

    // road segments
    for (int si = 0; si < (int)pristine_.segs.size(); ++si) {
        const Way::Seg* s = seg(si);
        if (!s) continue;
        float cr, cg, cb;
        segColor(s->attr, cr, cg, cb);
        const bool selected = (si == selSeg_);
        float thick = view3d_ ? 2.5f : std::max(1.5f, (float)s->width * scale_);
        if (selected) { cr = 1.0f; cg = 1.0f; cb = 1.0f; thick += 2.0f; }
        for (size_t k = 0; k + 1 < s->pts.size(); ++k) {
            float x0, y0, x1, y1;
            if (!toScreen(s->pts[k], x0, y0)) continue;
            if (!toScreen(s->pts[k + 1], x1, y1)) continue;
            r.line2D(white, x0, y0, x1, y1, thick, cr, cg, cb, 1.0f);
        }
    }
    // segments the overlay added (they are in way_ but not in pristine_)
    for (size_t si = pristine_.segs.size(); si < way_.segs.size(); ++si) {
        const Way::Seg& s = way_.segs[si];
        float cr, cg, cb;
        segColor(s.attr, cr, cg, cb);
        for (size_t k = 0; k + 1 < s.pts.size(); ++k) {
            float x0, y0, x1, y1;
            if (!toScreen(s.pts[k], x0, y0)) continue;
            if (!toScreen(s.pts[k + 1], x1, y1)) continue;
            r.line2D(white, x0, y0, x1, y1, 3.0f, 0.2f, 1.0f, 0.4f, 1.0f);
        }
    }

    // nodes
    for (size_t i = 0; i < nodes_.size(); ++i) {
        const Way::Node* n = node((int)i);
        if (!n) continue;
        float x, y;
        Way::Pos p = n->pos;
        if (!toScreen(p, x, y)) continue;
        const bool sel = ((int)i == selNode_);
        const float s = sel ? 5.0f : 2.5f;
        const float cr = sel ? 1.0f : 0.30f, cg = sel ? 0.25f : 0.85f,
                    cb = sel ? 0.25f : 0.95f;
        r.quad2D(white, x - s, y - s, s * 2, s * 2, cr, cg, cb, 1.0f);
        if (sel) {
            r.line2D(white, x - 14, y, x + 14, y, 1.5f, 1, 1, 1, 0.9f);
            r.line2D(white, x, y - 14, x, y + 14, 1.5f, 1, 1, 1, 0.9f);
        }
    }

    // --- chrome -----------------------------------------------------------
    r.quad2D(white, 0, 0, (float)w, 34, 0.05f, 0.06f, 0.08f, 0.85f);
    char line[512];
    snprintf(line, sizeof line,
             "ht2editor  MAP  tile=%s  %s  nodes=%zu  segments=%zu (live %zu)  "
             "sel=%s  seg=%d",
             tile_.c_str(), view3d_ ? "3D" : "2D", nodes_.size(), pristine_.segs.size(),
             way_.segs.size(),
             selNode_ >= 0 && node(selNode_) ? node(selNode_)->name.c_str() : "-", selSeg_);
    f.draw(r, line, 8, 8, 1.4f, 1, 1, 1, 1);

    if (const Way::Node* n = node(selNode_)) {
        snprintf(line, sizeof line,
                 "node %s  x=%.2f y=%.2f z=%.2f  flags=0x%x",
                 n->name.c_str(), n->pos.x, n->pos.y, n->pos.z, n->flags);
        f.draw(r, line, 8, 40, 1.2f, 0.85f, 0.95f, 1.0f, 1);
        float x, y;
        Way::Pos p = n->pos;
        if (toScreen(p, x, y))
            f.draw(r, n->name, x + 10, y - 14, 1.1f, 1.0f, 0.9f, 0.35f, 1);
    }
    if (const Way::Seg* s = seg(selSeg_)) {
        snprintf(line, sizeof line, "seg[%d] %s  attr=0x%x width=%.2f/%.2f pts=%zu",
                 selSeg_, s->name.c_str(), (unsigned)s->attr, s->width, s->width1,
                 s->pts.size());
        f.draw(r, line, 8, 58, 1.2f, 0.95f, 0.9f, 0.6f, 1);
    }
    if (!status_.empty()) f.draw(r, status_, 8, (float)h - 24, 1.2f, 1.0f, 0.9f, 0.4f, 1);
    const char* help =
        "drag node  right-drag pan  wheel zoom  TAB next  V 2d/3d  F fit  "
        "S save  DEL delete  arrows nudge";
    f.draw(r, help, 8, (float)h - 44, 1.1f, 0.7f, 0.75f, 0.8f, 1);
    r.end2D();
}

// ---------------------------------------------------------------------------
// text reports (the headless harness reads these)
// ---------------------------------------------------------------------------
void MapEditor::printList() const {
    LOG("editor: tile %s: %zu nodes, %zu shipped segments, %zu segments after the overlay",
        tile_.c_str(), nodes_.size(), pristine_.segs.size(), way_.segs.size());
    for (size_t i = 0; i < nodes_.size(); ++i) {
        const Way::Node* n = node((int)i);
        if (!n) continue;
        LOG("editor: node %-24s x=%.3f y=%.3f z=%.3f flags=0x%x", n->name.c_str(),
            n->pos.x, n->pos.y, n->pos.z, n->flags);
    }
    for (size_t i = 0; i < pristine_.segs.size(); ++i) {
        const Way::Seg* s = seg((int)i);
        if (!s) { LOG("editor: seg %zu <deleted by the overlay>", i); continue; }
        LOG("editor: seg %-3zu %-24s attr=0x%x w=%.2f/%.2f pts=%zu", i, s->name.c_str(),
            (unsigned)s->attr, s->width, s->width1, s->pts.size());
    }
    for (size_t i = pristine_.segs.size(); i < way_.segs.size(); ++i)
        LOG("editor: seg %-3zu %-24s attr=0x%x w=%.2f/%.2f pts=%zu  [added by the overlay]",
            i, way_.segs[i].name.c_str(), (unsigned)way_.segs[i].attr, way_.segs[i].width,
            way_.segs[i].width1, way_.segs[i].pts.size());
}

void MapEditor::printReport() const {
    LOG("editor: mode=map tile=%s nodes=%zu segs=%zu live_segs=%zu view=%s sel_node=%s sel_seg=%d",
        tile_.c_str(), nodes_.size(), pristine_.segs.size(), way_.segs.size(),
        view3d_ ? "3d" : "2d",
        selNode_ >= 0 && node(selNode_) ? node(selNode_)->name.c_str() : "-", selSeg_);
}

} // namespace ht2

// The cockpit's rear-view mirror (docs/mirror.md).
//
// Data facts this file encodes (all verified against original/, see
// docs/mirror.md):
//   * frames: COMMON/esc16.RES "m16\mirr{L,R,T}{S,M,L,H}.msk" (esc15.RES has
//     the same art as "m15\..."); the centre of every frame is the palette key
//     (alpha 0), i.e. a hole for the reflection.  The hole is *derived* here by
//     finding the largest all-transparent rectangle, so no pixel constants are
//     hard-coded;
//   * camera mount: the original looks up "<Brand>SLRSpaceL/R" by name
//     (king.exe 0x1416ab / 0x141702).  Those anchors exist for every brand but
//     are never linked to geometry, so they are pure placement nodes.  They sit
//     at the very back of the truck: ZilR SLR = (-1.102, -8.010, -0.370) in
//     b3d (x, z_long, height), i.e. 0.27 in front of the model's rear face
//     (-8.280), looking backwards gives the rear view without the truck's own
//     body in the way.  "<Brand>Mirror{L,R}Space" (ZilR: (-1.0, 1.28, 1.25)) is
//     the *visible* mirror housing, linked to Mirror1L_type1 / Mirror3R_type1;
//     it is reported in the log but not used as the camera (from the cab door
//     the truck's own cab shell would fill the view).
#include "Mirror.h"
#include "Renderer.h"
#include "../res/Res.h"
#include "../res/Mask.h"
#include "../util/Log.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace ht2 {

namespace {

// Largest all-transparent axis-aligned rectangle of an RGBA image (the mirror
// glass).  Classic largest-rectangle-in-histogram scan, O(w*h).
void largestTransparentRect(const uint8_t* rgba, int w, int h,
                            int& bx, int& by, int& bw, int& bh) {
    bx = by = bw = bh = 0;
    std::vector<int> heights((size_t)w, 0);
    std::vector<int> stack;
    stack.reserve((size_t)w + 1);
    long bestArea = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x)
            heights[(size_t)x] = (rgba[((size_t)y * w + x) * 4 + 3] == 0) ? heights[(size_t)x] + 1 : 0;
        stack.clear();
        for (int x = 0; x <= w; ++x) {
            const int cur = (x < w) ? heights[(size_t)x] : 0;
            while (!stack.empty() && heights[(size_t)stack.back()] >= cur) {
                const int hh = heights[(size_t)stack.back()];
                stack.pop_back();
                const int left = stack.empty() ? 0 : stack.back() + 1;
                const int ww = x - left;
                if ((long)hh * ww > bestArea) {
                    bestArea = (long)hh * ww;
                    bx = left; by = y - hh + 1; bw = ww; bh = hh;
                }
            }
            stack.push_back(x);
        }
    }
}

std::string brandOf(const std::string& model) {
    std::string base = model;
    while (!base.empty() && !std::isalpha((unsigned char)base.back())) base.pop_back();
    return base;
}

// "<prefix>SLRSpaceL" &c.: the exact model name wins, then the brand prefix.
const B3D::Anchor* findAnchor(const std::vector<std::pair<std::string, B3D::Anchor>>* list,
                              const std::string& model, const std::string& suffix,
                              std::string* outName) {
    if (!list) return nullptr;
    const std::string brand = brandOf(model);
    const B3D::Anchor* best = nullptr;
    std::string bestName;
    int bestRank = -1;
    for (const auto& kv : *list) {
        if (kv.first.size() <= suffix.size()) continue;
        if (kv.first.compare(kv.first.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        int rank = -1;
        if (!model.empty() && kv.first.compare(0, model.size(), model) == 0) rank = 2;
        else if (!brand.empty() && kv.first.compare(0, brand.size(), brand) == 0) rank = 1;
        else continue;
        if (rank > bestRank || (rank == bestRank && kv.first.size() > bestName.size())) {
            bestRank = rank; best = &kv.second; bestName = kv.first;
        }
    }
    if (best && outName) *outName = bestName;
    return best;
}

} // namespace

const char* Mirror::posName() const {
    switch (pos_) {
        case PosLeft: return "left";
        case PosRight: return "right";
        default: return "top";
    }
}

Mirror::Pos Mirror::nextPos() { pos_ = (Pos)((pos_ + 1) % 3); return pos_; }
Mirror::Size Mirror::nextSize() { size_ = (Size)((size_ + 1) % 4); return size_; }

bool Mirror::load(Renderer& r, const std::string& dataDir,
                  const std::vector<std::pair<std::string, B3D::Anchor>>* anchors,
                  const std::string& vehicleModel) {
    unload(r);
    if (!r.getWindowHandle()) return false;

    Res res;
    std::string prefix = "m16\\";
    if (!res.load(dataDir + "/COMMON/esc16.RES")) {
        if (!res.load(dataDir + "/COMMON/esc15.RES")) {
            if (!artLogged_) {
                artLogged_ = true;
                LOG("mirror: no COMMON/esc16.RES or esc15.RES -- mirror disabled");
            }
            return false;
        }
        prefix = "m15\\";
    }

    static const char* POS = "LRT";
    static const char* SZ = "SMLH";
    int loadedFrames = 0;
    for (int p = 0; p < 3; ++p) {
        for (int s = 0; s < 4; ++s) {
            Frame& f = frames_[p][s];
            std::string name = prefix + "mirr" + POS[p] + SZ[s] + ".msk";
            const Res::Entry* e = res.find(name);
            if (!e || e->kind != Res::Kind::Mask) continue;
            DecodedImage img;
            if (!Mask::decode(res.data(*e), e->size, img)) continue;
            f.tex = r.createTexture(img.rgba.data(), img.width, img.height);
            f.w = img.width; f.h = img.height;
            largestTransparentRect(img.rgba.data(), img.width, img.height,
                                   f.gx, f.gy, f.gw, f.gh);
            if (f.tex && f.valid()) ++loadedFrames;
        }
    }
    if (!loadedFrames) {
        if (!artLogged_) {
            artLogged_ = true;
            LOG("mirror: %smirr{L,R,T}{S,M,L,H}.msk not found in COMMON/%s -- mirror disabled",
                prefix.c_str(), prefix == "m16\\" ? "esc16.RES" : "esc15.RES");
        }
        return false;
    }

    // The camera mount: the model's own SLR anchor (see the file header), with
    // the interior "top" frame using the midpoint of the two sides.
    const B3D::Anchor* a = nullptr;
    std::string aName;
    if (pos_ == PosLeft) {
        a = findAnchor(anchors, vehicleModel, "SLRSpaceL", &aName);
    } else if (pos_ == PosRight) {
        a = findAnchor(anchors, vehicleModel, "SLRSpaceR", &aName);
    } else {
        std::string ln, rn;
        const B3D::Anchor* l = findAnchor(anchors, vehicleModel, "SLRSpaceL", &ln);
        const B3D::Anchor* rr = findAnchor(anchors, vehicleModel, "SLRSpaceR", &rn);
        if (l && rr) {
            anchorLocal_[0] = 0.5f * (l->t[0] + rr->t[0]);
            anchorLocal_[1] = 0.5f * (l->t[2] + rr->t[2]);
            anchorLocal_[2] = 0.5f * (l->t[1] + rr->t[1]);
            hasAnchor_ = true;
            aName = ln + " + " + rn;
        } else if (l || rr) {
            a = l ? l : rr;
            aName = l ? ln : rn;
        }
    }
    if (a) {
        // B3D anchors are (x, z_long, height); the vehicle-local eye frame is
        // (x right, y up, z forward) - App::viewPose() does the same swap.
        anchorLocal_[0] = a->t[0];
        anchorLocal_[1] = a->t[2];
        anchorLocal_[2] = a->t[1];
        hasAnchor_ = true;
    }
    anchorName_ = aName;
    if (hasAnchor_) {
        // The housing anchor is the visible mirror; log it once for the record.
        std::string mn;
        const B3D::Anchor* m = findAnchor(anchors, vehicleModel, "MirrorLSpace", &mn);
        LOG("mirror: %d frames from %s, mount '%s' (%.2f %.2f %.2f)%s", loadedFrames,
            prefix == "m16\\" ? "esc16.RES" : "esc15.RES", anchorName_.c_str(),
            anchorLocal_[0], anchorLocal_[1], anchorLocal_[2],
            m ? "" : " [no SLR anchor: using the fallback behind the truck]");
    } else {
        LOG("mirror: %d frames from %s, no <model>SLRSpace{L,R} anchor for '%s' -- "
            "camera falls back to (%.2f %.2f %.2f) vehicle-local",
            loadedFrames, prefix == "m16\\" ? "esc16.RES" : "esc15.RES", vehicleModel.c_str(),
            anchorLocal_[0], anchorLocal_[1], anchorLocal_[2]);
    }

    loaded_ = true;
    layout(r);
    return true;
}

void Mirror::unload(Renderer& r) {
    if (r.getWindowHandle()) {
        for (int p = 0; p < 3; ++p)
            for (int s = 0; s < 4; ++s) {
                if (frames_[p][s].tex) r.deleteTexture(frames_[p][s].tex);
                frames_[p][s] = Frame();
            }
        r.destroyRenderTarget(target_);
    } else {
        for (int p = 0; p < 3; ++p) for (int s = 0; s < 4; ++s) frames_[p][s] = Frame();
        target_ = Renderer::RenderTarget();
    }
    loaded_ = false;
    hasAnchor_ = false;
    anchorName_.clear();
    lastCostMs_ = 0.0f;
}

// Screen layout.  The original writes the destination of the 480x170 H frame as
// (0x6f6a3c=1117, 0x6f6a40=50) in the same init that loads the masks, i.e.
// flush with the right edge of a 1600-wide screen and 50 px down - just below
// the 1600x50 HUD strip.  The frame is therefore anchored to the top right here
// too (the left variants mirror that to the left edge), with the original's
// 50-px vertical offset scaled by the window *width* (the HUD strip is scaled
// that way as well, so the mirror stays clear of it).  Everything else scales
// with the window height against the 720-px reference the art was measured at.
void Mirror::layout(const Renderer& r) {
    const Frame& f = frame();
    if (!f.valid()) { fx_ = fy_ = fw_ = fh_ = 0; gx_ = gy_ = gw_ = gh_ = 0; return; }
    const float s = std::max(0.5f, std::min(2.0f, (float)r.height_ / 720.0f));
    fw_ = std::max(8, (int)(f.w * s + 0.5f));
    fh_ = std::max(8, (int)(f.h * s + 0.5f));
    const int margin = std::max(4, (int)(8.0f * s + 0.5f));
    fy_ = std::max(margin, (int)(50.0f * (float)r.width_ / 1600.0f + 0.5f));
    if (pos_ == PosLeft) fx_ = margin;
    else if (pos_ == PosRight || pos_ == PosTop) fx_ = r.width_ - fw_ - margin;
    else fx_ = margin;
    // The glass hole, scaled with the frame.
    gx_ = fx_ + (int)(f.gx * (float)fw_ / (float)f.w + 0.5f);
    gy_ = fy_ + (int)(f.gy * (float)fh_ / (float)f.h + 0.5f);
    gw_ = (int)(f.gw * (float)fw_ / (float)f.w + 0.5f);
    gh_ = (int)(f.gh * (float)fh_ / (float)f.h + 0.5f);
    if (gw_ < 1) gw_ = 1;
    if (gh_ < 1) gh_ = 1;
}

bool Mirror::ensureTarget(Renderer& r, int w, int h) {
    if (target_.live() && target_.w == w && target_.h == h) return true;
    if (!r.createRenderTarget(target_, w, h)) {
        if (!rtLogged_) {
            rtLogged_ = true;
            LOG("mirror: render target %dx%d unavailable -- mirror disabled", w, h);
        }
        return false;
    }
    return true;
}

void Mirror::camera(float& x, float& y, float& z, float& yawDeg, float& pitchDeg) const {
    x = cam_[0]; y = cam_[1]; z = cam_[2]; yawDeg = cam_[3]; pitchDeg = cam_[4];
}

void Mirror::frameRect(int& x, int& y, int& w, int& h) const {
    x = fx_; y = fy_; w = fw_; h = fh_;
}

void Mirror::glassRect(int& x, int& y, int& w, int& h) const {
    x = gx_; y = gy_; w = gw_; h = gh_;
}

void Mirror::draw(Renderer& r, const Mat4& vehicleModel) {
    lastCostMs_ = 0.0f;
    if (!active()) return;
    if (r.renderTargetBound()) return;      // never nest a mirror inside a target
    layout(r);
    const Frame& f = frame();
    if (!f.valid() || gw_ < 2 || gh_ < 2) return;

    // --- reflection camera: the mirror mount, looking backwards ---
    const float* m = vehicleModel.m;
    const float lx = anchorLocal_[0], ly = anchorLocal_[1], lz = anchorLocal_[2];
    cam_[0] = m[0] * lx + m[4] * ly + m[8]  * lz + m[12];
    cam_[1] = m[1] * lx + m[5] * ly + m[9]  * lz + m[13];
    cam_[2] = m[2] * lx + m[6] * ly + m[10] * lz + m[14];
    // The vehicle's forward axis is the model matrix' third column; a rear-view
    // mirror looks along -forward.
    const float fwdX = m[8], fwdZ = m[10];
    cam_[3] = atan2f(-fwdX, fwdZ) * 180.0f / 3.14159265f;   // yaw + 180
    cam_[4] = -5.0f;                                         // slight downward tilt

    // --- pass 1: the world from the mirror camera, into the render target ---
    const int rtW = std::max(16, (int)(gw_ * scale_ + 0.5f));
    const int rtH = std::max(16, (int)(gh_ * scale_ + 0.5f));
    if (!ensureTarget(r, rtW, rtH)) return;

    const auto t0 = std::chrono::steady_clock::now();
    const float prevNear = r.nearPlaneValue();
    const Mat4 prevModel = r.modelMatrix();
    if (r.bindRenderTarget(target_, true)) {
        r.setNearPlane(0.4f);
        r.clearModel();
        r.draw(cam_[0], cam_[1], cam_[2], cam_[3], cam_[4]);
        r.drawScene(cam_[0], cam_[1], cam_[2], cam_[3], cam_[4]);
        if (extraWorldPass) extraWorldPass(cam_[0], cam_[1], cam_[2], cam_[3], cam_[4]);
        r.setNearPlane(prevNear);
        r.setModel(prevModel);
        r.unbindRenderTarget();
    } else {
        return;
    }
    lastCostMs_ = std::chrono::duration<float, std::milli>(
                      std::chrono::steady_clock::now() - t0).count();

    // --- pass 2: the reflection (U flipped: a mirror swaps left and right)
    //     plus the original frame art over it ---
    const bool was2D = r.overlayActive();
    if (!was2D) r.begin2D();
    // U runs 1 -> 0: a real mirror swaps left and right, and the reflection was
    // rendered with the same Direct3D->OpenGL orientation the main view uses, so
    // this one swap is the whole mirror effect.  V runs 1 -> 0 as well because a
    // GL texture's row 0 is the *bottom* of the FBO - only the horizontal axis
    // may be swapped, a mirror never flips upside down.
    const float u0 = flipU_ ? 1.0f : 0.0f, u1 = flipU_ ? 0.0f : 1.0f;
    r.quad2D(target_.color, (float)gx_, (float)gy_, (float)gw_, (float)gh_,
             1.0f, 1.0f, 1.0f, 1.0f, u0, 1.0f, u1, 0.0f);
    r.quad2D(f.tex, (float)fx_, (float)fy_, (float)fw_, (float)fh_, 1.0f, 1.0f, 1.0f, 1.0f);
    if (!was2D) r.end2D();
    else r.flush2D();
}

} // namespace ht2

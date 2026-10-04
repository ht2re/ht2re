#pragma once
// The rear-view mirror of the cockpit view (docs/mirror.md).
//
// The original (king.exe) renders the mirror as *2D art over a re-rendered 3D
// scene*: it loads the frames `m16\mirr{L,R,T}{S,M,L,H}.msk` from
// COMMON/esc16.RES (init at king.exe file offset 0x197900) and, separately,
// looks up the truck's mirror-surface anchors `%sSLRSpaceL/R`,
// `%sBLRSpaceL/R`, `%sHLRSpaceL/R` by name (0x1416ab/0x141702) - those records
// exist in COMMON/TRUCKS.B3D for every brand but are referenced by NO link
// record, i.e. they carry no geometry and only give the reflection camera its
// mount point (docs/mirror.md "What the shipped data has").
//
// This class keeps that split:
//   * Renderer::createRenderTarget() gives a small FBO; the world is re-drawn
//     into it from a virtual camera at the model's mirror anchor looking
//     backwards (the same Renderer::draw()/drawScene() path the cockpit uses);
//   * the reflection is blitted into the frame's transparent "glass" rect with
//     its U axis flipped (a real mirror swaps left and right);
//   * the decoded mirr*.msk frame is drawn over it with alpha blending.
//
// Everything degrades gracefully: no art, no mirror anchors or no render-target
// support means loaded() == false / draw() does nothing, and a hidden mirror
// costs one branch per frame.
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include "Math3D.h"
#include "Renderer.h"
#include "../world/B3D.h"

namespace ht2 {

class Mirror {
public:
    // Which of the three original frames is shown: the left/right door mirror
    // (portrait, 169x419 for H) or the interior "top" mirror (landscape,
    // 480x170 for H).
    enum Pos { PosLeft = 0, PosRight = 1, PosTop = 2 };
    // The original's four frame sizes (mirr*S/M/L/H).
    enum Size { SizeS = 0, SizeM = 1, SizeL = 2, SizeH = 3 };

    // One decoded frame: the mask texture plus its transparent inner rect.
    struct Frame {
        unsigned tex = 0;
        int w = 0, h = 0;
        int gx = 0, gy = 0, gw = 0, gh = 0;   // "glass" (transparent) rect
        bool valid() const { return tex != 0 && gw > 0 && gh > 0; }
    };

    // Loads all 12 frames from <dataDir>/COMMON/esc16.RES (falls back to
    // esc15.RES), finds the mirror anchors in `anchors` (B3D::anchorList of
    // COMMON/TRUCKS.B3D, may be null) for `vehicleModel` ("ZilR" -> "Zil") and
    // creates the render target.  Returns false and logs once when the art is
    // absent; the object then stays inert.
    bool load(Renderer& r, const std::string& dataDir,
              const std::vector<std::pair<std::string, B3D::Anchor>>* anchors,
              const std::string& vehicleModel);
    void unload(Renderer& r);

    bool loaded() const { return loaded_; }
    // The parent sets this: the mirror only exists in the cockpit view.
    void setVisible(bool v) { visible_ = v; }
    bool visible() const { return visible_; }
    bool active() const { return loaded_ && visible_; }

    void setPos(Pos p) { pos_ = p; }
    void setSize(Size s) { size_ = s; }
    Pos pos() const { return pos_; }
    Size size() const { return size_; }
    // Cycle helpers (key bindings / tests): returns the new value.
    Pos nextPos();
    Size nextSize();

    // Frame texture actually used (0 when not loaded).
    unsigned frameTexture() const { return frame().tex; }
    const Frame& frame() const { return frames_[pos_][size_]; }
    // Mirror anchor found in the model ("ZilRSLRSpaceL" or "").
    const std::string& anchorName() const { return anchorName_; }
    bool hasAnchor() const { return hasAnchor_; }
    // Reflection camera used for the last draw (world space, GL frame).
    void camera(float& x, float& y, float& z, float& yawDeg, float& pitchDeg) const;
    // Screen rect of the last drawn frame + its glass rect (diagnostics/tests).
    void frameRect(int& x, int& y, int& w, int& h) const;
    void glassRect(int& x, int& y, int& w, int& h) const;
    int targetWidth() const { return target_.w; }
    int targetHeight() const { return target_.h; }
    // Reflection resolution scale (0.5 = half the on-screen glass size).
    void setResolutionScale(float s) { scale_ = s < 0.1f ? 0.1f : (s > 1.0f ? 1.0f : s); }
    float resolutionScale() const { return scale_; }
    // A real mirror swaps left and right: the reflection is blitted with its U
    // axis flipped (default).  Only the test harness turns this off, to show the
    // flip is what it says on the tin.
    void setFlipU(bool f) { flipU_ = f; }
    bool flipU() const { return flipU_; }
    // Extra 3D passes for the reflection (App's traffic): called as
    // pass(camX, camY, camZ, camYawDeg, camPitchDeg) while the target is bound.
    std::function<void(float, float, float, float, float)> extraWorldPass;
    // Cost bookkeeping: ms of the last reflection pass (0 when not drawn).
    float lastCostMs() const { return lastCostMs_; }

    // The whole mirror for one frame: renders the reflection into the render
    // target and blits it + the frame mask with its own 2D pass.  Call once per
    // frame in the cockpit view, after the world/vehicle passes and before the
    // HUD's begin2D().  `vehicleModel` is Vehicle::modelMatrix(): its third
    // column is the vehicle's forward axis, which is all the mirror camera needs
    // (the view looks the other way).  No-op when !active().
    void draw(Renderer& r, const Mat4& vehicleModel);

private:
    Frame frames_[3][4];
    Pos pos_ = PosTop;
    // SizeM (226x76 at 720p) is the shipping default: the interior mirror is
    // clearly visible but leaves the cockpit view's sky budget alone (SizeH's
    // 480x170 frame costs ~46k sky pixels and trips shot_test's "cockpit
    // windscreen shows the sky" check; setSize(SizeH) is available).
    Size size_ = SizeM;
    bool loaded_ = false, visible_ = true, hasAnchor_ = false, artLogged_ = false, rtLogged_ = false;
    std::string anchorName_;
    float anchorLocal_[3] = {0.0f, 2.2f, -9.0f};   // (x, height, z_long)
    Renderer::RenderTarget target_;
    float scale_ = 0.5f;
    bool flipU_ = true;
    // last frame geometry
    float cam_[5] = {0, 0, 0, 0, -5.0f};
    int fx_ = 0, fy_ = 0, fw_ = 0, fh_ = 0;      // frame rect
    int gx_ = 0, gy_ = 0, gw_ = 0, gh_ = 0;      // glass rect (screen px)
    float lastCostMs_ = 0.0f;

    // Screen layout of the current pos/size for the window the renderer has.
    void layout(const Renderer& r);
    bool ensureTarget(Renderer& r, int w, int h);
    const char* posName() const;
};

} // namespace ht2

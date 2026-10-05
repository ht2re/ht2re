#pragma once
// Map editor: ENV/<tile>.way road network, editable, saved to the overlay.
// Part of ht2editor (tools/editor/, docs/editor.md).
#include <SDL.h>
#include <string>
#include <vector>
#include "world/Way.h"
#include "world/B3D.h"
#include "world/Terrain.h"
#include "world/GroundMap.h"
#include "world/SceneTextures.h"
#include "world/Overlay.h"

namespace ht2 {

class Renderer;
class Font;

class MapEditor {
public:
    // Loads ENV/<tile>.way and the overlay; ENV/<tile>.b3d is loaded lazily for
    // the 3D view (textured context around the network).
    bool load(Renderer& r, const std::string& dataDir, const std::string& tile,
              Overlay& ov);
    // 3D context: the tile's own geometry plus the ground the game derives from
    // it (Terrain + GroundMap).  Must run BEFORE Renderer::init(), which builds
    // those meshes once from the pointers registered here.
    bool prepare3D(Renderer& r);
    bool texture3D(Renderer& r);       // decode ENV/<tile>.res (needs GL)

    const std::string& tile() const { return tile_; }
    size_t nodeCount() const { return nodes_.size(); }
    size_t segCount() const { return pristine_.segs.size(); }
    size_t liveSegCount() const { return way_.segs.size(); }
    int selectedNode() const { return selNode_; }
    int selectedSeg() const { return selSeg_; }

    void setView3D(bool on) { view3d_ = on; }
    bool view3D() const { return view3d_; }
    void setCamera(const float cam[5]);   // x,y,z,yaw,pitch (3D view)
    void fitView(int w, int h);

    // --- selection ---------------------------------------------------------
    bool selectNode(const std::string& name);
    void selectNodeIndex(int i);
    void selectSegIndex(int shippedIndex);
    void cycleNode(int delta);
    const Way::Node* node(int i) const;
    const Way::Seg* seg(int shippedIndex) const;

    // --- editing (each writes straight into the overlay) -------------------
    bool moveNodeAbs(int i, double x, double y, double z);
    bool nudgeNode(int i, double dx, double dy, double dz);
    bool setNodeFlags(int i, uint32_t flags);
    bool addNode(const std::string& name, double x, double y, double z,
                 const std::string& room);
    bool deleteNode(const std::string& name);
    bool connectNodes(const std::string& a, const std::string& b, double width);
    bool setSegFields(int shippedIndex, int attr, double width, double width1);
    bool deleteSeg(int shippedIndex);

    // --- presentation ------------------------------------------------------
    void draw(Renderer& r, Font& f, int w, int h);
    bool event(Renderer& r, const SDL_Event& e, int w, int h);
    void printList() const;
    void printReport() const;

private:
    struct NRef { int room = -1, node = -1; };
    void rebuildWorking();            // way_ = pristine_ + overlay
    void rebuildNodeIndex();
    int effSegIndex(int shippedIndex) const;
    void bounds(double& x0, double& z0, double& x1, double& z1) const;

    float px(double x) const { return halfW_ + (float)((x - cx_) * scale_); }
    float pz(double z) const { return halfH_ + (float)((z - cz_) * scale_); }
    double wx(float sx) const { return cx_ + (sx - halfW_) / scale_; }
    double wz(float sy) const { return cz_ + (sy - halfH_) / scale_; }
    int pickNode(float mx, float my, float radius) const;

    Way pristine_;                    // exactly what the shipped .way contains
    Way way_;                         // pristine_ + overlay (what is drawn)
    std::vector<NRef> nodes_;         // flat editable node refs into way_
    std::string tile_, dataDir_;
    Overlay* ov_ = nullptr;

    B3D env_;
    SceneTextures tex_;
    Terrain terrain_;
    GroundMap ground_;
    bool envLoaded_ = false, texLoaded_ = false, groundBuilt_ = false;

    int selNode_ = -1, selSeg_ = -1;
    std::string selNodeName_;
    bool view3d_ = false;
    float cam3_[5] = {0, 500, 0, 0, -55};
    float focus_[3] = {0, 0, 0};      // orbit/zoom centre of the 3D view
    // 2D view state
    double cx_ = 0, cz_ = 0;
    float scale_ = 0.05f;
    float halfW_ = 640, halfH_ = 360;
    int viewW_ = 1280, viewH_ = 720;
    bool dragging_ = false;
    bool panning_ = false;
    int dragMx_ = 0, dragMy_ = 0;
    float mouseX_ = -1, mouseY_ = -1;
    std::string status_;
};

} // namespace ht2

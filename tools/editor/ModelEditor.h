#pragma once
// Car-model editor: a vehicle container of COMMON/TRUCKS.B3D, its parts
// (submeshes), materials and anchors, edited into the overlay.
// Part of ht2editor (tools/editor/, docs/editor.md).
#include <SDL.h>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "world/B3D.h"
#include "world/SceneTextures.h"
#include "world/Overlay.h"

namespace ht2 {

class Renderer;
class Font;

class ModelEditor {
public:
    // One "part" = every submesh sharing a node name (the b3d container is
    // flattened per face x material, so a wheel is several submeshes).
    struct PartInfo {
        std::string name;
        std::vector<int> sub;                 // indices into pristine_
        std::vector<std::string> materials;
        int tris = 0;
        float bb[6] = {0, 0, 0, 0, 0, 0};     // b3d space (x, z, height)
        bool hidden = false;
    };

    bool load(Renderer& r, const std::string& dataDir, const std::string& model,
              Overlay& ov);
    // Re-runs buildNamedScene (after anchor edits) and re-derives subs_.
    void rebuild(Renderer& r, bool anchors = false);

    const std::string& model() const { return model_; }
    size_t partCount() const { return parts_.size(); }
    // Only the anchors this model's links actually placed (B3D::usedAnchors);
    // the container holds every other vehicle's anchors too.
    size_t anchorCount() const { return anchorView_.size(); }
    size_t containerAnchorCount() const { return b3d_.anchorList.size(); }
    size_t submeshCount() const { return pristine_.size(); }
    size_t triangleCount() const;

    // --- selection ---------------------------------------------------------
    bool selectPart(const std::string& name);
    void selectPartIndex(int i);
    void cyclePart(int d);
    bool selectAnchor(const std::string& name);
    void cycleAnchor(int d);
    const PartInfo* part(int i) const;
    const std::pair<std::string, B3D::Anchor>* anchor(int i) const;
    int selectedPart() const { return selPart_; }
    int selectedAnchor() const { return selAnchor_; }

    // --- editing -----------------------------------------------------------
    // The anchor edits re-assemble the named scene, so they need the renderer to
    // re-upload the batches (the editor calls them from its own event/script
    // handling, where the renderer is at hand).
    bool setPartHidden(const std::string& name, bool hidden);
    bool setPartMaterial(const std::string& name, const std::string& material);
    bool moveAnchor(Renderer& r, const std::string& name, double dx, double dy, double dz);
    bool rotateAnchor(Renderer& r, const std::string& name, double dyaw, double dpitch,
                      double droll);
    bool setAnchorPos(Renderer& r, const std::string& name, double x, double y, double z);

    // --- view --------------------------------------------------------------
    void setAngles(float yaw, float pitch);
    void setDistance(float d);
    void fitCamera(int w, int h);
    void draw(Renderer& r, Font& f, int w, int h);
    bool event(Renderer& r, const SDL_Event& e, int w, int h);
    void printList() const;
    void printReport() const;

private:
    void derive();       // pristine_ + hidden_/mats_ -> subs_ and parts_
    void buildAnchorView();   // B3D::usedAnchors -> the editor's anchor list

    B3D b3d_;
    std::vector<B3D::SubMesh> pristine_;   // buildNamedScene() output, untouched
    std::vector<B3D::SubMesh> subs_;       // what is uploaded
    std::vector<PartInfo> parts_;
    std::vector<int> anchorView_;   // indices into b3d_.anchorList
    std::map<std::string, bool> hidden_;
    std::map<std::string, std::string> mats_;
    SceneTextures tex_;
    std::string model_, dataDir_;
    Overlay* ov_ = nullptr;
    bool dirty_ = true, textured_ = false;

    int selPart_ = -1, selAnchor_ = -1;
    float yaw_ = 205.0f, pitch_ = -18.0f;
    float dist_ = 30.0f;
    float centre_[3] = {0, 0, 0};
    float ext_ = 10.0f;
    int viewW_ = 1280, viewH_ = 720;
    bool dragging_ = false;
    float mouseX_ = -1, mouseY_ = -1;
    std::string status_;
};

} // namespace ht2

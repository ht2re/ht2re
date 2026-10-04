#pragma once
// 3D cockpit instruments (docs/cockpit.md).
//
// The original has no 2D dashboard: the instrument cluster is part of the
// cockpit model.  The retail data keeps it in COMMON/CABINES.B3D, not in the
// truck model: the container "<Brand>Cab" (ZilCab, KamazCab, ...) holds the
// dashboard panels plus one needle mesh per gauge, and the matching
// "<Brand>SpeedSpace" / "<Brand>TachoSpace" / "<Brand>FuelArrowSpace" anchors
// (all children of the identity "<Brand>CabSpace" anchor) give each needle's
// pivot, its rotation plane and its zero pose.
//
// This class extracts that cluster (the three dash panels that bracket the
// needle pivots, plus the needles), uploads it once as two small dynamic
// batches and draws the needles with one model matrix per frame - so the
// truck's own batched geometry is never re-uploaded.
#include <string>
#include <vector>
#include "../gfx/Math3D.h"
#include "../world/B3D.h"
#include "../world/SceneTextures.h"

namespace ht2 {

class Renderer;
class Vehicle;

class Cockpit {
public:
    // Loads COMMON/CABINES.B3D and binds the instruments of `vehicleModel`
    // ("ZilR" -> "Zil").  Returns false (and logs once) when the file, the cab
    // container or the needle anchors are missing; every other method is then a
    // no-op, so a data-less build keeps working.
    bool load(Renderer& r, const std::string& dataDir, const std::string& vehicleModel);
    void unload(Renderer& r);

    // The parent toggles this: the gauges only animate in the cockpit view
    // (requirement 4) and never in the chase/bonnet/free-camera views.
    void setVisible(bool v) { visible_ = v; }
    bool visible() const { return visible_; }
    bool loaded() const { return loaded_; }
    const std::string& brand() const { return brand_; }
    int needleCount() const { return (int)needles_.size(); }
    // Current needle angle in degrees (diagnostics; 0 = the modelled zero pose).
    float needleAngleDeg(int i) const;
    size_t triangles() const { return panelTris_ + needleTris_; }

    // Driver's eye for the cockpit camera, in vehicle-local coordinates
    // (x right, y up, z forward - the frame Vehicle::modelMatrix() maps to the
    // world) plus the extra pitch in degrees needed to look at the cluster.
    // Returns false when no cockpit is loaded (the caller keeps its own pose).
    bool eyePose(float& x, float& y, float& z, float& pitchDeg) const;

    // Draws the static cluster and the animated needles.  `vehicleModel` is
    // Vehicle::modelMatrix(); the camera arguments match Renderer::beginTraffic.
    void draw(Renderer& r, const Mat4& vehicleModel, const Vehicle& v,
              float camX, float camY, float camZ, float camYawDeg, float camPitchDeg);

private:
    enum Kind { KSpeed = 0, KTacho = 1, KFuel = 2 };

    struct Needle {
        std::string anchor;      // B3D anchor name, e.g. "ZilSpeedSpace"
        B3D::Anchor pose;        // pivot + zero rotation (cab space)
        int batch = -1;          // Renderer traffic-model index for this needle
        int kind = KSpeed;
        float sweep = 250.0f;    // degrees of needle travel over `maxValue`
        float maxValue = 120.0f; // full-scale value in the gauge's own unit
        float angle = 0.0f;      // last drawn angle (degrees)
    };

    float gaugeValue(const Needle& n, const Vehicle& v) const;

    B3D cab_;                    // COMMON/CABINES.B3D
    SceneTextures tex_;          // CABINES.RES / 1 / 2
    std::vector<Needle> needles_;
    std::string brand_;
    int panelBatch_ = -1;
    size_t panelTris_ = 0, needleTris_ = 0;
    bool loaded_ = false, visible_ = false;
    Mat4 place_;                 // cab -> vehicle (GL frame)
    float eye_[3] = {0.0f, 0.0f, 0.0f};   // vehicle-local eye position
    float eyePitch_ = 0.0f;      // extra downward pitch (degrees)
};

} // namespace ht2

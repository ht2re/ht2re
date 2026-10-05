// Cockpit: the 3D instrument cluster of COMMON/CABINES.B3D (docs/cockpit.md).
//
// What the shipped data actually contains (verified with tools/b3d_full.py and
// a small probe over the parsed tree - dumps in docs/cockpit.md):
//
//   * COMMON/TRUCKS.B3D has NO instrument nodes.  It only carries the exterior,
//     the "<Brand>BoothSpace" cab mount (t = x, z_long, height) and
//     "<Brand>cgBlue/RedSpace%d" (the police beacon lamps, not gauges).
//   * COMMON/CABINES.B3D holds the cockpit: a top level "<Brand>Cab" container
//     (ZilCab, KamazCab, ...) with the dash panels, and an identity
//     "<Brand>CabSpace" parenting the anchors
//         <Brand>SpeedSpace / <Brand>TachoSpace / <Brand>FuelArrowSpace
//     Every shipped one of those anchors rotates about the model's longitudinal
//     axis, so the needle plane is the (x, height) plane: the needles face the
//     driver.  The needle meshes are plain quads with their pivot at the mesh
//     origin and their tip along the mesh-local +z.
//
// The binding is by name ("<Brand>SpeedSpace" -> the "<Brand>Speed" container);
// B3D::buildNamedScene() ignores subtag-4 self anchors, so the needles come out
// of it in mesh-local coordinates - exactly the frame the spin below needs.
//
// Rendering: the panels and every needle become their own Renderer "traffic"
// batch (Renderer::addTrafficModel), uploaded once; a frame only writes one 4x4
// model matrix per needle.  The truck's own batched geometry is never touched or
// re-uploaded.
#include "Cockpit.h"
#include "../gfx/Renderer.h"
#include "Vehicle.h"
#include "../util/Log.h"
#include <cmath>
#include <cstring>
#include <cctype>
#include <algorithm>

namespace ht2 {
namespace {

// ---------------------------------------------------------------------------
// placement / camera constants
//
// The retail release has no link record joining COMMON/CABINES.B3D's
// "<Brand>Cab" to the truck model, so the cab frame -> vehicle frame offset is
// a chosen convention (docs/cockpit.md):
//   * the dash front panels sit at cab z_long 1.70..1.75 for every brand and
//     stack h -1.60..-0.80; the truck cab's front wall is at ZilR z_long 1.74
//     and its windscreen base at h ~1.15 (assembled ZilR bbox);
//   * the panel plane is put 0.19 behind that wall and the dash top 0.45 under
//     the windscreen base -> (z_long -0.17, height +1.95).
// The eye is the seating position the cab model itself implies: 0.75 above the
// dash top and ~1.4 in front of the instrument plane, between the door panels
// (cab z -0.82..0.72) and the steering wheel (cab z 1.23, h -0.74).
constexpr float kPlaceZ = -0.17f;      // cab z_long -> vehicle z_long
constexpr float kPlaceH = 1.95f;       // cab height -> vehicle height
constexpr float kEyeCabX = 0.0f;
constexpr float kEyeCabH = -0.20f;
constexpr float kEyeCabZ = 0.30f;
constexpr float kEyePitch = -11.0f;    // look down at the cluster

// Needle travel (chosen - docs/cockpit.md).  The zero pose is the modelled one
// (the anchor), the sweep is the span of the painted scale: 0 at the lower left,
// full scale at the lower right.
constexpr int kGaugeCount = 3;         // speed, tacho, fuel arrow
constexpr float kSweepDial = 250.0f;   // speed + tacho
constexpr float kSweepFuel = 120.0f;   // the small "arrow" fuel gauge
constexpr float kSpeedMax = 120.0f;    // km/h (Vehicle tops out at 108)
constexpr float kTachoMax = 2500.0f;   // rpm (Vehicle::rpm() = 600..2500)
constexpr float kFuelMax = 100.0f;     // percent (Vehicle::fuel() = 0..1)

std::string paramsName(const std::vector<uint32_t>& p, size_t dwOff) {
    if (dwOff + 8 > p.size()) return std::string();
    char buf[33];
    for (int i = 0; i < 8; ++i) {
        uint32_t w = p[dwOff + (size_t)i];
        buf[i * 4 + 0] = (char)(w & 0xFF);
        buf[i * 4 + 1] = (char)((w >> 8) & 0xFF);
        buf[i * 4 + 2] = (char)((w >> 16) & 0xFF);
        buf[i * 4 + 3] = (char)((w >> 24) & 0xFF);
    }
    buf[32] = 0;
    size_t len = 0;
    while (len < 32 && buf[len]) ++len;
    return std::string(buf, len);
}

const B3D::Node* findNode(const B3D::Node& n, const std::string& name) {
    if (n.name == name) return &n;
    for (const auto& k : n.kids)
        if (const B3D::Node* r = findNode(k, name)) return r;
    return nullptr;
}

const B3D::Node* findNode(const B3D& b, const std::string& name) {
    for (const auto& n : b.tree())
        if (const B3D::Node* r = findNode(n, name)) return r;
    return nullptr;
}

// First mesh record inside a subtree (a needle container holds exactly one).
int firstMeshIndex(const B3D::Node& n) {
    if (n.meshIndex >= 0) return n.meshIndex;
    for (const auto& k : n.kids)
        if (int r = firstMeshIndex(k); r >= 0) return r;
    return -1;
}

// Material name of a mesh node's face record (subtag 35/8: params[5]).
std::string meshMaterial(const B3D& b, const B3D::Node& n) {
    if (n.faceIndex >= 0 && n.params.size() > 5) {
        uint32_t mi = n.params[5];
        if (mi < b.names().size()) return b.names()[mi];
    }
    for (const auto& k : n.kids)
        if (std::string m = meshMaterial(b, k); !m.empty()) return m;
    return std::string();
}

// b3d (x, z_long, height) -> GL (x, y = height, z = z_long) is the component
// swap S that Renderer::buildMeshFor() applies to every uploaded vertex.  The
// Anchor::m is row-major and column-vector (p' = M p, see B3D.cpp's xfPoint),
// so A_gl = S A S and t_gl = S t.
Mat4 glFromAnchor(const B3D::Anchor& a) {
    static const int sg[3] = {0, 2, 1};
    Mat4 m;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            m.m[c * 4 + r] = a.m[sg[r] * 3 + sg[c]];   // Mat4 is column-major
    m.m[12] = a.t[0];
    m.m[13] = a.t[2];
    m.m[14] = a.t[1];
    return m;
}

// The cluster geometry is one-sided in the data and the traffic pass may cull,
// so duplicate every triangle with the opposite winding (same vertices and
// normals, so the duplicate only ever z-fights with itself at equal depth).
B3D::SubMesh twoSided(const B3D::SubMesh& sm) {
    B3D::SubMesh out = sm;
    const size_t n = sm.idx.size() - (sm.idx.size() % 3);
    out.idx.reserve(sm.idx.size() * 2);
    for (size_t t = 0; t + 3 <= n; t += 3) {
        out.idx.push_back(sm.idx[t + 0]);
        out.idx.push_back(sm.idx[t + 2]);
        out.idx.push_back(sm.idx[t + 1]);
    }
    return out;
}

// Least-squares plane z_long = a + b*x + c*height over a submesh's vertices.  The
// dash panels are single curved strips, so this recovers the panel's z at a
// needle pivot precisely enough to tell the gauge faces apart.
void planeFit(const B3D::SubMesh& sm, float& a, float& b, float& c) {
    double n = 0, sx = 0, sh = 0, sxx = 0, shh = 0, sxh = 0, sz = 0, sxz = 0, shz = 0;
    for (size_t v = 0; v + 2 < sm.pos.size(); v += 3) {
        double x = sm.pos[v + 0], z = sm.pos[v + 1], h = sm.pos[v + 2];
        n += 1; sx += x; sh += h; sxx += x * x; shh += h * h; sxh += x * h;
        sz += z; sxz += x * z; shz += h * z;
    }
    if (n < 3) { a = n > 0 ? (float)(sz / n) : 0.0f; b = c = 0.0f; return; }
    // normal equations for [a b c]
    double m[3][3] = {{n, sx, sh}, {sx, sxx, sxh}, {sh, sxh, shh}};
    double rhs[3] = {sz, sxz, shz};
    double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
               - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
               + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (std::fabs(det) < 1e-9) { a = (float)(sz / n); b = c = 0.0f; return; }
    auto det3 = [](double q[3][3]) {
        return q[0][0] * (q[1][1] * q[2][2] - q[1][2] * q[2][1])
             - q[0][1] * (q[1][0] * q[2][2] - q[1][2] * q[2][0])
             + q[0][2] * (q[1][0] * q[2][1] - q[1][1] * q[2][0]);
    };
    double ma[3][3], mb[3][3], mc[3][3];
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) {
            ma[r][k] = (k == 0) ? rhs[r] : m[r][k];
            mb[r][k] = (k == 1) ? rhs[r] : m[r][k];
            mc[r][k] = (k == 2) ? rhs[r] : m[r][k];
        }
    a = (float)(det3(ma) / det);
    b = (float)(det3(mb) / det);
    c = (float)(det3(mc) / det);
}

std::string lower(const std::string& s) {
    std::string o = s;
    for (auto& c : o) c = (char)tolower((unsigned char)c);
    return o;
}

bool startsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

// The CABINES container is "<Brand>Cab" while the TRUCKS.B3D model is e.g.
// "ZilR", "Gazelle1C" or "BmwM5police".
std::string pickBrand(const B3D& cab, const std::string& model) {
    const std::string m = lower(model);
    static const struct { const char* model; const char* brand; } kAlias[] = {
        {"gazelle", "gazel"}, {"renault", "reno"},
        {"bmwm5police", "bmwpolice"}, {"bmwm5", "bmw"},
    };
    std::string best;
    size_t bestScore = 0;
    for (const auto& n : cab.tree()) {
        const std::string nm = lower(n.name);
        if (nm.size() < 4 || nm.compare(nm.size() - 3, 3, "cab") != 0) continue;
        const std::string brand = n.name.substr(0, n.name.size() - 3);
        const std::string lb = lower(brand);
        size_t score = 0;
        if (lb == m) score = 1000 + lb.size();
        else if (startsWith(m, lb)) score = 800 + lb.size();
        else if (startsWith(lb, m)) score = 600 + m.size();
        for (const auto& a : kAlias)
            if (m == a.model && lb == a.brand) score = std::max(score, (size_t)(900 + lb.size()));
        if (score > bestScore) { bestScore = score; best = brand; }
    }
    return best;
}

} // namespace

bool Cockpit::load(Renderer& r, const std::string& dataDir, const std::string& vehicleModel) {
    // Re-entrant: buying another truck of the same brand keeps the batches that
    // are already uploaded (the Renderer has no "delete batch" API).
    if (cab_.loaded() && loaded_) {
        const std::string again = pickBrand(cab_, vehicleModel);
        if (again == brand_ && !needles_.empty()) return true;
    }
    unload(r);
    const std::string path = dataDir + "/COMMON/CABINES.B3D";
    if (!cab_.loaded() && !cab_.load(path)) {
        LOG("cockpit: %s missing - cockpit instruments stay disabled", path.c_str());
        cab_ = B3D();
        return false;
    }
    brand_ = pickBrand(cab_, vehicleModel);
    if (brand_.empty()) {
        LOG("cockpit: no '<Brand>Cab' container in CABINES.B3D matches vehicle '%s'"
            " - cockpit instruments stay disabled", vehicleModel.c_str());
        cab_ = B3D();
        return false;
    }
    if (!cab_.buildNamedScene(brand_ + "Cab")) {
        LOG("cockpit: CABINES.B3D has no usable '%sCab' scene - instruments disabled",
            brand_.c_str());
        cab_ = B3D();
        return false;
    }

    // --- the three gauges: anchor + rotation plane ------------------------
    struct Want { Kind kind; const char* container; const char* space;
                 float sweep; float maxValue; };
    const Want wants[kGaugeCount] = {
        {KSpeed, "Speed",      "SpeedSpace",      kSweepDial, kSpeedMax},
        {KTacho, "Tacho",      "TachoSpace",      kSweepDial, kTachoMax},
        {KFuel,  "FuelArrow",  "FuelArrowSpace",  kSweepFuel, kFuelMax},
    };
    // The cab textures live in CABINES.RES / CABINES1.RES / CABINES2.RES; the
    // B3D material names ("CabZil_Rule", "CabZil_14", ...) are the texture
    // stems.  Loaded before the batches so they bind the right textures.
    if (!tex_.load(r, dataDir + "/COMMON/CABINES"))
        LOG("cockpit: no CABINES textures - the cluster would draw untextured");

    std::vector<bool> used(cab_.submeshes().size(), false);
    for (const Want& w : wants) {
        const std::string anchorName = brand_ + w.space;
        const B3D::Anchor* a = nullptr;
        for (const auto& kv : cab_.anchorList)
            if (kv.first == anchorName) { a = &kv.second; break; }
        const B3D::Node* cont = findNode(cab_, brand_ + w.container);
        if (!a || !cont) {
            if (w.kind != KFuel)      // a few cabs have no fuel arrow
                LOG("cockpit: %s '%s' missing - that gauge stays off",
                    a ? "container" : "anchor", anchorName.c_str());
            continue;
        }
        const int mi = firstMeshIndex(*cont);
        if (mi < 0 || (size_t)mi >= cab_.meshes().size()) continue;
        const float radius = cab_.meshes()[(size_t)mi].bbox[3];
        const float* mc = cab_.meshes()[(size_t)mi].bbox;       // local centre
        const std::string mat = meshMaterial(cab_, *cont);
        int found = -1;
        for (size_t i = 0; i < cab_.submeshes().size(); ++i) {
            if (used[i]) continue;
            const B3D::SubMesh& sm = cab_.submeshes()[i];
            if (std::fabs(sm.radius - radius) > 0.03f) continue;
            if (!mat.empty() && sm.material != mat) continue;
            if (std::fabs(sm.center[0] - mc[0]) > 0.05f ||
                std::fabs(sm.center[1] - mc[1]) > 0.05f ||
                std::fabs(sm.center[2] - mc[2]) > 0.05f) continue;  // not mesh-local
            found = (int)i;
            break;
        }
        if (found < 0) {
            LOG("cockpit: no mesh-local needle geometry for '%s%s' - gauge off",
                brand_.c_str(), w.container);
            continue;
        }
        // The needle must spin about the model's longitudinal axis: the anchor
        // maps the needle's plane normal (mesh-local +Y) onto column 1.
        if (std::fabs(a->m[4]) < 0.9f)
            LOG("cockpit: '%s' does not map the needle plane normal onto the"
                " longitudinal axis - the spin plane may be wrong", anchorName.c_str());
        used[(size_t)found] = true;
        std::vector<B3D::SubMesh> one{twoSided(cab_.submeshes()[(size_t)found])};
        const int batch = r.addTrafficModel(one, &tex_);
        if (batch < 0) {
            LOG("cockpit: could not upload the '%s' needle - gauge off", anchorName.c_str());
            continue;
        }
        needleTris_ += one[0].idx.size() / 3;
        Needle n;
        n.anchor = anchorName;
        n.pose = *a;
        n.kind = w.kind;
        n.sweep = w.sweep;
        n.maxValue = w.maxValue;
        n.batch = batch;
        needles_.push_back(n);
    }
    if (needles_.empty()) {
        LOG("cockpit: %sCab exposes none of the needle anchors - instruments disabled",
            brand_.c_str());
        cab_ = B3D();
        return false;
    }

    // --- the dash panels behind the gauges ---------------------------------
    // For every needle, pick the submesh that carries the dial face at the
    // pivot: the pivot must be inside its (x, height) bbox and the panel's own
    // z_long there must match the pivot's.  That selects <Brand>Cabin_3/4/5
    // (Zil) and the equivalent panels of the other cabs, and skips the rest of
    // the interior shell, which would otherwise surround the camera.
    std::vector<B3D::SubMesh> panels;
    std::vector<B3D::SubMesh> wheelParts;
    std::vector<bool> panelUsed(cab_.submeshes().size(), false);
    for (const Needle& n : needles_) {
        const float px = n.pose.t[0], pz = n.pose.t[1], ph = n.pose.t[2];
        int best = -1;
        float bestD = 0.30f;
        for (size_t i = 0; i < cab_.submeshes().size(); ++i) {
            if (used[i]) continue;
            const B3D::SubMesh& sm = cab_.submeshes()[i];
            if (sm.radius > 1.0f) continue;              // the interior shell
            float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
            for (size_t v = 0; v + 2 < sm.pos.size(); v += 3)
                for (int k = 0; k < 3; ++k) {
                    mn[k] = std::min(mn[k], sm.pos[v + (size_t)k]);
                    mx[k] = std::max(mx[k], sm.pos[v + (size_t)k]);
                }
            const float tol = 0.12f;
            if (px < mn[0] - tol || px > mx[0] + tol) continue;
            if (ph < mn[2] - tol || ph > mx[2] + tol) continue;
            float a = 0, b = 0, c = 0;
            planeFit(sm, a, b, c);
            const float d = std::fabs(a + b * px + c * ph - pz);
            if (d < bestD) { bestD = d; best = (int)i; }
        }
        if (best >= 0 && !panelUsed[(size_t)best]) {
            panelUsed[(size_t)best] = true;
            panels.push_back(twoSided(cab_.submeshes()[(size_t)best]));
        }
    }
    // Retain the authored cabin, including the steering wheel, doors and roof.
    // Glass is a separate overlay in the original viewer; its opaque texture
    // must not close the windscreen in this geometry pass.
    for (size_t i = 0; i < cab_.submeshes().size(); ++i) {
        if (used[i] || panelUsed[i]) continue;
        const auto& sm = cab_.submeshes()[i];
        if (lower(sm.name).find("glass") != std::string::npos ||
            lower(sm.material).find("glass") != std::string::npos) continue;
        B3D::SubMesh part = sm;
        if (startsWith(lower(sm.name), "rule")) {
            B3D::Anchor wheel;
            if (cab_.anchorOf(brand_ + "RuleSpace", wheel)) {
                wheelPose_ = glFromAnchor(wheel);
                wheelParts.push_back(twoSided(part));
                continue; // mesh-local animated wheel, no static duplicate
            }
        }
        panels.push_back(twoSided(part));
    }
    if (!panels.empty()) {
        panelBatch_ = r.addTrafficModel(panels, &tex_);
        for (const auto& p : panels) panelTris_ += p.idx.size() / 3;
    }

    if (!wheelParts.empty()) {
        wheelBatch_ = r.addTrafficModel(wheelParts, &tex_);
        for (const auto& part : wheelParts) wheelTris_ += part.idx.size() / 3;
        LOG("cockpit: %sRuleSpace steering wheel ready (%zu triangles)", brand_.c_str(), wheelTris_);
    }
    place_ = Mat4::translation(0.0f, kPlaceH, kPlaceZ);
    eye_[0] = kEyeCabX;
    eye_[1] = kEyeCabH + kPlaceH;
    eye_[2] = kEyeCabZ + kPlaceZ;
    eyePitch_ = kEyePitch;
    loaded_ = true;
    visible_ = false;
    LOG("cockpit: %sCab instruments ready: %d of %d needles, %zu panel submeshes,"
        " %zu tris, %zu cab textures",
        brand_.c_str(), (int)needles_.size(), kGaugeCount, panels.size(),
        triangles(), tex_.count());
    return true;
}

void Cockpit::unload(Renderer& r) {
    (void)r;                 // the Renderer owns the batches and frees them at shutdown
    needles_.clear();
    panelBatch_ = -1;
    wheelBatch_ = -1;
    wheelTris_ = 0;
    wheelAngle_ = 0;
    panelTris_ = needleTris_ = 0;
    loaded_ = false;
    visible_ = false;
}

float Cockpit::needleAngleDeg(int i) const {
    if (i < 0 || i >= (int)needles_.size()) return 0.0f;
    return needles_[(size_t)i].angle;
}

float Cockpit::gaugeValue(const Needle& n, const Vehicle& v) const {
    switch (n.kind) {
        case KSpeed: return std::fabs(v.speedKmh());
        case KTacho: return v.rpm();
        case KFuel:  return v.fuel() * 100.0f;
    }
    return 0.0f;
}

bool Cockpit::eyePose(float& x, float& y, float& z, float& pitchDeg) const {
    if (!loaded_) return false;
    x = eye_[0];
    y = eye_[1];
    z = eye_[2];
    pitchDeg = eyePitch_;
    return true;
}

Mat4 Cockpit::wheelTransform(float steering) const {
    // Rule_0 is authored in raw B3D XY, hence GL XZ. Its spin normal is local
    // GL +Y; the RuleSpace mount then supplies the steering-column tilt.
    return wheelPose_ * Mat4::rotY(steering * 450.0f);
}

void Cockpit::draw(Renderer& r, const Mat4& vehicleModel, const Vehicle& v,
                   float camX, float camY, float camZ, float camYawDeg, float camPitchDeg) {
    if (!loaded_ || !visible_) return;
    r.beginTraffic(camX, camY, camZ, camYawDeg, camPitchDeg);
    const Mat4 base = vehicleModel * place_;
    if (panelBatch_ >= 0) r.drawTrafficInstance(panelBatch_, base);
    if (wheelBatch_ >= 0) {
        // Presentation range chosen for the port, not an original steering ratio.
        wheelAngle_ = v.steering() * 450.0f;
        r.drawTrafficInstance(wheelBatch_, base * wheelTransform(v.steering()));
    }
    for (Needle& n : needles_) {
        float val = gaugeValue(n, v) / n.maxValue;
        val = std::min(1.0f, std::max(0.0f, val));
        n.angle = val * n.sweep;
        // The modelled anchor pose is the scale zero; an increasing angle sweeps
        // the tip clockwise in the dial plane seen by the driver, which is a
        // rotation about the vehicle's forward axis.
        const Mat4 spin = Mat4::rotZ(-n.angle * 3.14159265f / 180.0f);
        r.drawTrafficInstance(n.batch, base * glFromAnchor(n.pose) * spin);
    }
}

} // namespace ht2

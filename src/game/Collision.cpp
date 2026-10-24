#include "Collision.h"
#include "../util/Log.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_set>

// ---------------------------------------------------------------------------
// The blocking rule
// ---------------------------------------------------------------------------
// The renderer's world list (B3D::buildScene) contains everything that is
// drawn: roads, aprons, grass, decals - and the walls, fences, poles, trees
// and buildings we must collide with.  A SubMesh is one (face node x material)
// batch, so the classification is done in two steps:
//
//   1. whole-SubMesh filters
//        * material: drivable surfaces and decals never block.  A material is
//          ground-like when its lower-cased name contains one of GROUND_MAT,
//          unless it also contains one of SOLID_MAT (so mat_beton_stolb -
//          a concrete *pillar* - still blocks while mat_beton ground does not).
//        * a SubMesh whose height extent is below kDecalMinExtent is a painted
//          marking or decal (road lines, shadows, small posters).
//   2. per-triangle filters
//        * sentinel vertices (|x|,|z| > kAbsurdXZ or |height| > kAbsurdHeight)
//          are dropped; the shipped tiles contain vertices at h = +-1e6.
//        * a triangle whose unit normal is more than kHorizontalUp vertical
//          (|n_h| > 0.8, i.e. shallower than ~37 deg) is a surface: ground,
//          roof, bridge deck, canopy top, container lid.  Those never block -
//          this is what keeps drive-in repair bays, under-canopy parking and
//          roads under overpasses free while the walls around them block.
//   3. everything left is rasterised into the grid.
//
// Keep this in sync with tools/collision_probe.py (the Python reproduction over
// the shipped data) and docs/collision.md.
namespace ht2 {
namespace {

constexpr float kDecalMinExtent = 0.5f;    // m, SubMesh height extent
constexpr float kHorizontalUp = 0.8f;      // |n_h| above this = surface
constexpr float kAbsurdHeight = 1000.0f;   // sentinel vertices
constexpr float kAbsurdXZ = 20000.0f;
constexpr float kMinCell = 0.25f;
constexpr float kMaxCell = 32.0f;
// 1 bit/cell: the real world needs ~104 M cells at 1 m (13 MB).  Grow the cell
// size instead of allocating an unbounded grid if some mod adds huge geometry.
constexpr double kMaxGridCells = 256.0e6;

const char* const kGroundMat[] = {
    "road", "grass", "beton", "concrete", "gravel", "zebra", "spl", "ground",
    "ter_", "water", "cross", "mark", "asphalt", "sand", "dirt", "pavement",
    "sidewalk", "kerb", "curb", "trotoar", "trotuar", "stoika", "stoyanka",
    "parking", "runway", "taxiway", "apron", "swamp", "bshadow", "river",
    "tonnel", "zont", "bottom_bridge", "bridge_0",
};
const char* const kSolidMat[] = {
    "stolb", "pillar", "column", "kolonn", "wall", "fence", "confence",
    "ograjdenie", "rama", "beton_bridge", "side_beton",
};

std::string lower(const std::string& s) {
    std::string r = s;
    for (char& c : r)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return r;
}

int popcount64(uint64_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(v);
#else
    int n = 0;
    while (v) { v &= v - 1; ++n; }
    return n;
#endif
}

// mat_forest* are vertical scenery quads.  They are the world-edge barrier
// the player can drive through when they are skipped entirely.  A few of
// them also lie across a carriageway (ap.b3d obj_ap_048 west of the start),
// so only a forest triangle that does not cover road/apron/asphalt is solid.
bool isForestBillboard(const std::string& material) {
    return lower(material).find("forest") != std::string::npos;
}
bool isCarriageway(const std::string& material) {
    const std::string m = lower(material);
    return m.find("road") != std::string::npos || m.find("asphalt") != std::string::npos ||
           m.find("apron") != std::string::npos;
}
bool pointInTriXZ(const float* a, const float* b, const float* c, float x, float z) {
    const float e = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (std::fabs(e) < 1e-8f) return false;
    const float w0 = ((b[0] - x) * (c[1] - z) - (b[1] - z) * (c[0] - x)) / e;
    const float w1 = ((c[0] - x) * (a[1] - z) - (c[1] - z) * (a[0] - x)) / e;
    const float w2 = 1.0f - w0 - w1;
    const float eps = 1e-3f;
    return (w0 >= -eps && w1 >= -eps && w2 >= -eps) ||
           (w0 <= eps && w1 <= eps && w2 <= eps);
}

bool isGroundMaterial(const std::string& material) {
    if (material.empty()) return false;
    const std::string m = lower(material);
    bool ground = false;
    for (const char* p : kGroundMat)
        if (m.find(p) != std::string::npos) { ground = true; break; }
    if (!ground) return false;
    for (const char* p : kSolidMat)
        if (m.find(p) != std::string::npos) return false;   // pillar/wall wins
    return true;
}

// 2D (x, z) triangle vs axis-aligned cell rectangle, separating-axis test.
// Conservative: a triangle touching a cell marks it.
bool triRectOverlap(const float* p0, const float* p1, const float* p2,
                    float rx0, float rx1, float rz0, float rz1) {
    const float* tri[3] = {p0, p1, p2};
    const float e[3][2] = {
        {p1[0] - p0[0], p1[1] - p0[1]},
        {p2[0] - p1[0], p2[1] - p1[1]},
        {p0[0] - p2[0], p0[1] - p2[1]},
    };
    for (int a = 0; a < 3; ++a) {
        const float nx = -e[a][1], nz = e[a][0];
        float tmin = 1e30f, tmax = -1e30f;
        for (int i = 0; i < 3; ++i) {
            float v = nx * tri[i][0] + nz * tri[i][1];
            tmin = std::min(tmin, v);
            tmax = std::max(tmax, v);
        }
        const float c0 = nx * rx0 + nz * rz0, c1 = nx * rx0 + nz * rz1;
        const float c2 = nx * rx1 + nz * rz0, c3 = nx * rx1 + nz * rz1;
        const float rmin = std::min(std::min(c0, c1), std::min(c2, c3));
        const float rmax = std::max(std::max(c0, c1), std::max(c2, c3));
        if (tmax < rmin || rmax < tmin) return false;
    }
    return true;
}

float triUpComponent(const float* a, const float* b, const float* c) {
    const float e1x = b[0] - a[0], e1z = b[1] - a[1], e1h = b[2] - a[2];
    const float e2x = c[0] - a[0], e2z = c[1] - a[1], e2h = c[2] - a[2];
    // cross(e1, e2), height component last
    const float nh = e1x * e2z - e1z * e2x;
    const float nx = e1z * e2h - e1h * e2z;
    const float nz = e1h * e2x - e1x * e2h;
    const float len = std::sqrt(nx * nx + nh * nh + nz * nz);
    if (len < 1e-9f) return 1.0f;              // degenerate: treat as surface
    return std::fabs(nh) / len;
}

// x-extent of a (convex) triangle inside the z band [zlo, zhi].  Used as a
// conservative fallback for absurdly large triangles, where the exact
// per-cell test would cost millions of iterations.
bool triBandX(const float* a, const float* b, const float* c,
              float zlo, float zhi, float& xmin, float& xmax) {
    const float* v[3] = {a, b, c};
    float lo = 1e30f, hi = -1e30f;
    bool any = false;
    for (int i = 0; i < 3; ++i) {
        const float* p = v[i];
        const float* q = v[(i + 1) % 3];
        if (p[1] >= zlo && p[1] <= zhi) {
            lo = std::min(lo, p[0]);
            hi = std::max(hi, p[0]);
            any = true;
        }
        for (float zc : {zlo, zhi}) {
            const float dz = q[1] - p[1];
            if (std::fabs(dz) < 1e-9f) continue;
            const float t = (zc - p[1]) / dz;
            if (t < 0.0f || t > 1.0f) continue;
            const float x = p[0] + (q[0] - p[0]) * t;
            lo = std::min(lo, x);
            hi = std::max(hi, x);
            any = true;
        }
    }
    xmin = lo;
    xmax = hi;
    return any;
}

bool absurd(const float* v) {
    return std::fabs(v[0]) > kAbsurdXZ || std::fabs(v[1]) > kAbsurdXZ ||
           std::fabs(v[2]) > kAbsurdHeight;
}

float submeshExtent(const B3D::SubMesh& sm) {
    if (sm.pos.size() < 3) return 0.0f;
    float lo = sm.pos[2], hi = sm.pos[2];
    for (size_t i = 5; i < sm.pos.size(); i += 3) {
        lo = std::min(lo, sm.pos[i]);
        hi = std::max(hi, sm.pos[i]);
    }
    return hi - lo;
}

bool skippedGroup(const B3D::SubMesh& sm, const std::set<std::string>* skipGroups) {
    return skipGroups && !sm.group.empty() && skipGroups->count(sm.group) != 0;
}

} // namespace

float Collision::defaultCellSize() { return 1.0f; }

void Collision::clear() {
    bits_.clear();
    walls_.clear(); wallBuckets_.clear(); wideWalls_.clear();
    nx_ = nz_ = 0;
    x0_ = z0_ = 0.0f;
    solidCells_ = 0;
    stats_ = Stats();
}

bool Collision::solidCell(int ix, int iz) const { return cellSolid(ix, iz); }

void Collision::build(const std::vector<B3D::SubMesh>& meshes) {
    build(meshes, defaultCellSize());
}

void Collision::build(const std::vector<B3D::SubMesh>& meshes, float cellSize,
                      const std::set<std::string>* skipGroups) {
    clear();
    cell_ = (cellSize >= kMinCell && cellSize <= kMaxCell) ? cellSize
                                                           : defaultCellSize();
    stats_.submeshes = meshes.size();

    // --- pass 1: bounds of everything that can block ------------------------
    float mnx = 1e30f, mnz = 1e30f, mxx = -1e30f, mxz = -1e30f;
    for (const B3D::SubMesh& sm : meshes) {
        if (skippedGroup(sm, skipGroups)) continue;
        if (isGroundMaterial(sm.material)) { ++stats_.ground; continue; }
        if (submeshExtent(sm) < kDecalMinExtent) { ++stats_.flat; continue; }
        for (size_t i = 0; i + 2 < sm.pos.size(); i += 3) {
            const float* v = &sm.pos[i];
            if (absurd(v)) continue;
            mnx = std::min(mnx, v[0]);
            mxx = std::max(mxx, v[0]);
            mnz = std::min(mnz, v[1]);
            mxz = std::max(mxz, v[1]);
        }
    }
    if (mxz < mnz || mxx < mnx) {
        LOG("collision: no solid submeshes (%zu of %zu ground, %zu flat)",
            stats_.ground, stats_.flat, stats_.submeshes);
        return;
    }

    const float margin = 2.0f * cell_;
    x0_ = std::floor((mnx - margin) / cell_) * cell_;
    z0_ = std::floor((mnz - margin) / cell_) * cell_;
    nx_ = (int)std::ceil((mxx + margin - x0_) / cell_) + 1;
    nz_ = (int)std::ceil((mxz + margin - z0_) / cell_) + 1;
    while ((double)nx_ * (double)nz_ > kMaxGridCells) {   // pathological mods
        cell_ *= 2.0f;
        x0_ = std::floor((mnx - 2.0f * cell_) / cell_) * cell_;
        z0_ = std::floor((mnz - 2.0f * cell_) / cell_) * cell_;
        nx_ = (int)std::ceil((mxx + 2.0f * cell_ - x0_) / cell_) + 1;
        nz_ = (int)std::ceil((mxz + 2.0f * cell_ - z0_) / cell_) + 1;
    }
    if (nx_ <= 0 || nz_ <= 0) return;
    stats_.gridCells = (size_t)nx_ * (size_t)nz_;
    bits_.assign((stats_.gridCells + 63) / 64, 0ull);

    // Carriageway cells (2 m).  A forest billboard is solid unless one of
    // these centres lies inside it, which is the apron-crossing quad and not
    // the map-edge barrier.
    std::unordered_set<uint64_t> roadCells;
    auto roadKey = [](float x, float z) -> uint64_t {
        const int cx = (int)std::floor(x / 2.0f);
        const int cz = (int)std::floor(z / 2.0f);
        return (uint64_t)(uint32_t)cx << 32 | (uint32_t)cz;
    };
    for (const B3D::SubMesh& sm : meshes) {
        if (!isCarriageway(sm.material)) continue;
        for (size_t t = 0; t + 2 < sm.idx.size(); t += 3) {
            const uint32_t ia = sm.idx[t], ib = sm.idx[t + 1], ic = sm.idx[t + 2];
            if ((size_t)ia * 3 + 2 >= sm.pos.size() || (size_t)ib * 3 + 2 >= sm.pos.size() ||
                (size_t)ic * 3 + 2 >= sm.pos.size())
                continue;
            const float* a = &sm.pos[(size_t)ia * 3];
            const float* b = &sm.pos[(size_t)ib * 3];
            const float* c = &sm.pos[(size_t)ic * 3];
            if (absurd(a) || absurd(b) || absurd(c)) continue;
            const float tx0 = std::min(a[0], std::min(b[0], c[0]));
            const float tx1 = std::max(a[0], std::max(b[0], c[0]));
            const float tz0 = std::min(a[1], std::min(b[1], c[1]));
            const float tz1 = std::max(a[1], std::max(b[1], c[1]));
            for (float z = std::floor(tz0 / 2.0f) * 2.0f; z <= tz1; z += 2.0f)
                for (float x = std::floor(tx0 / 2.0f) * 2.0f; x <= tx1; x += 2.0f)
                    if (pointInTriXZ(a, b, c, x + 1.0f, z + 1.0f))
                        roadCells.insert(roadKey(x + 1.0f, z + 1.0f));
        }
    }

    // --- pass 2: rasterise the wall-like triangles --------------------------
    for (size_t si = 0; si < meshes.size(); ++si) {
        const B3D::SubMesh& sm = meshes[si];
        if (skippedGroup(sm, skipGroups)) continue;
        if (isGroundMaterial(sm.material)) continue;
        if (submeshExtent(sm) < kDecalMinExtent) continue;
        const size_t n = sm.idx.size();
        for (size_t t = 0; t + 2 < n; t += 3) {
            const uint32_t ia = sm.idx[t], ib = sm.idx[t + 1], ic = sm.idx[t + 2];
            if ((size_t)ia * 3 + 2 >= sm.pos.size() ||
                (size_t)ib * 3 + 2 >= sm.pos.size() ||
                (size_t)ic * 3 + 2 >= sm.pos.size())
                continue;
            const float* a = &sm.pos[(size_t)ia * 3];
            const float* b = &sm.pos[(size_t)ib * 3];
            const float* c = &sm.pos[(size_t)ic * 3];
            if (absurd(a) || absurd(b) || absurd(c)) continue;
            if (isForestBillboard(sm.material)) {
                const float tx0 = std::min(a[0], std::min(b[0], c[0]));
                const float tx1 = std::max(a[0], std::max(b[0], c[0]));
                const float tz0 = std::min(a[1], std::min(b[1], c[1]));
                const float tz1 = std::max(a[1], std::max(b[1], c[1]));
                bool coversRoad = false;
                for (float z = std::floor(tz0 / 2.0f) * 2.0f; z <= tz1 && !coversRoad; z += 2.0f)
                    for (float x = std::floor(tx0 / 2.0f) * 2.0f; x <= tx1; x += 2.0f) {
                        const float cx = x + 1.0f, cz = z + 1.0f;
                        if (!roadCells.count(roadKey(cx, cz))) continue;
                        if (pointInTriXZ(a, b, c, cx, cz)) { coversRoad = true; break; }
                    }
                if (coversRoad) continue;   // the billboard lies on the carriageway
            }
            if (triUpComponent(a, b, c) > kHorizontalUp) continue;   // surface
            ++stats_.triangles;
            const size_t wall = walls_.size();
            walls_.push_back({a[0],a[1],a[2],b[0],b[1],b[2],c[0],c[1],c[2]});

            const float tx0 = std::min(a[0], std::min(b[0], c[0]));
            const float tx1 = std::max(a[0], std::max(b[0], c[0]));
            const float tz0 = std::min(a[1], std::min(b[1], c[1]));
            const float tz1 = std::max(a[1], std::max(b[1], c[1]));
            const int bx0 = (int)std::floor(tx0/32), bx1 = (int)std::floor(tx1/32);
            const int bz0 = (int)std::floor(tz0/32), bz1 = (int)std::floor(tz1/32);
            if ((int64_t)(bx1-bx0+1)*(bz1-bz0+1) > 65536)
                wideWalls_.push_back(wall);
            else
                for (int bx=bx0;bx<=bx1;++bx)
                    for (int bz=bz0;bz<=bz1;++bz)
                        wallBuckets_[wallKey(bx,bz)].push_back(wall);
            int i0 = std::max(0, (int)std::floor((tx0 - x0_) / cell_));
            int i1 = std::min(nx_ - 1, (int)std::floor((tx1 - x0_) / cell_));
            int j0 = std::max(0, (int)std::floor((tz0 - z0_) / cell_));
            int j1 = std::min(nz_ - 1, (int)std::floor((tz1 - z0_) / cell_));
            if (i1 < i0 || j1 < j0) continue;
            if ((long long)(i1 - i0 + 1) * (long long)(j1 - j0 + 1) > (1 << 20)) {
                // >1 km2 footprint: row-scan instead of per-cell SAT
                for (int j = j0; j <= j1; ++j) {
                    const float rz0 = z0_ + j * cell_;
                    float xlo = 0, xhi = 0;
                    if (!triBandX(a, b, c, rz0, rz0 + cell_, xlo, xhi)) continue;
                    int r0 = std::max(i0, (int)std::floor((xlo - x0_) / cell_));
                    int r1 = std::min(i1, (int)std::floor((xhi - x0_) / cell_));
                    for (int i = r0; i <= r1; ++i)
                        setBit((size_t)i * (size_t)nz_ + (size_t)j);
                }
                continue;
            }
            for (int i = i0; i <= i1; ++i) {
                const float rx0 = x0_ + i * cell_, rx1 = rx0 + cell_;
                for (int j = j0; j <= j1; ++j) {
                    const size_t cellIdx = (size_t)i * (size_t)nz_ + (size_t)j;
                    if (bit(cellIdx)) continue;
                    const float rz0 = z0_ + j * cell_, rz1 = rz0 + cell_;
                    if (triRectOverlap(a, b, c, rx0, rx1, rz0, rz1))
                        setBit(cellIdx);
                }
            }
        }
    }

    recountSolids();
    LOG("collision: %zu solid triangles -> grid %dx%d @ %.2f m (%.1f MB), "
        "%zu solid cells (%.2f%% of grid); skipped %zu ground + %zu flat of %zu "
        "submeshes",
        stats_.triangles, nx_, nz_, cell_, (double)bits_.size() * 8.0 / 1e6,
        solidCells_, stats_.gridCells ? 100.0 * (double)solidCells_ / (double)stats_.gridCells : 0.0,
        stats_.ground, stats_.flat, stats_.submeshes);
}

void Collision::recountSolids() {
    solidCells_ = 0;
    for (uint64_t w : bits_) solidCells_ += (size_t)popcount64(w);
    stats_.cells = solidCells_;
}

// --- authored thin walls -----------------------------------------------------
// A `hit_*` proxy's subtag-9 record is the impassable *surface* of an object and
// its subtag-23 children bound that surface, so a wall is a finite zero-thickness
// quad - exactly what king.exe's perform_wall_collision (0x57e430) treats it as.
// A rasterised grid cannot hold such a surface without inflating it, so walls are
// kept as their own list and tested by crossing (docs/hit_proxies.md).
void Collision::setWalls(const std::vector<B3D::HitProxy>& proxies) {
    authoredWalls_.clear();
    for (const B3D::HitProxy& p : proxies) {
        for (const B3D::Wall& w : p.walls) {
            if (w.ring.size() < 6) continue;                     // needs >= 2 points
            const float nlen = std::sqrt(w.n[0] * w.n[0] + w.n[1] * w.n[1]);
            if (nlen < 1e-3f) continue;                          // vertical wall only
            if (std::fabs(w.n[2]) > 0.5f * nlen) continue;       // horizontal: floor/roof
            AuthoredWall aw;
            aw.nx = w.n[0] / nlen;
            aw.nz = w.n[1] / nlen;
            aw.d = w.n[3] / nlen;
            aw.ring = w.ring;
            aw.x0 = aw.x1 = w.ring[0];
            aw.z0 = aw.z1 = w.ring[1];
            aw.hLo = w.haveBottom ? w.yMin : 1e30f;
            aw.hHi = -1e30f;
            for (size_t i = 0; i + 2 < w.ring.size(); i += 3) {
                aw.x0 = std::min(aw.x0, w.ring[i]);
                aw.x1 = std::max(aw.x1, w.ring[i]);
                aw.z0 = std::min(aw.z0, w.ring[i + 1]);
                aw.z1 = std::max(aw.z1, w.ring[i + 1]);
                aw.hLo = std::min(aw.hLo, w.ring[i + 2]);
                aw.hHi = std::max(aw.hHi, w.ring[i + 2]);
            }
            if (!(aw.hLo < 1e29f)) aw.hLo = aw.hHi;              // no subtag-12 bottom
            authoredWalls_.push_back(std::move(aw));
        }
    }
}

bool Collision::crossedWall(float fromX, float fromZ, float toX, float toZ,
                           float bottom, float top, float& outX, float& outZ) const {
    if (top < bottom) return false;
    float bestT = 2.0f;
    for (const AuthoredWall& w : authoredWalls_) {
        if (w.hHi < bottom || w.hLo > top) continue;             // another level
        if (std::max(fromX, toX) < w.x0 || std::min(fromX, toX) > w.x1) continue;
        if (std::max(fromZ, toZ) < w.z0 || std::min(fromZ, toZ) > w.z1) continue;
        const float s0 = w.nx * fromX + w.nz * fromZ + w.d;
        const float s1 = w.nx * toX + w.nz * toZ + w.d;
        if ((s0 > 0.0f) == (s1 > 0.0f)) continue;                // no crossing
        const float denom = s0 - s1;
        if (std::fabs(denom) < 1e-9f) continue;
        const float t = s0 / denom;
        if (t < 0.0f || t > 1.0f || t >= bestT) continue;
        const float px = fromX + (toX - fromX) * t;
        const float pz = fromZ + (toZ - fromZ) * t;
        const size_t n = w.ring.size() / 3;
        bool in = false;
        for (size_t a = 0, b = n - 1; a < n; b = a++) {
            const float ax = w.ring[3 * a], az = w.ring[3 * a + 1];
            const float bx = w.ring[3 * b], bz = w.ring[3 * b + 1];
            if ((az > pz) != (bz > pz) &&
                px < (bx - ax) * (pz - az) / (bz - az) + ax)
                in = !in;
        }
        if (!in) continue;                                        // outside the extent
        bestT = t;
        outX = px;
        outZ = pz;
    }
    return bestT <= 1.0f;
}

void Collision::build(const std::vector<B3D::SubMesh>& meshes,
                      const std::vector<B3D::HitProxy>& proxies) {
    build(meshes);                       // the visible-mesh grid stays the baseline
    setWalls(proxies);                   // ... and the authored surfaces go on top
    LOG("collision: %zu authored thin walls from %zu proxies (%zu with an extent); "
        "%zu grid cells", authoredWalls_.size(), proxies.size(),
        std::count_if(authoredWalls_.begin(), authoredWalls_.end(),
                      [](const AuthoredWall& w) { return w.ring.size() >= 6; }),
        solidCells_);
}

bool Collision::blockedHeight(float x, float z, float r, float bottom, float top) const {
    if (top < bottom) return false;
    auto hit = [&](size_t index) {
        const auto& t = walls_[index];
        if (std::max({t[2],t[5],t[8]}) < bottom ||
            std::min({t[2],t[5],t[8]}) > top ||
            std::min({t[0],t[3],t[6]}) > x+r || std::max({t[0],t[3],t[6]}) < x-r ||
            std::min({t[1],t[4],t[7]}) > z+r || std::max({t[1],t[4],t[7]}) < z-r) return false;
        // Clip a triangle to the body's vertical slab BEFORE projecting it.
        // A sloped wall's footprint below a bridge must not block its deck.
        struct P { float x,z,h; };
        std::vector<P> p = {{t[0],t[1],t[2]},{t[3],t[4],t[5]},{t[6],t[7],t[8]}};
        for (int side=0;side<2;++side) {
            std::vector<P> q;
            const float level = side ? top : bottom;
            for (size_t i=0;i<p.size();++i) {
                const P a=p[i], b=p[(i+1)%p.size()];
                const bool ai = side ? a.h<=level : a.h>=level;
                const bool bi = side ? b.h<=level : b.h>=level;
                if (ai) q.push_back(a);
                if (ai!=bi) {
                    const float u=(level-a.h)/(b.h-a.h);
                    q.push_back({a.x+(b.x-a.x)*u,a.z+(b.z-a.z)*u,level});
                }
            }
            p.swap(q);
            if (p.empty()) return false;
        }
        float area=0; bool pos=false,neg=false;
        for (size_t i=0;i<p.size();++i) {
            const P a=p[i],b=p[(i+1)%p.size()];
            const float ex=b.x-a.x,ez=b.z-a.z,len=ex*ex+ez*ez;
            const float u = len>1e-12f ? std::clamp(((x-a.x)*ex+(z-a.z)*ez)/len,0.f,1.f) : 0;
            const float dx=x-a.x-ex*u,dz=z-a.z-ez*u;
            if (dx*dx+dz*dz<=r*r) return true;
            const float cross=ex*(z-a.z)-ez*(x-a.x);
            pos |= cross>1e-6f; neg |= cross< -1e-6f;
            area += (a.x-p[0].x)*(b.z-p[0].z)-(a.z-p[0].z)*(b.x-p[0].x);
        }
        return std::fabs(area)>1e-6f && !(pos&&neg);
    };
    for (size_t i:wideWalls_) if (hit(i)) return true;
    for (int bx=(int)std::floor((x-r)/32);bx<=(int)std::floor((x+r)/32);++bx)
        for (int bz=(int)std::floor((z-r)/32);bz<=(int)std::floor((z+r)/32);++bz) {
            const auto it=wallBuckets_.find(wallKey(bx,bz));
            if (it!=wallBuckets_.end()) for (size_t i:it->second) if (hit(i)) return true;
        }
    return false;
}

bool Collision::blocked(float x, float z, float r, float bottom, float top) const {
    if (bits_.empty()) return false;
    if (r < 0.0f) r = 0.0f;
    if (std::isfinite(bottom) && std::isfinite(top))
        return blockedHeight(x,z,r,bottom,top);
    // Grid bbox of the circle; reject early when it is fully outside.
    const float gx1 = x0_ + (float)nx_ * cell_;
    const float gz1 = z0_ + (float)nz_ * cell_;
    if (x + r < x0_ || x - r > gx1 || z + r < z0_ || z - r > gz1) return false;
    int i0 = (int)std::floor((x - r - x0_) / cell_);
    int i1 = (int)std::floor((x + r - x0_) / cell_);
    int j0 = (int)std::floor((z - r - z0_) / cell_);
    int j1 = (int)std::floor((z + r - z0_) / cell_);
    i0 = std::max(i0, 0); i1 = std::min(i1, nx_ - 1);
    j0 = std::max(j0, 0); j1 = std::min(j1, nz_ - 1);
    if (i1 < i0 || j1 < j0) return false;
    const float r2 = r * r;
    for (int i = i0; i <= i1; ++i) {
        const float rx0 = x0_ + i * cell_;
        const float dx = std::max(rx0 - x, std::max(0.0f, x - (rx0 + cell_)));
        const float dx2 = dx * dx;
        if (dx2 > r2) continue;
        for (int j = j0; j <= j1; ++j) {
            if (!bit((size_t)i * (size_t)nz_ + (size_t)j)) continue;
            const float rz0 = z0_ + j * cell_;
            const float dz = std::max(rz0 - z, std::max(0.0f, z - (rz0 + cell_)));
            if (dx2 + dz * dz <= r2) return true;
        }
    }
    return false;
}

float Collision::sweep(float fromX, float fromZ, float toX, float toZ, float r,
                       float bottom, float top) const {
    const float dx = toX - fromX, dz = toZ - fromZ;
    const float len = std::sqrt(dx * dx + dz * dz);
    if (len < 1e-5f) return blocked(fromX, fromZ, r,bottom,top) ? 0.0f : 1.0f;
    const float step = std::max(0.25f, 0.5f * cell_);
    int steps = (int)(len / step) + 1;
    steps = std::min(std::max(steps, 2), 256);
    float best = 0.0f, blockedAt = 1.0f;
    for (int s = 1; s <= steps; ++s) {
        const float t = (float)s / (float)steps;
        if (blocked(fromX + dx * t, fromZ + dz * t, r,bottom,top)) { blockedAt = t; break; }
        best = t;
        blockedAt = t;
    }
    if (best <= 0.0f && blocked(fromX, fromZ, r,bottom,top)) return 0.0f;
    // refine between the last free and the first blocked sample
    float lo = best, hi = blockedAt;
    if (hi <= lo) return 1.0f;
    for (int it = 0; it < 10; ++it) {
        const float mid = 0.5f * (lo + hi);
        if (blocked(fromX + dx * mid, fromZ + dz * mid, r,bottom,top)) hi = mid;
        else lo = mid;
    }
    return lo;
}

bool Collision::contactNormal(float x, float z, float r, float& outNx, float& outNz,
                              float bottom, float top) const {
    if (bits_.empty()) return false;
    const float reach = r + cell_;
    int i0 = std::max(0, (int)std::floor((x - reach - x0_) / cell_));
    int i1 = std::min(nx_ - 1, (int)std::floor((x + reach - x0_) / cell_));
    int j0 = std::max(0, (int)std::floor((z - reach - z0_) / cell_));
    int j1 = std::min(nz_ - 1, (int)std::floor((z + reach - z0_) / cell_));
    float sx = 0.0f, sz = 0.0f;
    // Accept any cell within `reach`: after the sweep stopped, the circle may be
    // exactly tangent to the wall, i.e. no cell overlaps it any more.
    const float reach2 = reach * reach;
    for (int i = i0; i <= i1; ++i) {
        const float rx0 = x0_ + i * cell_, rx1 = rx0 + cell_;
        for (int j = j0; j <= j1; ++j) {
            if (!bit((size_t)i * (size_t)nz_ + (size_t)j)) continue;
            const float rz0 = z0_ + j * cell_, rz1 = rz0 + cell_;
            if (std::isfinite(bottom) && std::isfinite(top) &&
                !blockedHeight((rx0+rx1)*.5f,(rz0+rz1)*.5f,cell_*.71f,bottom,top)) continue;
            const float cx = std::min(std::max(x, rx0), rx1);
            const float cz = std::min(std::max(z, rz0), rz1);
            float nx = x - cx, nz = z - cz;
            float d2 = nx * nx + nz * nz;
            if (d2 > reach2) continue;                   // too far to matter
            if (d2 < 1e-8f) {                            // centre inside cell
                nx = x - 0.5f * (rx0 + rx1);
                nz = z - 0.5f * (rz0 + rz1);
                const float l = std::sqrt(nx * nx + nz * nz);
                if (l < 1e-6f) continue;
                nx /= l; nz /= l;
                d2 = 0.0f;
            } else {
                const float l = std::sqrt(d2);
                nx /= l; nz /= l;
                d2 = l * l;
            }
            const float w = std::max(0.05f, reach - std::sqrt(d2));
            sx += nx * w;
            sz += nz * w;
        }
    }
    const float l = std::sqrt(sx * sx + sz * sz);
    if (l < 1e-5f) return false;
    outNx = sx / l;
    outNz = sz / l;
    return true;
}

bool Collision::resolveMove(float fromX, float fromZ, float toX, float toZ, float r,
                            float& outX, float& outZ, float bottom, float top) const {
    if (bits_.empty() && authoredWalls_.empty()) {
        outX = toX;
        outZ = toZ;
        return true;
    }
    // The authored thin walls (docs/hit_proxies.md) come first: they are exact
    // surfaces, so the car stops *at* the plane instead of one cell short of it.
    if (!authoredWalls_.empty()) {
        float wx = 0, wz = 0;
        if (crossedWall(fromX, fromZ, toX, toZ, bottom, top, wx, wz)) {
            const float len = std::sqrt((toX - fromX) * (toX - fromX) +
                                        (toZ - fromZ) * (toZ - fromZ));
            if (len > 1e-4f) {                       // stop just before the surface
                const float back = std::min(0.05f, len * 0.5f) / len;
                wx -= (wx - fromX) * back;
                wz -= (wz - fromZ) * back;
            }
            outX = wx;
            outZ = wz;
            return false;
        }
    }
    if (bits_.empty()) {
        outX = toX;
        outZ = toZ;
        return true;
    }
    // Always sweep the segment: a fast step must not tunnel through a wall even
    // when the destination itself happens to be free.
    float t = sweep(fromX, fromZ, toX, toZ, r,bottom,top);
    if (t >= 1.0f - 1e-4f) {
        outX = toX;
        outZ = toZ;
        return true;
    }
    // Back off a hair so the contact point is strictly free: sliding flush along
    // a wall then does not immediately re-collide with the cell it touches.
    const float moveLen = std::sqrt((toX - fromX) * (toX - fromX) +
                                    (toZ - fromZ) * (toZ - fromZ));
    if (moveLen > 1e-4f)
        t = std::max(0.0f, t - 1e-3f / moveLen);
    float px = fromX + (toX - fromX) * t;
    float pz = fromZ + (toZ - fromZ) * t;
    float remX = (toX - fromX) * (1.0f - t);
    float remZ = (toZ - fromZ) * (1.0f - t);
    // 2) slide: drop the component pushing into the wall and sweep the rest
    for (int it = 0; it < 3; ++it) {
        const float remLen = std::sqrt(remX * remX + remZ * remZ);
        if (remLen < 1e-4f) break;
        float nx = 0.0f, nz = 0.0f;
        if (!contactNormal(px, pz, r, nx, nz,bottom,top)) break;
        const float into = remX * nx + remZ * nz;
        if (into >= 0.0f) break;                      // already moving away
        remX -= nx * into;
        remZ -= nz * into;
        if (std::sqrt(remX * remX + remZ * remZ) < 1e-4f) break;
        const float st = sweep(px, pz, px + remX, pz + remZ, r,bottom,top);
        px += remX * st;
        pz += remZ * st;
        if (st >= 1.0f - 1e-4f) break;                // slid all the way
        remX *= (1.0f - st);
        remZ *= (1.0f - st);
    }
    outX = px;
    outZ = pz;
    return false;
}

// ---------------------------------------------------------------------------
// Capsule bodies (cars / trucks)
// ---------------------------------------------------------------------------
// A body is the segment of its axis (front axle to rear axle) with a radius
// around it.  Every query samples the axis with circles spaced at most
// `radius` apart, so a wall can never slip between two samples while the
// rounded ends keep the car from catching on corners.  yaw is degrees with
// forward = (sin yaw, cos yaw), the Vehicle/Renderer convention.
void Collision::bodyAxis(float x, float z, float yawDeg, const Body& b,
                         float& ax, float& az, float& bx, float& bz) {
    const float a = yawDeg * 3.14159265358979f / 180.0f;
    const float fx = std::sin(a), fz = std::cos(a);
    const float h = b.halfAxis();
    ax = x + fx * h; az = z + fz * h;
    bx = x - fx * h; bz = z - fz * h;
}

bool Collision::blockedBody(float x, float z, float yawDeg, const Body& b) const {
    if (bits_.empty()) return false;
    const float r = b.radius > 0.2f ? b.radius : 0.2f;
    float ax, az, bx, bz;
    bodyAxis(x, z, yawDeg, b, ax, az, bx, bz);
    const float dx = bx - ax, dz = bz - az;
    const float len = std::sqrt(dx * dx + dz * dz);
    const int n = (int)(len / r) + 1;                 // samples, at most r apart
    for (int i = 0; i <= n; ++i) {
        const float t = (n > 0) ? (float)i / (float)n : 0.5f;
        if (blocked(ax + dx * t, az + dz * t, r)) return true;
    }
    return false;
}

bool Collision::resolveBody(float fromX, float fromZ, float fromYaw,
                            float toX, float toZ, float toYaw, const Body& b,
                            float& outX, float& outZ) const {
    if (bits_.empty()) { outX = toX; outZ = toZ; return true; }
    if (!blockedBody(toX, toZ, toYaw, b)) { outX = toX; outZ = toZ; return true; }
    if (blockedBody(fromX, fromZ, fromYaw, b)) { outX = fromX; outZ = fromZ; return false; }
    // The move is one frame (a few metres at most), so a bisection on t is both
    // exact enough and cheaper than a multi-circle sweep.
    float lo = 0.0f, hi = 1.0f;
    for (int i = 0; i < 12; ++i) {
        const float m = 0.5f * (lo + hi);
        const float mx = fromX + (toX - fromX) * m;
        const float mz = fromZ + (toZ - fromZ) * m;
        float dy = toYaw - fromYaw;
        while (dy > 180.0f) dy -= 360.0f;
        while (dy < -180.0f) dy += 360.0f;
        if (blockedBody(mx, mz, fromYaw + dy * m, b)) hi = m;
        else lo = m;
    }
    float px = fromX + (toX - fromX) * lo;
    float pz = fromZ + (toZ - fromZ) * lo;
    // Slide: drop the part of the remaining motion that pushes into the wall.
    float remX = toX - px, remZ = toZ - pz;
    float yawHere = fromYaw;
    for (int it = 0; it < 3; ++it) {
        const float remLen = std::sqrt(remX * remX + remZ * remZ);
        if (remLen < 1e-3f) break;
        float nx = 0.0f, nz = 0.0f;
        if (!contactNormal(px, pz, b.radius, nx, nz)) break;
        const float into = remX * nx + remZ * nz;
        if (into >= 0.0f) break;
        remX -= nx * into;
        remZ -= nz * into;
        const float step = std::sqrt(remX * remX + remZ * remZ);
        if (step < 1e-3f) break;
        float slo = 0.0f, shi = 1.0f;
        for (int i = 0; i < 10; ++i) {
            const float m = 0.5f * (slo + shi);
            if (blockedBody(px + remX * m, pz + remZ * m, yawHere, b)) shi = m;
            else slo = m;
        }
        px += remX * slo;
        pz += remZ * slo;
        if (slo >= 1.0f - 1e-4f) break;
        remX *= (1.0f - slo);
        remZ *= (1.0f - slo);
    }
    outX = px;
    outZ = pz;
    return false;
}

float Collision::bodyOverlap(float ax, float az, float ayawDeg, const Body& a,
                             float bx, float bz, float byawDeg, const Body& b,
                             float& nx, float& nz) {
    float a1x, a1z, a2x, a2z, b1x, b1z, b2x, b2z;
    bodyAxis(ax, az, ayawDeg, a, a1x, a1z, a2x, a2z);
    bodyAxis(bx, bz, byawDeg, b, b1x, b1z, b2x, b2z);
    const float A[2][2] = {{a1x, a1z}, {a2x, a2z}};
    const float B[2][2] = {{b1x, b1z}, {b2x, b2z}};
    float best = 1e30f;
    float cax = ax, caz = az, cbx = bx, cbz = bz;
    auto segClosest = [](const float p[2], const float q0[2], const float q1[2],
                         float out[2]) {
        const float dx = q1[0] - q0[0], dz = q1[1] - q0[1];
        const float l2 = dx * dx + dz * dz;
        float t = l2 > 1e-9f ? ((p[0] - q0[0]) * dx + (p[1] - q0[1]) * dz) / l2 : 0.0f;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        out[0] = q0[0] + dx * t;
        out[1] = q0[1] + dz * t;
    };
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            float ca[2], cb[2];
            segClosest(B[j], A[i], A[1 - i], ca);      // point on A closest to B[j]
            const float d1 = (ca[0] - B[j][0]) * (ca[0] - B[j][0]) +
                             (ca[1] - B[j][1]) * (ca[1] - B[j][1]);
            segClosest(A[i], B[j], B[1 - j], cb);      // point on B closest to A[i]
            const float d2 = (cb[0] - A[i][0]) * (cb[0] - A[i][0]) +
                             (cb[1] - A[i][1]) * (cb[1] - A[i][1]);
            if (d1 < best) { best = d1; cax = ca[0]; caz = ca[1]; cbx = B[j][0]; cbz = B[j][1]; }
            if (d2 < best) { best = d2; cax = A[i][0]; caz = A[i][1]; cbx = cb[0]; cbz = cb[1]; }
        }
    }
    float dx = cbx - cax, dz = cbz - caz;
    float d = std::sqrt(dx * dx + dz * dz);
    if (d < 1e-4f) {                                   // concentric: pick an axis
        const float ang = ayawDeg * 3.14159265358979f / 180.0f;
        nx = -std::cos(ang);
        nz = std::sin(ang);
        return a.radius + b.radius;
    }
    nx = dx / d;
    nz = dz / d;
    return a.radius + b.radius - d;
}

} // namespace ht2

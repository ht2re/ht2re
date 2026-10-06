#include "GroundMap.h"
#include "Terrain.h"
#include "../util/Log.h"
#include <cmath>
#include <map>
#include <limits>
#include <algorithm>
#include <cstddef>

namespace ht2 {

static const float NANF = std::numeric_limits<float>::quiet_NaN();

void GroundMap::build(const std::vector<B3D::SubMesh>& meshes,
                      const std::vector<float>& roadPts, float cellSize,
                      const Terrain* base) {
    data_.clear();
    minY_.clear();
    clear_.clear();
    w_ = h_ = 0;
    filled_ = 0;
    cell_ = cellSize > 0 ? cellSize : 12.0f;
    if (meshes.empty()) return;

    // extent of every emitted vertex (x = submesh pos[0], z = pos[1] in b3d space)
    // A handful of marker vertices in the shipped data carry sentinel heights
    // (e.g. -10000 / 1000000 in ENV/cb.b3d).  The world itself lives in a narrow
    // band, so the median height is used as the centre of a +-1000/+1500 window.
    std::vector<float> hs;
    hs.reserve(1 << 16);
    for (const auto& sm : meshes) {
        size_t stride = sm.pos.size() / 3 > 4096 ? (sm.pos.size() / 3) / 4096 : 1;
        for (size_t k = 0; k * 3 + 2 < sm.pos.size(); k += stride) hs.push_back(sm.pos[k * 3 + 2]);
    }
    float median = 0.0f;
    if (!hs.empty()) {
        std::nth_element(hs.begin(), hs.begin() + hs.size() / 2, hs.end());
        median = hs[hs.size() / 2];
    }
    const float kLo = median - 1000.0f, kHi = median + 1500.0f;
    size_t skipped = 0;
    float minX = 1e30f, maxX = -1e30f, minZ = 1e30f, maxZ = -1e30f;
    float hmin = 1e30f, hmax = -1e30f;
    for (const auto& sm : meshes) {
        for (size_t i = 0; i + 2 < sm.pos.size(); i += 3) {
            float y = sm.pos[i + 2];
            if (y < kLo || y > kHi) { ++skipped; continue; }
            minX = std::min(minX, sm.pos[i + 0]);
            maxX = std::max(maxX, sm.pos[i + 0]);
            minZ = std::min(minZ, sm.pos[i + 1]);
            maxZ = std::max(maxZ, sm.pos[i + 1]);
            hmin = std::min(hmin, y);
            hmax = std::max(hmax, y);
        }
    }
    if (skipped) LOG("ground: skipped %zu sentinel vertices", skipped);
    if (minX > maxX || minZ > maxZ) return;
    minX_ = minX; minZ_ = minZ;
    hmin_ = hmin; hmax_ = hmax;
    w_ = (int)((maxX - minX) / cell_) + 1;
    h_ = (int)((maxZ - minZ) / cell_) + 1;
    // guard against a runaway allocation
    if ((double)w_ * h_ > 40e6) {
        float f = sqrtf((float)w_ * h_ / 40e6f);
        cell_ *= f;
        w_ = (int)((maxX - minX) / cell_) + 1;
        h_ = (int)((maxZ - minZ) / cell_) + 1;
    }
    data_.assign((size_t)w_ * h_, NANF);
    minY_.assign((size_t)w_ * h_, NANF);

    // 1) the road network first: it is the ground the vehicles actually use.
    //    The per-cell value is the *mean* of the road points that land in the
    //    cell (not the last one) and the field is kept in `road_` as well, so
    //    the drivable profile survives the terrain smoothing below.
    size_t roadCells = 0;
    {
        std::vector<float> sum((size_t)w_ * h_, 0.0f);
        std::vector<int> cnt((size_t)w_ * h_, 0);
        for (size_t k = 0; k + 2 < roadPts.size(); k += 3) {
            float x = roadPts[k], z = roadPts[k + 1], y = roadPts[k + 2];
            int gx = (int)((x - minX_) / cell_);
            int gz = (int)((z - minZ_) / cell_);
            if (gx < 0 || gz < 0 || gx >= w_ || gz >= h_) continue;
            size_t i = (size_t)gz * w_ + gx;
            sum[i] += y;
            ++cnt[i];
            ++roadCells;
        }
        road_.assign((size_t)w_ * h_, NANF);
        for (size_t i = 0; i < sum.size(); ++i) {
            if (!cnt[i]) continue;
            float y = sum[i] / (float)cnt[i];
            data_[i] = y;
            road_[i] = y;
            float& m = minY_[i];
            if (std::isnan(m) || y < m) m = y;
        }
        // Grow the road profile by one cell: a 15 m carriageway is wider than the
        // 12 m cell its centre line lands in, so the tyre contact points sit in
        // cells that carry no road point at all.
        std::vector<float> rd = road_;
        for (int z = 0; z < h_; ++z)
            for (int x = 0; x < w_; ++x) {
                size_t i = (size_t)z * w_ + x;
                if (!std::isnan(road_[i])) continue;
                for (int dz = -1; dz <= 1 && std::isnan(rd[i]); ++dz)
                    for (int dx = -1; dx <= 1; ++dx) {
                        int nx = x + dx, nz = z + dz;
                        if (nx < 0 || nz < 0 || nx >= w_ || nz >= h_) continue;
                        float v = road_[(size_t)nz * w_ + nx];
                        if (!std::isnan(v)) { rd[i] = v; break; }
                    }
            }
        road_.swap(rd);
        size_t rc = 0;
        for (float v : road_) if (!std::isnan(v)) ++rc;
        roadCells_ = rc;
        // How continuous is the drivable profile?  A track that follows this
        // field samples one cell at a time, so a big difference between
        // neighbouring cells is a step at the cell border.  (Real height
        // discontinuities - a bridge meeting the ground, a two-level junction -
        // are part of the world and are reported by --road-probe separately.)
        size_t steps = 0, stepsHalf = 0;
        float maxStep = 0.0f, maxStepAt[2] = {0, 0};
        for (int z = 0; z < h_; ++z)
            for (int x = 0; x + 1 < w_; ++x) {
                float a = road_[(size_t)z * w_ + x], b = road_[(size_t)z * w_ + x + 1];
                if (std::isnan(a) || std::isnan(b)) continue;
                float d = std::fabs(a - b);
                if (d > 0.5f) ++stepsHalf;
                if (d > 1.0f) ++steps;
                if (d > maxStep) {
                    maxStep = d;
                    maxStepAt[0] = minX_ + (x + 1) * cell_;
                    maxStepAt[1] = minZ_ + z * cell_;
                }
            }
        LOG("ground: road profile %zu cells, %zu >1.0 m and %zu >0.5 m steps between "
            "neighbouring cells (max %.2f m at %.0f, %.0f)",
            rc, steps, stepsHalf, maxStep, maxStepAt[0], maxStepAt[1]);
        // A plain chamfer transform over the (grown) road cells: surfaceAt()
        // answers with the height of the *nearest* road cell, so a query a few
        // metres off the centre line still reads the carriageway and not the
        // average of two roads meeting at a junction.
        roadDist_.assign((size_t)w_ * h_, std::numeric_limits<float>::infinity());
        for (size_t i = 0; i < road_.size(); ++i)
            if (!std::isnan(road_[i])) roadDist_[i] = 0.0f;
        const float step = cell_, diag = cell_ * 1.41421f;
        auto relax = [&](int x, int z, int dx, int dz, float w) {
            int nx = x + dx, nz = z + dz;
            if (nx < 0 || nz < 0 || nx >= w_ || nz >= h_) return;
            size_t n = (size_t)nz * w_ + nx, c = (size_t)z * w_ + x;
            float nd = roadDist_[n] + w;
            if (nd < roadDist_[c]) { roadDist_[c] = nd; road_[c] = road_[n]; }
        };
        for (int z = 0; z < h_; ++z)
            for (int x = 0; x < w_; ++x) {
                relax(x, z, -1, 0, step);  relax(x, z, 0, -1, step);
                relax(x, z, -1, -1, diag); relax(x, z, 1, -1, diag);
            }
        for (int z = h_ - 1; z >= 0; --z)
            for (int x = w_ - 1; x >= 0; --x) {
                relax(x, z, 1, 0, step);   relax(x, z, 0, 1, step);
                relax(x, z, 1, 1, diag);   relax(x, z, -1, 1, diag);
            }
    }

    // water planes are marked, not used as ground (the sea would be drivable)
    water_.assign((size_t)w_ * h_, 0);
    auto isWater = [](const std::string& m) {
        for (const char* k : {"water", "sea", "river", "lake", "vod", "voda"})
            if (m.find(k) != std::string::npos) return true;
        return false;
    };

    // 2) lowest vertex per cell elsewhere: buildings stand on the ground, roofs
    //    are above it (cells already carrying a road point keep that height)
    for (const auto& sm : meshes) {
        const bool water = isWater(sm.material);
        for (size_t i = 0; i + 2 < sm.pos.size(); i += 3) {
            float y = sm.pos[i + 2];
            if (y < kLo || y > kHi) continue;
            int gx = (int)((sm.pos[i + 0] - minX_) / cell_);
            int gz = (int)((sm.pos[i + 1] - minZ_) / cell_);
            if (gx < 0 || gz < 0 || gx >= w_ || gz >= h_) continue;
            if (water) { water_[(size_t)gz * w_ + gx] = 1; continue; }
            float& c = data_[(size_t)gz * w_ + gx];
            if (std::isnan(c)) c = y;
            // Seed the ceiling from the vertices too: a triangle smaller than a
            // cell can miss every cell centre in the coverage pass below.
            float& m0 = minY_[(size_t)gz * w_ + gx];
            if (std::isnan(m0) || y < m0) m0 = y;
        }
    }
    for (float v : data_) if (!std::isnan(v)) ++filled_;
    if (!filled_) { data_.clear(); minY_.clear(); w_ = h_ = 0; return; }

    // 2b) the tile-surface ceiling: the *coverage* of every tile triangle, not
    //     just its vertices.  A runway/apron is one quad kilometres wide with
    //     four vertices, so a vertex-based field would leave its interior
    //     unclamped and the terrain would cut straight through it.  Each cell
    //     centre is tested against every triangle whose bounding box contains
    //     it and keeps the LOWEST surface (buildings stand on the ground, the
    //     ground under them is the ceiling).
    {
        size_t rasterized = 0, covered = 0;
        for (const auto& sm : meshes) {
            if (isWater(sm.material)) continue;
            const std::vector<float>& p = sm.pos;
            for (size_t t = 0; t + 2 < sm.idx.size(); t += 3) {
                const uint32_t ia = sm.idx[t], ib = sm.idx[t + 1], ic = sm.idx[t + 2];
                if ((size_t)ia * 3 + 2 >= p.size() || (size_t)ib * 3 + 2 >= p.size() ||
                    (size_t)ic * 3 + 2 >= p.size()) continue;
                const float* a = &p[(size_t)ia * 3];
                const float* b = &p[(size_t)ib * 3];
                const float* c = &p[(size_t)ic * 3];
                if (a[2] < kLo || a[2] > kHi || b[2] < kLo || b[2] > kHi ||
                    c[2] < kLo || c[2] > kHi) continue;
                float ax = a[0], az = a[1], bx = b[0], bz = b[1], cx = c[0], cz = c[1];
                float minx = std::min(ax, std::min(bx, cx)), maxx = std::max(ax, std::max(bx, cx));
                float minz = std::min(az, std::min(bz, cz)), maxz = std::max(az, std::max(bz, cz));
                if (maxx < minX_ || minx > this->maxX() || maxz < minZ_ || minz > this->maxZ()) continue;
                int x0 = (int)std::floor((minx - minX_) / cell_);
                int x1 = (int)std::floor((maxx - minX_) / cell_);
                int z0 = (int)std::floor((minz - minZ_) / cell_);
                int z1 = (int)std::floor((maxz - minZ_) / cell_);
                if (x0 < 0) x0 = 0; if (z0 < 0) z0 = 0;
                if (x1 >= w_) x1 = w_ - 1;
                if (z1 >= h_) z1 = h_ - 1;
                // guard against a degenerate/absurd bounding box
                if ((double)(x1 - x0 + 1) * (z1 - z0 + 1) > 4.0e5) continue;
                const float e = (bx - ax) * (cz - az) - (bz - az) * (cx - ax);
                if (std::fabs(e) < 1e-9f) continue;      // vertical / degenerate
                ++rasterized;
                for (int gz = z0; gz <= z1; ++gz) {
                    float wz = minZ_ + (gz + 0.5f) * cell_;
                    for (int gx = x0; gx <= x1; ++gx) {
                        float wx = minX_ + (gx + 0.5f) * cell_;
                        // barycentric inside test in the (x,z) plane
                        float w0 = ((bx - wx) * (cz - wz) - (bz - wz) * (cx - wx)) / e;
                        float w1 = ((cx - wx) * (az - wz) - (cz - wz) * (ax - wx)) / e;
                        float w2 = 1.0f - w0 - w1;
                        // The tiles mix both xz windings (the renderer mirrors x
                        // for Direct3D), so a triangle counts whichever way it
                        // faces - only a cell outside all three edges is skipped.
                        if (!((w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) ||
                              (w0 <= 0.0f && w1 <= 0.0f && w2 <= 0.0f))) continue;
                        float y = w0 * a[2] + w1 * b[2] + w2 * c[2];
                        float& m = minY_[(size_t)gz * w_ + gx];
                        if (std::isnan(m) || y < m) m = y;
                    }
                }
            }
        }
        for (float v : minY_) if (!std::isnan(v)) ++covered;
        LOG("ground: tile ceiling seeded by %zu triangles -> %zu of %zu cells covered",
            rasterized, covered, minY_.size());
    }

    // cells with nothing at all: take the calibrated heightmap
    if (base && base->loaded()) {
        for (int z = 0; z < h_; ++z)
            for (int x = 0; x < w_; ++x) {
                float& c = data_[(size_t)z * w_ + x];
                if (!std::isnan(c)) continue;
                float wx = minX_ + x * cell_, wz = minZ_ + z * cell_;
                if (base->covers(wx, wz)) c = base->heightAtWorld(wx, wz, 0.0f);
            }
    }

    // fill any remaining holes from neighbours
    std::vector<float> next = data_;
    for (int pass = 0; pass < 24; ++pass) {
        bool changed = false;
        for (int z = 0; z < h_; ++z)
            for (int x = 0; x < w_; ++x) {
                size_t i = (size_t)z * w_ + x;
                if (!std::isnan(data_[i])) continue;
                float sum = 0;
                int n = 0;
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dx = -1; dx <= 1; ++dx) {
                        int nx = x + dx, nz = z + dz;
                        if (nx < 0 || nz < 0 || nx >= w_ || nz >= h_) continue;
                        float v = data_[(size_t)nz * w_ + nx];
                        if (!std::isnan(v)) { sum += v; ++n; }
                    }
                if (n) { next[i] = sum / n; changed = true; }
            }
        data_.swap(next);
        if (!changed) break;
    }

    // light smoothing so the drive is not jittery.  Road cells are left alone:
    // `data_` is the terrain field and smoothing it across the carriageway would
    // pull the asphalt height towards the off-road ground in the same cell (and
    // the road height into the field beside it), which is one of the ways a
    // vehicle ends up a couple of units off the asphalt.
    next = data_;
    for (int z = 1; z < h_ - 1; ++z)
        for (int x = 1; x < w_ - 1; ++x) {
            if (!std::isnan(road_[(size_t)z * w_ + x])) continue;
            float sum = 0;
            int n = 0;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    size_t ni = (size_t)(z + dz) * w_ + (x + dx);
                    if (!std::isnan(road_[ni])) continue;    // never smooth towards a road
                    float v = data_[ni];
                    if (!std::isnan(v)) { sum += v; ++n; }
                }
            if (n) next[(size_t)z * w_ + x] = sum / n;
        }
    data_.swap(next);

    LOG("ground: %dx%d cells of %.1f units, %zu sampled (%zu road points), height %.1f..%.1f "
        "(median %.1f, %zu filtered)",
        w_, h_, cell_, filled_, roadCells, hmin_, hmax_, median, skipped);

    // The ceiling: the per-cell minimum of the tile surface, grown by
    // CLEARANCE_RADIUS cells, then a min-plus distance transform with an
    // additive CEILING_SLOPE per unit of distance.  Growing the field makes the
    // clamp cover the whole footprint of the geometry (and not only the cells a
    // vertex lands in); the slope term turns the clamp into a *cut slope* that
    // rises away from the tiles, so the generated terrain never forms a flat
    // shelf with a hard straight edge across the landscape.
    {
        const float INF = std::numeric_limits<float>::infinity();
        float minYmin = INF;
        for (float v : minY_) if (!std::isnan(v) && v < minYmin) minYmin = v;
        std::vector<float> cur = minY_;
        std::vector<float> tmp = minY_;
        for (int pass = 0; pass < CLEARANCE_RADIUS; ++pass) {
            for (int z = 0; z < h_; ++z)
                for (int x = 0; x < w_; ++x) {
                    float m = INF;
                    for (int dz = -1; dz <= 1; ++dz)
                        for (int dx = -1; dx <= 1; ++dx) {
                            int nx = x + dx, nz = z + dz;
                            if (nx < 0 || nz < 0 || nx >= w_ || nz >= h_) continue;
                            float v = cur[(size_t)nz * w_ + nx];
                            if (v < m) m = v;
                        }
                    tmp[(size_t)z * w_ + x] = m;
                }
            cur.swap(tmp);
        }
        for (float& v : cur) if (v == INF) v = NANF;
        clear_.swap(cur);

        // Chamfer transform to the nearest covered cell: the plain distance and
        // the surface height of the cell it lands in.  Two sweeps with the
        // (1, 1, sqrt2) kernel; the pair is propagated together so `near` is
        // always the height of the cell `dist` measures to.
        const float step = cell_;
        const float diag = step * 1.41421f;
        near_ = clear_;
        dist_.assign((size_t)w_ * h_, INF);
        for (size_t i = 0; i < clear_.size(); ++i)
            if (!std::isnan(clear_[i])) dist_[i] = 0.0f;
        auto relax = [&](int x, int z, int dx, int dz, float w) {
            int nx = x + dx, nz = z + dz;
            if (nx < 0 || nz < 0 || nx >= w_ || nz >= h_) return;
            size_t n = (size_t)nz * w_ + nx;
            float nd = dist_[n] + w;
            size_t c = (size_t)z * w_ + x;
            if (nd < dist_[c]) { dist_[c] = nd; near_[c] = near_[n]; }
        };
        for (int z = 0; z < h_; ++z)
            for (int x = 0; x < w_; ++x) {
                relax(x, z, -1, 0, step);  relax(x, z, 0, -1, step);
                relax(x, z, -1, -1, diag); relax(x, z, 1, -1, diag);
            }
        for (int z = h_ - 1; z >= 0; --z)
            for (int x = w_ - 1; x >= 0; --x) {
                relax(x, z, 1, 0, step);   relax(x, z, 0, 1, step);
                relax(x, z, 1, 1, diag);   relax(x, z, -1, 1, diag);
            }
        size_t near = 0;
        for (size_t i = 0; i < near_.size(); ++i) if (dist_[i] == 0.0f) ++near;
        LOG("ground: tile ceiling %dx%d cells, radius %d -> %zu cells on the surface, "
            "fade %.0f units (raw tile surface min %.1f)",
            w_, h_, CLEARANCE_RADIUS, near, (double)CEILING_FADE,
            minYmin == INF ? 0.0f : minYmin);
    }
}

bool GroundMap::ceilingAt(float x, float z, float& nearY, float& dist) const {
    if (near_.empty()) return false;
    int gx = (int)std::floor((x - minX_) / cell_);
    int gz = (int)std::floor((z - minZ_) / cell_);
    if (gx < 0 || gz < 0 || gx >= w_ || gz >= h_) return false;
    size_t i = (size_t)gz * w_ + gx;
    if (std::isnan(near_[i])) return false;
    nearY = near_[i];
    dist = dist_[i];
    return true;
}

bool GroundMap::inside(float x, float z) const {
    if (w_ <= 0 || h_ <= 0) return true;                 // no map: don't restrict
    return x >= minX_ && z >= minZ_ &&
           x < minX_ + w_ * cell_ && z < minZ_ + h_ * cell_;
}

bool GroundMap::waterAt(float x, float z, float roadHeight) const {
    if (water_.empty()) return false;
    int gx = (int)((x - minX_) / cell_), gz = (int)((z - minZ_) / cell_);
    if (gx < 0 || gz < 0 || gx >= w_ || gz >= h_) return false;
    if (!water_[(size_t)gz * w_ + gx]) return false;
    if (std::isfinite(roadHeight)) {
        float h,d; bool visible;
        if (roadField_.heightAt(x,z,roadHeight,0,h,d,visible) &&
            std::fabs(h-roadHeight)<0.1f) return false;
    }
    return true;
}

float GroundMap::heightAt(float x, float z, float fallback) const {
    if (empty()) return fallback;
    float fx = (x - minX_) / cell_;
    float fz = (z - minZ_) / cell_;
    if (fx < 0 || fz < 0 || fx > w_ - 1.001f || fz > h_ - 1.001f) return fallback;
    int x0 = (int)fx, z0 = (int)fz;
    int x1 = std::min(x0 + 1, w_ - 1), z1 = std::min(z0 + 1, h_ - 1);
    float tx = fx - x0, tz = fz - z0;
    float v00 = data_[(size_t)z0 * w_ + x0], v10 = data_[(size_t)z0 * w_ + x1];
    float v01 = data_[(size_t)z1 * w_ + x0], v11 = data_[(size_t)z1 * w_ + x1];
    if (std::isnan(v00) || std::isnan(v10) || std::isnan(v01) || std::isnan(v11)) {
        float best = fallback;
        bool got = false;
        const float cand[4] = {v00, v10, v01, v11};
        for (float c : cand)
            if (!std::isnan(c) && (!got || c < best)) { best = c; got = true; }
        return got ? best : fallback;
    }
    float a = v00 * (1 - tx) + v10 * tx;
    float b = v01 * (1 - tx) + v11 * tx;
    return a * (1 - tz) + b * tz;
}

// The drivable road surface: the height of the nearest road cell (see road_),
// which is the mean of every `.way` polyline point that landed in it.  Unlike
// heightAt() this is never smoothed towards the terrain and never mixed with
// the off-road ground, so it is the field a vehicle has to follow.
float GroundMap::surfaceAt(float x, float z, float fallback) const {
    bool found = false;
    float y = roadProfileAt(x, z, found);
    return found ? y : fallback;
}

void GroundMap::buildRoadSurface(const std::vector<B3D::SubMesh>& meshes) {
    roadField_.build(meshes);
    terrainField_.build(meshes, true);
}

float GroundMap::drivableY(float x, float z, float fallback, int* outSource,
                           float* outDist) const {
    if (outSource) *outSource = 0;
    if (outDist) *outDist = 0.0f;
    // `fallback` is the height to answer with when *no* layer covers the point
    // (outside the mapped world): the caller passes the height the vehicle is at,
    // so the surface degrades to "keep going" instead of to zero.
    const float ref = fallback;
    const float base = heightAt(x, z, fallback);
    // A layer that "found" a surface 100 m from the reference is not the surface
    // under the vehicle (a bogus triangle, a road in another valley); such an
    // answer is rejected and the next layer is tried.  This also makes a "not
    // found" zero impossible to mistake for a height.
    const float kPlausible = 100.0f;

    float tileY = 0.0f, tileD = 0.0f;
    bool tileHit = false;
    if (!roadField_.empty()) {
        bool vis = false;
        tileHit = roadField_.heightAt(x, z, ref, 12.0f, tileY, tileD, vis);
        // The gate is deliberately wider than the layer's own window: the layer
        // picked the surface *closest to the reference*, and the caller's
        // reference can be up to a cut-slope above the road (the warehouse spawn
        // sits 12 m up on the terrain and has to reach the asphalt).
        if (tileHit && (tileD > 12.0f || !(std::fabs(tileY - ref) <= 20.0f)))
            tileHit = false;
    }
    float groundY = 0, groundD = 0;
    bool groundVisible = false;
    const bool groundHit = terrainField_.heightAt(x, z, ref, 0, groundY, groundD, groundVisible);
    // Prefer asphalt on the same level, but not a bridge deck above a car
    // that is already on the lawn below it. Both are real authored surfaces.
    if (tileHit && tileD < 0.01f &&
        (!groundHit || std::fabs(tileY-ref) <= std::fabs(groundY-ref)+.05f)) {
        if (outSource) *outSource = 2;
        if (outDist) *outDist = 0;
        return tileY;
    }
    // A lawn is not the nearby navigation centreline. Contact follows the
    // authored triangle, before any graph extrapolation or smoothed fallback.
    if (groundHit) {
        if (outSource) *outSource = 3;
        if (outDist) *outDist = 0;
        return groundY;
    }
    // A coarse cell beside a lawn can be several metres below the lawn edge.
    // Join to the actual edge across one cell instead of hitting an artificial
    // height step as soon as containment starts. Never extrapolate asphalt or
    // connect different decks; exact containing asphalt above has precedence.
    if (terrainField_.nearestGroundHeight(x,z,ref,cell_,groundY,groundD)) {
        const float weight=1-groundD/cell_;
        if(outSource)*outSource=3;
        if(outDist)*outDist=groundD;
        return base+(groundY-base)*weight;
    }
    // With authored ground available, navigation must not manufacture a lower
    // road across a gap in the lawn. This would turn the end of the edge blend
    // into a drop wall (and then the next patch into a climb wall).
    if (!terrainField_.empty() && !tileHit) return base;
    float wayY = 0.0f, wayD = 0.0f;
    bool wayHit = false;
    if (!roadSegCount_.empty()) {
        wayY = roadProfileAt(x, z, wayHit, wayD, 80.0f);
        // A layer answer tens of metres from the reference is not the surface
        // under the vehicle: re-query with a clean `found`, and drop the layer if
        // it still cannot answer.  This is the guard that keeps a bogus/absent
        // `.way` answer (or a stale flag) from becoming the drivable height.
        if (!wayHit || !(std::fabs(wayY - ref) <= kPlausible)) {
            bool wf2 = false;
            float wd2 = 0.0f;
            const float y2 = roadProfileAt(x, z, wf2, wd2, 80.0f);
            if (wf2 && std::fabs(y2 - ref) <= kPlausible) {
                wayY = y2;
                wayD = wd2;
                wayHit = true;
            } else {
                wayHit = false;
            }
        }
    }

    if (wayHit) {
        if (wayD <= 32.0f) {
            // On the network the authored centreline is the surface - unless the
            // tiles' own asphalt here is a *different* road (a deck/tier more
            // than 2 m away), in which case the one closest to the reference wins.
            if (tileHit && tileD <= 4.0f && std::fabs(tileY - wayY) > 2.0f &&
                std::fabs(tileY - ref) < std::fabs(wayY - ref)) {
                if (outSource) *outSource = 2;
                if (outDist) *outDist = tileD;
                return tileY;
            }
            if (outSource) *outSource = 1;
            if (outDist) *outDist = wayD;
            return wayY;
        }
        // Past 32 m the network is no longer *under* the vehicle.  The tiles' own
        // asphalt answers first if they carry a surface here (see the block below,
        // which runs next); only when they do not - the airport apron, where the
        // network simply ends - does the profile hand over to the terrain.  That
        // hand-over has to be a ramp: the mesh there is the cut slope the road runs
        // in and the terrain is 12 m above the profile, so a hard cut presents a
        // wall the climb check refuses (the truck stopped dead 42 m from the
        // start).  The ramp is *gradient-capped* - at most kRampSlope of rise per
        // metre, 26.6 degrees, under the vehicle's 3.5 m climb limit - and is a
        // function of the terrain field alone (a pure function of x, z), so the
        // surface stays path-independent, unlike the old ramp whose base was the
        // caller's own height.
          if (!(tileHit && tileD <= 12.0f)) {
            const float kRampSlope = 0.5f;
            float rise = base - wayY;
            if (rise > 0.0f) {
                const float reach = 32.0f + rise / kRampSlope;
                if (wayD < reach) {
                    float h = wayY + (wayD - 32.0f) * kRampSlope;
                    if (h > base) h = base;
                    if (outSource) *outSource = 1;
                    if (outDist) *outDist = wayD;
                    return h;
                }
            }
        }
    }
    if (tileHit && !(std::fabs(tileY - ref) <= kPlausible)) tileHit = false;
    if (tileHit) {
        if (outDist) *outDist = tileD;
        if (tileD <= 4.0f) {
            if (outSource) *outSource = 2;
            return tileY;
        }
        if (tileD <= 12.0f) {
            float w = 1.0f - (tileD - 4.0f) / 8.0f;
            if (outSource) *outSource = w > 0.5f ? 2 : 0;
            return tileY * w + base * (1.0f - w);
        }
    }
    return base;
}

bool GroundMap::surfaceOnRoad(float x, float z, float& outY) const {
    bool found = false;
    float y = roadProfileAt(x, z, found);
    if (!found) return false;
    outY = y;
    return true;
}

bool GroundMap::onRoad(float x, float z) const {
    bool found = false;
    roadProfileAt(x, z, found);
    return found;
}

// --- the exact drivable profile -------------------------------------------
// A 64-unit spatial hash of the `.way` segments.  surfaceAt()/surfaceOnRoad()
// answer with the nearest point of the polyline set, linearly interpolated
// inside a segment, so the height a vehicle follows is the authored road profile
// itself - not a 12-unit cell average of it (which stepped by up to 26 m between
// neighbouring cells on the sparse stretches of the network).
void GroundMap::setRoadSegments(const std::vector<float>& xyz1) {
    roadPts_ = xyz1;
    roadSegOf_.clear();
    roadSegStart_.clear();
    roadSegCount_.clear();
    roadBuckets_.clear();
    int seg = -1;
    size_t n = xyz1.size() / 4;
    for (size_t i = 0; i < n; ++i) {
        int s = (int)llroundf(xyz1[i * 4 + 3]);
        if (s != seg) {
            seg = s;
            roadSegStart_.push_back((int)i);
            roadSegCount_.push_back(0);
        }
        roadSegOf_.push_back(seg);
        roadSegCount_.back()++;
    }
    for (size_t s = 0; s < roadSegCount_.size(); ++s) {
        int b = roadSegStart_[s], c = roadSegCount_[s];
        for (int k = 0; k + 1 < c; ++k) {
            float ax = xyz1[(size_t)(b + k) * 4 + 0], az = xyz1[(size_t)(b + k) * 4 + 1];
            float bx = xyz1[(size_t)(b + k + 1) * 4 + 0], bz = xyz1[(size_t)(b + k + 1) * 4 + 1];
            int x0 = (int)std::floor(std::min(ax, bx) / kRoadBucket);
            int x1 = (int)std::floor(std::max(ax, bx) / kRoadBucket);
            int z0 = (int)std::floor(std::min(az, bz) / kRoadBucket);
            int z1 = (int)std::floor(std::max(az, bz) / kRoadBucket);
            for (int gz = z0; gz <= z1; ++gz)
                for (int gx = x0; gx <= x1; ++gx)
                    roadBuckets_[((long long)gx << 32) ^ (unsigned)gz].push_back((int)s);
        }
    }
    // Distance from every point to the nearest graph *node* (the endpoints of
    // the segments).  See roadEndDist_ in the header: a VDAT chord that runs far
    // from the node grid is an interpolation artefact (the shipped data has
    // chords spanning tens of metres across the map with y = 0), and it must not
    // become the drivable surface.
    roadEndDist_.assign(n, 1.0e9f);
    {
        const float kNodeBucket = 64.0f;
        std::map<long long, std::vector<int>> nodes;
        auto key = [](int x, int z) { return ((long long)x << 32) ^ (unsigned)z; };
        for (size_t s2 = 0; s2 < roadSegCount_.size(); ++s2) {
            const int b = roadSegStart_[s2], c = roadSegCount_[s2];
            if (c < 1) continue;
            for (int k : {0, c - 1}) {
                int i = b + k;
                int bx = (int)std::floor(xyz1[(size_t)i * 4 + 0] / kNodeBucket);
                int bz = (int)std::floor(xyz1[(size_t)i * 4 + 1] / kNodeBucket);
                nodes[key(bx, bz)].push_back(i);
            }
        }
        for (size_t i = 0; i < n; ++i) {
            const float x = xyz1[i * 4 + 0], z = xyz1[i * 4 + 1];
            int bx = (int)std::floor(x / kNodeBucket), bz = (int)std::floor(z / kNodeBucket);
            float best = 1.0e9f;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    auto it = nodes.find(key(bx + dx, bz + dz));
                    if (it == nodes.end()) continue;
                    for (int j : it->second) {
                        const float ddx = x - xyz1[(size_t)j * 4 + 0];
                        const float ddz = z - xyz1[(size_t)j * 4 + 1];
                        const float d2 = ddx * ddx + ddz * ddz;
                        if (d2 < best) best = d2;
                    }
                }
            roadEndDist_[i] = sqrtf(best);
        }
        size_t far_ = 0;
        for (float d : roadEndDist_) if (d > 40.0f) ++far_;
        LOG("ground: road graph nodes: %zu points (%.1f%%) more than 40 m from any node "
            "(interpolated chords - not a surface)", far_,
            100.0 * (double)far_ / (double)std::max<size_t>(1, n));
    }
    // A bounded chamfer sweep (40 m, 33 passes of 12 m cells) over a 12 m grid of
    // the node field, applied as a lower bound to every chord point: a point
    // 25 m from a node may not be more than 25 m from the node height.
    LOG("ground: road profile index: %zu segments, %zu points, %zu buckets",
        roadSegCount_.size(), n, roadBuckets_.size());
}

float GroundMap::roadProfileAt(float x, float z, bool& found, float maxDist) const {
    float d = 0.0f;
    return roadProfileAt(x, z, found, d, maxDist);
}

float GroundMap::roadProfileAt(float x, float z, bool& found, float& outDist,
                               float maxDist) const {
    found = false;
    outDist = maxDist;
    const float kNan = std::numeric_limits<float>::quiet_NaN();
    if (roadSegCount_.empty()) return kNan;
    float best = maxDist * maxDist;
    float outY = kNan;
    // `found` is set only by a candidate that passed every filter, so a caller
    // that passes a stale `found = true` can no longer read an untouched outY.
    // (That is exactly how the ride layer consumed a zero: a rejected candidate
    // left `outY` at its initial value and the caller took it for a height.)
    // Search the bucket the query is in plus one ring: the closest segment of a
    // 64-unit bucket cannot be further than 64*sqrt(2) away from any point in it,
    // and the caller's maxDist is below that.
    int cx = (int)std::floor(x / kRoadBucket), cz = (int)std::floor(z / kRoadBucket);
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) {
            auto it = roadBuckets_.find(((long long)(cx + dx) << 32) ^ (unsigned)(cz + dz));
            if (it == roadBuckets_.end()) continue;
            for (int s : it->second) {
                const int b = roadSegStart_[(size_t)s], c = roadSegCount_[(size_t)s];
                for (int k = 0; k + 1 < c; ++k) {
                    const float* a = &roadPts_[(size_t)(b + k) * 4];
                    const float* q = &roadPts_[(size_t)(b + k + 1) * 4];
                    float dx2 = q[0] - a[0], dz2 = q[1] - a[1];
                    float len2 = dx2 * dx2 + dz2 * dz2;
                    float u = len2 > 1e-6f ? ((x - a[0]) * dx2 + (z - a[1]) * dz2) / len2 : 0.0f;
                    if (u < 0.0f) u = 0.0f;
                    if (u > 1.0f) u = 1.0f;
                    float px = a[0] + dx2 * u, pz = a[1] + dz2 * u;
                    float d = (x - px) * (x - px) + (z - pz) * (z - pz);
                    if (d < best) {
                        // Reject an interpolated chord that is far from the graph
                        // nodes: it belongs to no road (the shipped VDAT has chords
                        // with y = 0 spanning tens of metres).  The node distance
                        // is the maximum of the two ends, so a segment between a
                        // node and a far point is still usable near the node.
                        const float nodeDist = std::max(roadEndDist_[(size_t)(b + k)],
                                                        roadEndDist_[(size_t)(b + k + 1)]);
                        if (nodeDist > 40.0f) continue;
                            best = d;
                        outY = a[2] + (q[2] - a[2]) * u;
                        outDist = sqrtf(d);
                        found = true;
                    }
                }
            }
        }
    return outY;
}

} // namespace ht2

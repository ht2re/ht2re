#include "GroundMap.h"
#include "Terrain.h"
#include "../util/Log.h"
#include <cmath>
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

    // 1) the road network first: it is the ground the vehicles actually use
    size_t roadCells = 0;
    for (size_t k = 0; k + 2 < roadPts.size(); k += 3) {
        float x = roadPts[k], z = roadPts[k + 1], y = roadPts[k + 2];
        int gx = (int)((x - minX_) / cell_);
        int gz = (int)((z - minZ_) / cell_);
        if (gx < 0 || gz < 0 || gx >= w_ || gz >= h_) continue;
        data_[(size_t)gz * w_ + gx] = y;
        float& m = minY_[(size_t)gz * w_ + gx];
        if (std::isnan(m) || y < m) m = y;
        ++roadCells;
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

    // light smoothing so the drive is not jittery
    next = data_;
    for (int z = 1; z < h_ - 1; ++z)
        for (int x = 1; x < w_ - 1; ++x) {
            float sum = 0;
            int n = 0;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    float v = data_[(size_t)(z + dz) * w_ + (x + dx)];
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

bool GroundMap::waterAt(float x, float z) const {
    if (water_.empty()) return false;
    int gx = (int)((x - minX_) / cell_), gz = (int)((z - minZ_) / cell_);
    if (gx < 0 || gz < 0 || gx >= w_ || gz >= h_) return false;
    return water_[(size_t)gz * w_ + gx] != 0;
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

} // namespace ht2

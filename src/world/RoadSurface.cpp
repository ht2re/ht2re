#include "RoadSurface.h"
#include "../util/Log.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>

namespace ht2 {

namespace {

// Material classification.  Enumerated from the shipped data (tools/road_points.cpp
// style census of original/ENV/*.b3d): the carriageway materials are
// `mat_road`, `mat_road0..4`, `mat_troad`, `mat_troad2..4`, `mat_tunnel_road`,
// `mat_mount_road`, plus `mat_crkroad2` (cracked asphalt) and `mat_roadgrn`
// (the road-family material used on the graded shoulder/verge - kept because it
// is a horizontal surface the tiles draw as part of the carriageway).
// Rejected: `mat_road_fence` (a vertical fence whose name contains "road"),
// anything below a 60-degree slope, and any triangle that does not span at least
// two 2-unit cells (a lane is 12..24 units wide, a tile-sized sliver is not a
// surface).
bool roadMaterial(const std::string& raw, std::string& lower) {
    lower = raw;
    for (char& c : lower) c = (char)tolower((unsigned char)c);
    if (lower.find("fence") != std::string::npos) return false;
    if (lower.find("stolb") != std::string::npos) return false;
    if (lower.find("bridge") != std::string::npos) return false;
    static const char* kAccept[] = {"road", "troad", "asfalt", "asphalt"};
    for (const char* k : kAccept)
        if (lower.find(k) != std::string::npos) return true;
    return false;
}

} // namespace

bool RoadSurface::isRoadMaterial(const std::string& name) {
    std::string lower;
    return roadMaterial(name, lower);
}

bool RoadSurface::isGroundMaterial(const std::string& name) {
    std::string lower = name;
    for (char& c : lower) c = (char)tolower((unsigned char)c);
    // Material alone is not enough: build() also rejects vertical triangles.
    for (const char* word : {"water", "river", "lake", "fence", "wall", "pillar",
                             "stolb", "tree", "forest", "roof", "bshadow", "mark",
                             "poster", "church", "side_beton", "beton_bridge"})
        if (lower.find(word) != std::string::npos) return false;
    if (isRoadMaterial(name)) return true;
    if (lower.rfind("ter_",0)==0 || lower.rfind("mat_ter_",0)==0) return true;
    for (const char* word : {"grass", "ground", "earth", "sand", "dirt",
                             "gravel", "pavement", "sidewalk", "trotoar", "trotuar",
                             "parking", "stoyanka", "beton", "concrete", "apron", "runway"})
        if (lower.find(word) != std::string::npos) return true;
    return false;
}

void RoadSurface::build(const std::vector<B3D::SubMesh>& meshes, bool groundSurfaces) {
    const auto t0 = std::chrono::steady_clock::now();
    tris_.clear();
    grid_ = Grid();
    info_ = Info();
    materials_.clear();

    // Pass 1: collect the horizontal carriageway triangles.
    float minX = 1e30f, maxX = -1e30f, minZ = 1e30f, maxZ = -1e30f;
    for (const auto& sm : meshes) {
        std::string lower;
        if (groundSurfaces) {
            if (!isGroundMaterial(sm.material)) continue;
            lower = sm.material;
            for (char& c : lower) c = (char)tolower((unsigned char)c);
        } else if (sm.material.empty() || !roadMaterial(sm.material, lower)) continue;
        if (std::find(materials_.begin(), materials_.end(), lower) == materials_.end())
            materials_.push_back(lower);
        const std::string matName = lower;
        for (size_t t = 0; t + 2 < sm.idx.size(); t += 3) {
            const uint32_t ia = sm.idx[t], ib = sm.idx[t + 1], ic = sm.idx[t + 2];
            if ((size_t)ia * 3 + 2 >= sm.pos.size() || (size_t)ib * 3 + 2 >= sm.pos.size() ||
                (size_t)ic * 3 + 2 >= sm.pos.size()) continue;
            // b3d (x, z, height) -> world (x, y, z)
            const float* a = &sm.pos[(size_t)ia * 3];
            const float* b = &sm.pos[(size_t)ib * 3];
            const float* c = &sm.pos[(size_t)ic * 3];
            Tri tr;
            tr.ax = a[0]; tr.ay = a[2]; tr.az = a[1];
            tr.bx = b[0]; tr.by = b[2]; tr.bz = b[1];
            tr.cx = c[0]; tr.cy = c[2]; tr.cz = c[1];
            ++info_.triangles;
            // A surface, not a wall: the horizontal extent must dominate the
            // vertical one (a 60-degree slope has |dy| = sqrt(3) * horizontal).
            float ux = tr.bx - tr.ax, uy = tr.by - tr.ay, uz = tr.bz - tr.az;
            float vx = tr.cx - tr.ax, vy = tr.cy - tr.ay, vz = tr.cz - tr.az;
            float nY = uz * vx - ux * vz;            // (u x v).y
            float nX = uy * vz - uz * vy;
            float nZ = ux * vy - uy * vx;
            float len = sqrtf(nX * nX + nY * nY + nZ * nZ);
            if (!(len > 1e-6f)) continue;
            // Natural cut slopes and small lawn patches are still solid ground.
            // Filtering them like carriageway creates holes in the contact field;
            // the vehicle's climb/drop checks decide whether they are driveable.
            if (std::fabs(nY) / len < (groundSurfaces ? 0.087156f : 0.5f)) continue; // ground: <85 deg
            float ex = std::max(std::fabs(ux), std::fabs(vx));
            float ez = std::max(std::fabs(uz), std::fabs(vz));
            if (!groundSurfaces && ex < 2.0f && ez < 2.0f) continue;
            tr.mat = (uint8_t)std::min<size_t>(
                255, (size_t)std::distance(materials_.begin(),
                                           std::find(materials_.begin(), materials_.end(), matName)));
            minX = std::min(minX, std::min(tr.ax, std::min(tr.bx, tr.cx)));
            maxX = std::max(maxX, std::max(tr.ax, std::max(tr.bx, tr.cx)));
            minZ = std::min(minZ, std::min(tr.az, std::min(tr.bz, tr.cz)));
            maxZ = std::max(maxZ, std::max(tr.az, std::max(tr.bz, tr.cz)));
            tris_.push_back(tr);
        }
    }
    if (tris_.empty()) {
        LOG("road-surface: no road-material triangles in the world");
        return;
    }

    // Pass 2: the 32-unit bucket grid.
    Grid& g = grid_;
    g.cell = 32.0f;
    g.minX = minX;
    g.minZ = minZ;
    g.nx = (int)((maxX - minX) / g.cell) + 1;
    g.nz = (int)((maxZ - minZ) / g.cell) + 1;
    if ((double)g.nx * g.nz > 40e6) {                  // paranoia: a bogus bbox
        LOG("road-surface: refusing a %dx%d grid", g.nx, g.nz);
        tris_.clear();
        return;
    }
    std::vector<int> count((size_t)g.nx * g.nz + 1, 0);
    auto bucketOf = [&](float x, float z, int& bx, int& bz) {
        bx = (int)((x - g.minX) / g.cell);
        bz = (int)((z - g.minZ) / g.cell);
        if (bx < 0) bx = 0;
        if (bz < 0) bz = 0;
        if (bx >= g.nx) bx = g.nx - 1;
        if (bz >= g.nz) bz = g.nz - 1;
    };
    for (const Tri& t : tris_) {
        int x0, z0, x1, z1;
        bucketOf(std::min(t.ax, std::min(t.bx, t.cx)), std::min(t.az, std::min(t.bz, t.cz)), x0, z0);
        bucketOf(std::max(t.ax, std::max(t.bx, t.cx)), std::max(t.az, std::max(t.bz, t.cz)), x1, z1);
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) ++count[(size_t)z * g.nx + x];
    }
    g.firstTriangle.assign((size_t)g.nx * g.nz + 1, 0);
    for (size_t i = 0; i < (size_t)g.nx * g.nz; ++i)
        g.firstTriangle[i + 1] = g.firstTriangle[i] + count[i];
    std::vector<int> cursor(g.firstTriangle.begin(), g.firstTriangle.end() - 1);
    g.tri.assign((size_t)g.firstTriangle.back(), 0);
    for (size_t id = 0; id < tris_.size(); ++id) {
        const Tri& t = tris_[id];
        int x0, z0, x1, z1;
        bucketOf(std::min(t.ax, std::min(t.bx, t.cx)), std::min(t.az, std::min(t.bz, t.cz)), x0, z0);
        bucketOf(std::max(t.ax, std::max(t.bx, t.cx)), std::max(t.az, std::max(t.bz, t.cz)), x1, z1);
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) {
                size_t b = (size_t)z * g.nx + x;
                g.tri[cursor[b]++] = (int)id;
            }
    }
    // Pass 3: the same bucket grid over the triangles' horizontal edges, for the
    // horizontal-distance cull of heightAt().
    edges_.clear();
    edges_.reserve(tris_.size() * 4);
    std::vector<std::vector<int>> edgeBuckets((size_t)g.nx * g.nz);
    for (size_t id = 0; id < tris_.size(); ++id) {
        const Tri& t = tris_[id];
        const float vx[3] = {t.ax, t.bx, t.cx};
        const float vz[3] = {t.az, t.bz, t.cz};
        for (int k = 0; k < 3; ++k) {
            const int k2 = (k + 1) % 3;
            int e = (int)(edges_.size() / 4);
            edges_.push_back(vx[k]); edges_.push_back(vz[k]);
            edges_.push_back(vx[k2]); edges_.push_back(vz[k2]);
            int x0, z0, x1, z1;
            bucketOf(std::min(vx[k], vx[k2]), std::min(vz[k], vz[k2]), x0, z0);
            bucketOf(std::max(vx[k], vx[k2]), std::max(vz[k], vz[k2]), x1, z1);
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x) edgeBuckets[(size_t)z * g.nx + x].push_back(e);
        }
    }
    size_t edgeTotal = 0;
    g.firstEdge.assign((size_t)g.nx * g.nz + 1, 0);
    for (size_t i = 0; i < (size_t)g.nx * g.nz; ++i) {
        g.firstEdge[i + 1] = g.firstEdge[i] + (int)edgeBuckets[i].size();
        edgeTotal += edgeBuckets[i].size();
    }
    g.edge.resize(edgeTotal);
    {
        size_t w = 0;
        for (size_t i = 0; i < (size_t)g.nx * g.nz; ++i)
            for (int e : edgeBuckets[i]) g.edge[w++] = e;
    }
    info_.surfaceSlots = g.tri.size();
    info_.bucketed = tris_.size();
    info_.buckets = (size_t)g.nx * g.nz;
    info_.gridCells = (size_t)g.nx * g.nz;
    info_.edgeSlots = g.edge.size();
    info_.edges = edges_.size() / 4;
    info_.tris = tris_.size();
    const auto t1 = std::chrono::steady_clock::now();
    info_.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::string mats;
    for (size_t i = 0; i < materials_.size(); ++i)
        mats += (i ? ", " : "") + materials_[i];
    LOG("road-surface: %zu road triangles (%zu horizontal carriageway) from [%s]",
        info_.triangles, info_.bucketed, mats.c_str());
    LOG("road-surface: %dx%d buckets of %.0f units, %zu triangle slots (%.2f/tri), "
        "%zu edge slots, %zu triangles + %zu edges kept, %.2f MB, built in %.1f ms",
        g.nx, g.nz, (double)g.cell, info_.surfaceSlots,
        info_.bucketed ? (double)info_.surfaceSlots / (double)info_.bucketed : 0.0,
        info_.edgeSlots, info_.tris, info_.edges, info_.bytes() / 1048576.0, info_.ms);
}

bool RoadSurface::nearestGroundHeight(float x, float z, float ref, float maxDist,
                                     float& outY, float& outDist) const {
    if (tris_.empty() || maxDist <= 0) return false;
    const Grid& g=grid_;
    const int x0=std::max(0,(int)std::floor((x-maxDist-g.minX)/g.cell));
    const int x1=std::min(g.nx-1,(int)std::floor((x+maxDist-g.minX)/g.cell));
    const int z0=std::max(0,(int)std::floor((z-maxDist-g.minZ)/g.cell));
    const int z1=std::min(g.nz-1,(int)std::floor((z+maxDist-g.minZ)/g.cell));
    float best=maxDist*maxDist, bestRef=1e30f;
    bool found=false;
    for(int iz=z0;iz<=z1;++iz)for(int ix=x0;ix<=x1;++ix) {
        const size_t bin=(size_t)iz*g.nx+ix;
        for(int k=g.firstTriangle[bin];k<g.firstTriangle[bin+1];++k) {
            const auto& t=tris_[(size_t)g.tri[(size_t)k]];
            if(t.mat>=materials_.size()||isRoadMaterial(materials_[t.mat]))continue;
            const float p[3][3]={{t.ax,t.ay,t.az},{t.bx,t.by,t.bz},{t.cx,t.cy,t.cz}};
            for(int e=0;e<3;++e) {
                const auto& a=p[e];const auto& b=p[(e+1)%3];
                const float dx=b[0]-a[0],dz=b[2]-a[2],len=dx*dx+dz*dz;
                const float u=len>1e-9f?std::max(0.f,std::min(1.f,((x-a[0])*dx+(z-a[2])*dz)/len)):0;
                const float ex=x-a[0]-u*dx,ez=z-a[2]-u*dz,d=ex*ex+ez*ez;
                const float y=a[1]+u*(b[1]-a[1]),dy=std::fabs(y-ref);
                if(dy>20)continue; // do not connect to a different vertical tier
                if(d<best-1e-5f || (std::fabs(d-best)<=1e-5f&&dy<bestRef)) {
                    best=d;bestRef=dy;outY=y;found=true;
                }
            }
        }
    }
    if(found)outDist=std::sqrt(best);
    return found;
}

int RoadSurface::surfacesAt(float x, float z, float maxDist, std::vector<float>& out) const {
    out.clear();
    if (tris_.empty() || grid_.nx <= 0) return 0;
    const Grid& g = grid_;
    int x0 = (int)((x - maxDist - g.minX) / g.cell);
    int x1 = (int)((x + maxDist - g.minX) / g.cell);
    int z0 = (int)((z - maxDist - g.minZ) / g.cell);
    int z1 = (int)((z + maxDist - g.minZ) / g.cell);
    if (x0 < 0) x0 = 0;
    if (z0 < 0) z0 = 0;
    if (x1 >= g.nx) x1 = g.nx - 1;
    if (z1 >= g.nz) z1 = g.nz - 1;
    for (int bz = z0; bz <= z1; ++bz)
        for (int bx = x0; bx <= x1; ++bx) {
            const size_t b = (size_t)bz * g.nx + bx;
            for (int i = g.firstTriangle[b]; i < g.firstTriangle[b + 1]; ++i) {
                const Tri& t = tris_[(size_t)g.tri[(size_t)i]];
                const float e = (t.bx - t.ax) * (t.cz - t.az) - (t.bz - t.az) * (t.cx - t.ax);
                if (std::fabs(e) < 1e-9f) continue;
                const float w0 = ((t.bx - x) * (t.cz - z) - (t.bz - z) * (t.cx - x)) / e;
                const float w1 = ((t.cx - x) * (t.az - z) - (t.cz - z) * (t.ax - x)) / e;
                const float w2 = 1.0f - w0 - w1;
                if (!((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0))) continue;
                const float y = w0 * t.ay + w1 * t.by + w2 * t.cy;
                bool dup = false;
                for (float v : out) if (std::fabs(v - y) < 0.1f) { dup = true; break; }
                if (!dup) out.push_back(y);
            }
        }
    std::sort(out.begin(), out.end(), std::greater<float>());
    return (int)out.size();
}

// Horizontal distance from (x, z) to the nearest road triangle edge (0 when the
// point is inside one).  Uses the same bucket grid, one ring around the query.
float RoadSurface::nearestEdgeDist(float x, float z, float maxDist) const {
    const Grid& g = grid_;
    if (g.edge.empty()) return maxDist + 1.0f;
    int bx = (int)((x - g.minX) / g.cell);
    int bz = (int)((z - g.minZ) / g.cell);
    bx = std::max(0, std::min(g.nx - 1, bx));
    bz = std::max(0, std::min(g.nz - 1, bz));
    float best = maxDist * maxDist + 1.0f;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) {
            const int cx = bx + dx, cz = bz + dz;
            if (cx < 0 || cz < 0 || cx >= g.nx || cz >= g.nz) continue;
            const size_t b = (size_t)cz * g.nx + cx;
            for (int i = g.firstEdge[b]; i < g.firstEdge[b + 1]; ++i) {
                const float* e = &edges_[(size_t)g.edge[(size_t)i] * 4];
                const float ex = e[2] - e[0], ez = e[3] - e[1];
                const float len2 = ex * ex + ez * ez;
                float u = len2 > 1e-9f ? ((x - e[0]) * ex + (z - e[1]) * ez) / len2 : 0.0f;
                if (u < 0.0f) u = 0.0f;
                if (u > 1.0f) u = 1.0f;
                const float px = e[0] + ex * u, pz = e[1] + ez * u;
                const float d2 = (x - px) * (x - px) + (z - pz) * (z - pz);
                if (d2 < best) best = d2;
            }
        }
    return sqrtf(best);
}

bool RoadSurface::heightAt(float x, float z, float ref, float maxDist,
                           float& outY, float& outDist, bool& outVisible) const {
    outVisible = true;
    if (tris_.empty() || grid_.nx <= 0) return false;
    const Grid& g = grid_;
    // Two candidates are tracked at once:
    //   `near`  the surface within `tol` of `ref` (the road the vehicle is on),
    //   `any`   the surface closest to `ref` when none is that close.
    // The first is what makes a viaduct deck and the carriageway under it both
    // answerable; the second is the fallback for a reference that is off (a
    // spawn, a teleport, a vehicle that wandered onto unmapped ground).  Without
    // it the layer answers "nothing" there and the ride drops to the terrain.
    // The vertical window a surface must be in to be "the road under the
    // vehicle".  Twelve metres is a full viaduct clearance and ten times any road
    // grade; the *safety* against picking up a surface that is not the road is
    // not this window but the reference the caller passes (the height it is
    // riding at, or the `.way` link's own height) plus the plausibility gate in
    // GroundMap::drivableY().  A tighter 4 m window broke the warehouse case: the
    // spawn there stands on the terrain cut 12 m above the carriageway and never
    // reached the asphalt.
    const float tol = 12.0f;
    float nearY = 0.0f, anyY = 0.0f;
    float nearD = tol, anyD = 1e30f;
    // maxDist bounds the bucket search horizontally; actual hits still require
    // triangle containment. The fallback has a separate vertical limit below.
    bool got = false;
    float topY = -1e30f;
    int x0 = (int)((x - maxDist - g.minX) / g.cell);
    int x1 = (int)((x + maxDist - g.minX) / g.cell);
    int z0 = (int)((z - maxDist - g.minZ) / g.cell);
    int z1 = (int)((z + maxDist - g.minZ) / g.cell);
    if (x0 < 0) x0 = 0;
    if (z0 < 0) z0 = 0;
    if (x1 >= g.nx) x1 = g.nx - 1;
    if (z1 >= g.nz) z1 = g.nz - 1;
    if (x0 > x1 || z0 > z1) return false;
    for (int bz = z0; bz <= z1; ++bz)
        for (int bx = x0; bx <= x1; ++bx) {
            const size_t b = (size_t)bz * g.nx + bx;
            for (int i = g.firstTriangle[b]; i < g.firstTriangle[b + 1]; ++i) {
                const Tri& t = tris_[(size_t)g.tri[(size_t)i]];
                const float e = (t.bx - t.ax) * (t.cz - t.az) - (t.bz - t.az) * (t.cx - t.ax);
                if (std::fabs(e) < 1e-9f) continue;
                const float w0 = ((t.bx - x) * (t.cz - z) - (t.bz - z) * (t.cx - x)) / e;
                const float w1 = ((t.cx - x) * (t.az - z) - (t.cz - z) * (t.ax - x)) / e;
                const float w2 = 1.0f - w0 - w1;
                if (!((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0))) continue;
                const float y = w0 * t.ay + w1 * t.by + w2 * t.cy;
                if (y > topY) topY = y;
                const float d = std::fabs(y - ref);
                if (d < nearD) { nearD = d; nearY = y; got = true; }
                if (d < anyD) { anyD = d; anyY = y; }
            }
        }
    // A spawn reference may be on the terrain cut rather than the road. Keep
    // the fallback bounded vertically; maxDist is horizontal, never squared
    // into a height threshold.
    if (!got) {
        if (anyD > 20.0f) return false;
        nearY = anyY;
    }
    // Barycentric containment above already proves horizontal distance ZERO.
    // Distance to an edge is not distance to a filled triangle: on a wide
    // apron it previously rejected its centre or blended it with terrain.
    outY = nearY;
    outDist = 0.0f;
    outVisible = !(topY > outY + 1.0f);
    return true;
}

} // namespace ht2

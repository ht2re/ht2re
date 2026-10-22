#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <array>
#include <limits>
#include <set>
#include <string>
#include <unordered_map>
#include "../world/B3D.h"

namespace ht2 {

// Solid-geometry collision for the drivable world.
//
// The ENV tiles are the only source of truth for what is solid: buildings,
// walls, fences, poles, containers, trees.  Submeshes are filtered by material
// and by "is this a wall or a surface?" (see the .cpp for the exact rule), then
// the remaining triangles are rasterised into a uniform bit grid so a query is
// O(1) in the world size: a lookup of the cells the circle overlaps.
//
// Coordinates are the B3D ones used by B3D::SubMesh: pos = (x, z, height).
// Legacy queries use the 2D occupancy grid. Height-aware circle/sweep queries
// clip original triangles to the body's vertical slab, using 32m spatial bins.
// Player driving supplies the road height and loaded model's bbox height.
//
// A body is a **capsule**: a segment of `length - 2 * radius` along the vehicle
// heading with radius `radius` (i.e. the rectangle of the car plus its corner
// rounding).  The single-circle helpers stay for round obstacles; blockedBody()
// / resolveBody() sweep the whole capsule.
class Collision {
public:
    // A car body in metres: `radius` = half width, `length` = bumper to bumper.
    struct Body {
        float radius = 1.0f;
        float length = 4.5f;
        float halfAxis() const { return std::max(0.0f, 0.5f * (length - 2.0f * radius)); }
    };

    // `meshes` are world submeshes in b3d order (x, z, height) - the same list
    // B3D::buildFrom produces and the renderer draws.
    void build(const std::vector<B3D::SubMesh>& meshes);
    // Same, with an explicit grid cell size (default kDefaultCellSize metres).
    // `skipGroups`, when set, drops those submesh groups: an ambush roadblock
    // stays in the world list while it is hidden, and must not keep blocking.
    void build(const std::vector<B3D::SubMesh>& meshes, float cellSize,
               const std::set<std::string>* skipGroups = nullptr);
    // Build from the authored collision proxies (docs/hit_proxies.md).  A proxy's
    // footprint polygon IS the authored solid shape of its object, so the rendered
    // mesh of that object must not block again (same obstacle, drawn): a submesh
    // whose centre lies inside such a polygon is skipped.  Everything not covered
    // by a proxy keeps the visible-mesh rule, so the world never gets holes.
    // The mesh grid above is the fallback; the authored `hit_*` surfaces are exact
    // and are added on top (docs/hit_proxies.md, king.exe perform_wall_collision).
    void build(const std::vector<B3D::SubMesh>& meshes,
               const std::vector<B3D::HitProxy>& proxies);
    void setWalls(const std::vector<B3D::HitProxy>& proxies);
    size_t wallCount() const { return authoredWalls_.size(); }
    // Does the segment (fromX,fromZ) -> (toX,toZ) cross an authored wall whose
    // height band overlaps [bottom, top]?  On a hit outX/outZ hold the contact point.
    bool crossedWall(float fromX, float fromZ, float toX, float toZ,
                     float bottom, float top, float& outX, float& outZ) const;
    void clear();
    bool empty() const { return bits_.empty(); }
    size_t cells() const { return solidCells_; }
    float cellSize() const { return cell_; }
    float minX() const { return x0_; }
    float minZ() const { return z0_; }
    int width() const { return nx_; }
    int height() const { return nz_; }

    // Is the circle (x, z, r) overlapping solid geometry?
    bool blocked(float x, float z, float r,
                 float bottom = -std::numeric_limits<float>::infinity(),
                 float top = std::numeric_limits<float>::infinity()) const;

    // Try to move from (fromX,fromZ) to (toX,toZ) for a body of radius `r`.
    // Returns true when the destination is free; otherwise `outX/outZ` hold the
    // furthest free point along the segment (slide, no penetration).
    bool resolveMove(float fromX, float fromZ, float toX, float toZ, float r,
                     float& outX, float& outZ,
                     float bottom = -std::numeric_limits<float>::infinity(),
                     float top = std::numeric_limits<float>::infinity()) const;

    // --- capsules (vehicles) -----------------------------------------------
    // The two ends of a body's axis, `yaw` in degrees with forward = (sin, cos)
    // (the Vehicle/Renderer convention).
    static void bodyAxis(float x, float z, float yawDeg, const Body& b,
                         float& ax, float& az, float& bx, float& bz);
    // Does the whole capsule overlap solid geometry?
    bool blockedBody(float x, float z, float yawDeg, const Body& b) const;
    // Sweep + slide the capsule from one pose to another; false when it hit
    // something (`outX/outZ` = the furthest free centre position).
    bool resolveBody(float fromX, float fromZ, float fromYaw,
                     float toX, float toZ, float toYaw, const Body& b,
                     float& outX, float& outZ) const;
    // Capsule-vs-capsule contact: the overlap (> 0 when the bodies touch) and
    // the unit normal pointing from A to B.  Used for car-vs-car and
    // car-vs-player contacts.
    static float bodyOverlap(float ax, float az, float ayawDeg, const Body& a,
                             float bx, float bz, float byawDeg, const Body& b,
                             float& nx, float& nz);

    // --- build statistics (logging / tools) --------------------------------
    struct Stats {
        size_t submeshes = 0;   // submeshes handed to build()
        size_t ground = 0;      // skipped: drivable surface / decal material
        size_t flat = 0;        // skipped: flatter than the decal threshold
        size_t triangles = 0;   // triangles rasterised
        size_t cells = 0;       // solid cells after rasterisation
        size_t gridCells = 0;   // nx * nz (bit grid size)
    };
    const Stats& stats() const { return stats_; }

    // Test hook: is the grid cell (ix, iz) solid?  False outside the grid.
    bool solidCell(int ix, int iz) const;

    static float defaultCellSize();

private:
    bool bit(size_t i) const { return (bits_[i >> 6] >> (i & 63)) & 1u; }
    void setBit(size_t i) { bits_[i >> 6] |= (uint64_t(1) << (i & 63)); }
    bool cellSolid(int ix, int iz) const {
        return (unsigned)ix < (unsigned)nx_ && (unsigned)iz < (unsigned)nz_ &&
               bit((size_t)ix * (size_t)nz_ + (size_t)iz);
    }
    // furthest free t in [0,1] along from->to (0 when `from` is blocked)
    float sweep(float fromX, float fromZ, float toX, float toZ, float r,
                float bottom, float top) const;
    bool contactNormal(float x, float z, float r, float& outNx, float& outNz,
                       float bottom = -std::numeric_limits<float>::infinity(),
                       float top = std::numeric_limits<float>::infinity()) const;
    bool blockedHeight(float x, float z, float r, float bottom, float top) const;
    static uint64_t wallKey(int x, int z) {
        return (uint64_t(uint32_t(x)) << 32) | uint32_t(z);
    }
    std::vector<std::array<float,9>> walls_; // B3D x,z,height triangles
    std::unordered_map<uint64_t,std::vector<size_t>> wallBuckets_; // 32m bins
    std::vector<size_t> wideWalls_; // pathological large footprints, no huge bin allocation

    std::vector<uint64_t> bits_;    // 1 = solid cell, ix * nz_ + iz
    // One authored thin wall: a vertical plane with a finite extent and a bottom.
    struct AuthoredWall {
        float nx = 0, nz = 0, d = 0;   // normalised XZ normal and offset
        float x0 = 0, x1 = 0, z0 = 0, z1 = 0;   // extent bounding box
        std::vector<float> ring;       // extent polygon, x, z, y triplets
        float hLo = 0, hHi = 0;        // height band (bottom .. top of the extent)
    };
    std::vector<AuthoredWall> authoredWalls_;
    void recountSolids();
    float cell_ = 1.0f, x0_ = 0.0f, z0_ = 0.0f;
    int nx_ = 0, nz_ = 0;
    size_t solidCells_ = 0;
    Stats stats_;
};

} // namespace ht2

#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include <cstddef>
#include <cstdint>
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
// The simulated world is two-dimensional (x, z) -- there is no height in the
// query, so an overpass deck does not block the road below it and only the
// wall-like geometry around a driveway blocks it.
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
    void build(const std::vector<B3D::SubMesh>& meshes, float cellSize);
    void clear();
    bool empty() const { return bits_.empty(); }
    size_t cells() const { return solidCells_; }
    float cellSize() const { return cell_; }
    float minX() const { return x0_; }
    float minZ() const { return z0_; }
    int width() const { return nx_; }
    int height() const { return nz_; }

    // Is the circle (x, z, r) overlapping solid geometry?
    bool blocked(float x, float z, float r) const;

    // Try to move from (fromX,fromZ) to (toX,toZ) for a body of radius `r`.
    // Returns true when the destination is free; otherwise `outX/outZ` hold the
    // furthest free point along the segment (slide, no penetration).
    bool resolveMove(float fromX, float fromZ, float toX, float toZ, float r,
                     float& outX, float& outZ) const;

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
    float sweep(float fromX, float fromZ, float toX, float toZ, float r) const;
    bool contactNormal(float x, float z, float r, float& outNx, float& outNz) const;

    std::vector<uint64_t> bits_;    // 1 = solid cell, ix * nz_ + iz
    float cell_ = 1.0f, x0_ = 0.0f, z0_ = 0.0f;
    int nx_ = 0, nz_ = 0;
    size_t solidCells_ = 0;
    Stats stats_;
};

} // namespace ht2

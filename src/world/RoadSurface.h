#pragma once
// RoadSurface - a drivable-height index built from the shipped tiles' own road
// triangles (`mat_road*`, see seed() for the exact list and the exclusions).
//
// Why it exists: the `.way` road graph is the authored centreline profile and
// agrees with the asphalt to a 0.000 m median (docs/terrain_road.md §8), but it
// stops short of a few driveable tiles - at the aa warehouse the carriageway
// continues to z = -4575 while the network ends at z = -4615.  There the terrain
// field takes over, and it is a *cut* several metres below the asphalt, so a
// vehicle that follows it sinks.  This index answers "what does the asphalt
// under me say" everywhere the tiles carry road geometry.
//
// Structure: the road triangles are bucketed into a uniform 2D grid (32-unit
// buckets, ~1.5 triangles per bucket) and a point query intersects the triangles
// of its bucket.  That is O(1) per query and costs ~50 k triangles of memory
// (~1 MB) instead of a rasterised heightfield: a 1-unit grid over the island
// would be 10^8 cells, 25,000x more memory, for a surface that is already
// piecewise planar in the tiles.
//
// Every surface in a column is collected, not just the top one, so a car on a
// viaduct and a car on the road below it each get their own deck: pick() prefers
// the surface closest to a reference height (the vehicle's current height / the
// `.way` link's height) and only falls back to the highest when both are far.
#include <cstdint>
#include <utility>
#include <vector>
#include "../world/B3D.h"

namespace ht2 {

class RoadSurface {
public:
    // One planar road triangle: world (x, y, z) plus its material id.
    struct Tri {
        float ax, ay, az, bx, by, bz, cx, cy, cz;
        uint8_t mat = 0;
    };
    // A bucket can hold the same triangle twice (its bounding box spans several
    // buckets); the fixed-size lists keep that visible to the query, which uses
    // firstTriangle[b] .. firstTriangle[b+1].
    //
    // `edge` holds the triangle's three horizontal edges bucketed the same way:
    // the vertical window alone is not enough to decide that a road is "here"
    // (a ground-level asphalt quad tens of metres away was picked up as the
    // surface under a road at y = 20 and the ride dropped to 0), so heightAt()
    // culls the candidates by their horizontal distance too.
    struct Grid {
        float minX = 0, minZ = 0, cell = 32.0f;
        int nx = 0, nz = 0;
        std::vector<int> firstTriangle;   // nx*nz + 1 offsets
        std::vector<int> tri;             // bucket -> triangle ids
        std::vector<int> firstEdge;       // nx*nz + 1 offsets
        std::vector<int> edge;            // bucket -> edge ids
    };

    struct Info {
        size_t triangles = 0;             // all road-material triangles seen
        size_t bucketed = 0;              // horizontal carriageway triangles kept
        size_t buckets = 0;
        size_t slots = 0;                 // triangle ids stored (with duplicates)
        size_t cells = 0;                 // grid cells with at least one surface
        size_t tier2 = 0;                 // cells holding a second surface (a tier)
        double ms = 0.0;                  // build time
        size_t bytes() const {
            return (slots + edgeSlots) * sizeof(int) + (gridCells + 1) * 2 * sizeof(int) +
                   tris * sizeof(Tri) + edges * 4 * sizeof(float);
        }
        size_t gridCells = 0, surfaceSlots = 0;
        size_t edgeSlots = 0, edges = 0;
        size_t tris = 0;
    };

    // True for a material the shipped tiles use for a carriageway surface (see
    // the .cpp for the enumerated list and the exclusions).  Shared with the
    // measurement probes so "asphalt" means the same thing everywhere.
    static bool isRoadMaterial(const std::string& name);
    static bool isGroundMaterial(const std::string& name);
    void build(const std::vector<B3D::SubMesh>& meshes, bool groundSurfaces = false);
    bool empty() const { return tris_.empty(); }
    const Info& info() const { return info_; }
    // Material names accepted / rejected, for the report.
    const std::vector<std::string>& roadMaterials() const { return materials_; }
    // The kept carriageway triangles (measurement only: the probes validate the
    // layered surface against the tiles' own asphalt).
    const std::vector<Tri>& triangles() const { return tris_; }

    // Every distinct asphalt surface in the (x, z) column (a viaduct deck and
    // the carriageway under it are two), written to `out` (descending).  This is
    // the primitive: it makes "which surface belongs to the road the vehicle is
    // on" a caller question (continuity with the current height), instead of
    // baking a guess into the index.
    int surfacesAt(float x, float z, float maxDist, std::vector<float>& out) const;

    // Height of the asphalt surface under (x, z) closest to `ref`, with the
    // horizontal distance to the nearest road triangle that provided it.
    // `visible` is true when the surface is not fully occluded by another road
    // surface more than 1 m above it (a viaduct deck hides the carriageway below
    // it in the picture, but the height is still reported: the caller decides).
    // Returns false when the column has no road triangle within `maxDist` of the
    // query point.
    bool heightAt(float x, float z, float ref, float maxDist,
                  float& outY, float& outDist, bool& outVisible) const;
    // Nearest point on a carriageway triangle. Used when a lane sample has
    // left the asphalt (the shoulder, a junction chord) so the body can be
    // put back on the surface that is actually drawn. `maxHoriz` is the XZ
    // search radius, `maxVert` rejects a deck whose height is far from `ref`.
    bool closestPoint(float x, float z, float ref, float maxHoriz, float maxVert,
                      float& outX, float& outZ, float& outY) const;
    // Closest point on a non-asphalt ground triangle, for the narrow boundary
    // hand-over where the generated terrain fills gaps between authored patches.
    bool nearestGroundHeight(float x, float z, float ref, float maxDist,
                             float& outY, float& outDist) const;
    // Drop authored grass triangles that lie on the carriageway. The shoulder
    // outside the road mesh is left alone. `mat_roadgrn` is not grass.
    void cutGrassOffCarriageway(std::vector<B3D::SubMesh>& meshes) const;

private:
    // Horizontal distance to the nearest road triangle edge (maxDist + 1 when
    // there is none within maxDist).
    float nearestEdgeDist(float x, float z, float maxDist) const;
    std::vector<Tri> tris_;
    std::vector<float> edges_;             // 4 floats per edge: ax, az, bx, bz
    Grid grid_;
    Info info_;
    std::vector<std::string> materials_;
};

} // namespace ht2

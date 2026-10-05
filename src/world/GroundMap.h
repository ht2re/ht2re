#pragma once
#include <map>
#include <vector>
#include <limits>
#include "B3D.h"
#include "RoadSurface.h"

namespace ht2 {

class Terrain;

// Drivable ground for the assembled world.
//
// The shipped `terrain*.raw` does NOT model the ENV ground: against all 762
// `.way` node heights of the 40 world tiles it correlates at r = 0.25..0.41 for
// every horizontal scale (20..200 units/sample), while the ENV geometry itself
// is an (almost) flat plane — e.g. every aa room road quad sits at exactly
// h = 447.059 and the props at h = 450, i.e. the `.way` height.  Verified by the
// independent check in docs/b3d_transform.md.
//
// So the ground is derived from the world geometry itself: every cell keeps the
// LOWEST vertex height inside it (buildings stand on the ground, their roofs are
// above it), holes are filled from neighbours and the field is lightly smoothed.
class GroundMap {
public:
    // `roadPts` = x,y,z triples from the .way road network (authoritative: the
    // road graph heights match the tile geometry exactly, verified against
    // STO_AA = 450 in ENV/aa.way).
    // `base` (optional) supplies the height for cells with no road point and no
    // geometry; without it such cells are filled from their neighbours.
    void build(const std::vector<B3D::SubMesh>& meshes,
               const std::vector<float>& roadPts = {},
               float cellSize = 12.0f,
               const Terrain* base = nullptr);
    // The road network as *segments* (x, z, y per vertex, 1 = segment break).
    // The cell-averaged `road_` field is then only the coarse terrain fallback;
    // the drivable profile answered by surfaceAt()/surfaceOnRoad() is the exact
    // nearest point of this polyline set, so it has no 12-unit quantisation.
    void setRoadSegments(const std::vector<float>& xyz1);
    // Authored asphalt triangles take precedence wherever they cover a point;
    // the graph profile is the fallback when no actual road surface is found.
    void buildRoadSurface(const std::vector<B3D::SubMesh>& meshes);
    const RoadSurface& roadSurface() const { return roadField_; }
    // One entry point: containing asphalt, else graph profile/ramp, else terrain.
    // `outSource` is 0 = terrain, 1 = `.way`, 2 = tile road.
    float drivableY(float x, float z, float fallback, int* outSource = nullptr,
                    float* outDist = nullptr) const;
    bool empty() const { return w_ <= 0 || h_ <= 0 || data_.empty(); }
    // world height of the ground, or `fallback` when the map has no data there
    float heightAt(float x, float z, float fallback = 0.0f) const;
    // world height of the *drivable road surface* at (x, z), or `fallback` when
    // the point is not on a road.  This is the road network's own profile
    // (every `.way` RSEG polyline point, averaged per cell and taken from the
    // nearest road cell), which is what the tile geometry carries and what the
    // AI vehicles' `Car::h` comes from - so a vehicle that follows `heightAt()`
    // (which is a *terrain* field, smoothed and mixed with the off-road ground
    // in the same cell) can sit several units off the asphalt.  See
    // docs/terrain_road.md "why the cars left the road".
    float surfaceAt(float x, float z, float fallback = 0.0f) const;
    // Same, with a validity flag: true only when the point is close enough to a
    // road cell for its height to be the carriageway (GroundMap::onRoad()).
    bool surfaceOnRoad(float x, float z, float& outY) const;
    // Exact drivable profile: the nearest `.way` polyline point, interpolated.
    // The search runs over a 64-unit bucket grid, so it is independent of the
    // number of segments (a few candidate segments per query).
    float roadProfileAt(float x, float z, bool& found, float maxDist = 32.0f) const;
    // Same, with the distance to the nearest road point.  `maxDist` bounds the
    // search: outside it `found` is false (a road 100 m away is not the road
    // under the wheels).
    float roadProfileAt(float x, float z, bool& found, float& outDist,
                        float maxDist = 32.0f) const;
    bool onRoad(float x, float z) const;
    size_t roadCells() const { return roadCells_; }

    // --- tile-surface ceiling ----------------------------------------------
    // A field the generated terrain mesh must stay below.  It is built from the
    // *coverage* of the tile triangles, not just their vertices (a 1-km apron
    // quad has four vertices and would otherwise be "covered" in four cells):
    //
    //   minY_[cell]    lowest non-water tile surface over the cell centre,
    //                  seeded from both triangle coverage and vertices
    //   clear_[cell]   minY_ dilated by CLEARANCE_RADIUS cells (3x3 min), so the
    //                  clamp covers a triangle's whole footprint and not only the
    //                  cells its vertices land in
    //   dist_[cell]    distance to the nearest covered cell (chamfer transform)
    //   near_[cell]    the tile surface of that nearest covered cell
    //
    // `ceilingAt()` hands both to the caller, which ramps the clamp from the
    // tile surface up to the raw heightmap over CEILING_FADE units.  Without the
    // ramp the clamp ends in a hard straight edge across the landscape; with a
    // *constant slope* instead of a ramp it ends in a wall next to every road.
    // See docs/terrain_road.md for the frames.
    bool ceilingAt(float x, float z, float& nearY, float& dist) const;
    bool hasClearance() const { return !near_.empty(); }
    static constexpr int CLEARANCE_RADIUS = 3;     // cells (~36 units at 12)
    static constexpr float CEILING_FADE = 450.0f;  // world units to reach the raw heightmap
    // Sea / lake planes are not drivable: the tiles carry big water quads whose
    // height is the sea level, which would otherwise become ground.
    bool waterAt(float x, float z, float roadHeight = std::numeric_limits<float>::quiet_NaN()) const;
    // Is the point inside the mapped world at all?  Outside it there is no
    // ground data, so the vehicle must be stopped at the edge.
    bool inside(float x, float z) const;
    int width() const { return w_; }
    int height() const { return h_; }
    float cellSize() const { return cell_; }
    float minX() const { return minX_; }
    float minZ() const { return minZ_; }
    float maxX() const { return minX_ + (w_ - 1) * cell_; }
    float maxZ() const { return minZ_ + (h_ - 1) * cell_; }
    float minHeight() const { return hmin_; }
    float maxHeight() const { return hmax_; }
    size_t filledCells() const { return filled_; }
    const std::vector<float>& cells() const { return data_; }

private:
    int w_ = 0, h_ = 0;
    float minX_ = 0, minZ_ = 0, cell_ = 12.0f;
    float hmin_ = 0, hmax_ = 0;
    size_t filled_ = 0;
    std::vector<float> data_;
    std::vector<float> minY_;      // per cell: lowest tile surface (NaN = none)
    std::vector<float> clear_;     // minY_ dilated by CLEARANCE_RADIUS cells
    std::vector<float> near_;      // tile surface at the nearest covered cell
    std::vector<float> dist_;      // distance to the nearest covered cell
    // The road network's own profile, kept apart from `data_` because that one
    // is a *terrain* field: the road cells are averaged per cell and the
    // smoothing pass would otherwise drag them towards the off-road ground
    // (and vice versa) inside the same 12-unit cell.
    std::vector<float> road_;      // per cell: mean road height (NaN = no road)
    std::vector<float> roadDist_;  // distance to the nearest road cell
    std::vector<uint8_t> water_;   // 1 = sea / lake surface   // NAN = no sample
    size_t roadCells_ = 0;
    // The road network itself: `roadPts_` is a flat (x, z, y) stream, `roadIdx_`
    // the segment id of every point (a segment ends where the id changes), and
    // `roadBuckets_` a 64-unit spatial hash of the segment ids.
    std::vector<float> roadPts_;
    // Distance from each point to the nearest node of the graph (0 at a node):
    // the `.way` VDAT keeps straight chords across a polyline, so a point whose
    // chord is far from the graph is an interpolation artefact, not a road.
    std::vector<float> roadEndDist_;
    std::vector<int> roadSegOf_;
    std::vector<int> roadSegStart_;
    std::vector<int> roadSegCount_;
    static constexpr float kRoadBucket = 64.0f;
    std::map<long long, std::vector<int>> roadBuckets_;
    void bucketSegment(int seg, float ax, float az, float bx, float bz);
    RoadSurface roadField_;
};

} // namespace ht2

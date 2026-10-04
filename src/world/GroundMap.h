#pragma once
#include <vector>
#include "B3D.h"

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
    bool empty() const { return w_ <= 0 || h_ <= 0 || data_.empty(); }
    // world height of the ground, or `fallback` when the map has no data there
    float heightAt(float x, float z, float fallback = 0.0f) const;

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
    bool waterAt(float x, float z) const;
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
    std::vector<uint8_t> water_;   // 1 = sea / lake surface   // NAN = no sample
};

} // namespace ht2

#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace ht2 {

// terrain*.raw: 66049 uint16 LE samples (257x257 grid), high byte == low byte,
// height = low byte (8-bit).
//
// The mapping to world coordinates is NOT the naive grid-centred one.  Fitting
// every one of the 762 `.way` road node heights of the 40 ENV tiles against the
// samples gives (see NOTES.md "terrain mapping"):
//
//     i = x / 415 + 162        j = -z / 415 + 126
//     worldHeight = 2.58 * byte - 157.1
//
// with r = 0.980 and a residual standard deviation of 20.6 world units over a
// 0..450 range.  The z axis is mirrored and the sample step is 415 units, so the
// island only occupies a ~24x27 patch of the map (i 151..174, j 114..140).
class Terrain {
public:
    static constexpr int GRID = 257;   // samples per side
    static constexpr int CELLS = 256;  // quads per side

    float h[GRID * GRID] = {0};        // raw samples (0..255)
    float hmin = 0.0f, hmax = 0.0f;    // sample range (after load)

    float sampleStep = 415.0f;         // world units per sample
    float centreI = 162.0f, centreJ = 126.0f;
    float heightScale = 2.58f;
    float heightOff = -157.1f;

    bool load(const std::string& path);
    bool loaded() const { return !raw_.empty(); }

    float worldY(float t) const { return heightOff + heightScale * t; }
    float sampleX(int i) const { return (i - centreI) * sampleStep; }
    float sampleZ(int j) const { return (centreJ - j) * sampleStep; }
    // bilinear world height, `fallback` outside the map
    float heightAtWorld(float x, float z, float fallback = 0.0f) const;
    // 0..1 coverage test
    bool covers(float x, float z) const;

private:
    std::vector<uint8_t> raw_;
};

} // namespace ht2

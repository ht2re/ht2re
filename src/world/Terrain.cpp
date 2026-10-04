#include "Terrain.h"
#include "../util/Log.h"
#include <cstdio>
#include <cstring>
#include <cmath>

namespace ht2 {

bool Terrain::load(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { LOG("terrain: cannot open %s", path.c_str()); return false; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz != (long)(GRID * GRID * 2)) {
        LOG("terrain: unexpected size %ld (want %d)", sz, GRID * GRID * 2);
        fclose(f);
        return false;
    }
    raw_.resize((size_t)GRID * GRID);
    // sample16 -> (sample16 & 0xFF); the two bytes of every sample are equal
    hmin = 1e9f; hmax = -1e9f;
    for (size_t k = 0; k < raw_.size(); ++k) {
        uint8_t b[2];
        if (fread(b, 1, 2, f) != 2) { fclose(f); return false; }
        if (b[0] != b[1]) { LOG("terrain: byte-pair mismatch at sample %zu", k); fclose(f); return false; }
        raw_[k] = b[0];
        h[k] = (float)b[0];
        if (h[k] < hmin) hmin = h[k];
        if (h[k] > hmax) hmax = h[k];
    }
    fclose(f);
    LOG("terrain: %d x %d samples, raw %.0f..%.0f -> world %.1f..%.1f (step %.0f)",
        GRID, GRID, hmin, hmax, worldY(hmin), worldY(hmax), sampleStep);
    return true;
}

float Terrain::heightAtWorld(float x, float z, float fallback) const {
    if (raw_.empty()) return fallback;
    float fi = x / sampleStep + centreI;
    float fj = -z / sampleStep + centreJ;
    if (fi < 0 || fj < 0 || fi > GRID - 1 || fj > GRID - 1) return fallback;
    int i0 = (int)fi, j0 = (int)fj;
    int i1 = i0 < GRID - 1 ? i0 + 1 : i0;
    int j1 = j0 < GRID - 1 ? j0 + 1 : j0;
    float ti = fi - i0, tj = fj - j0;
    float v00 = h[j0 * GRID + i0], v10 = h[j0 * GRID + i1];
    float v01 = h[j1 * GRID + i0], v11 = h[j1 * GRID + i1];
    float a = v00 * (1 - ti) + v10 * ti;
    float b = v01 * (1 - ti) + v11 * ti;
    return worldY(a * (1 - tj) + b * tj);
}

bool Terrain::covers(float x, float z) const {
    if (raw_.empty()) return false;
    float fi = x / sampleStep + centreI;
    float fj = -z / sampleStep + centreJ;
    return fi >= 0 && fj >= 0 && fi <= GRID - 1 && fj <= GRID - 1;
}

} // namespace ht2

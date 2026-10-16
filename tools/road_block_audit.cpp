// Diagnostic (not part of the game): WHICH material makes the road network solid?
//
//   c++ -std=c++17 -O2 -Isrc -o tools/out/road_block_audit \
//       tools/road_block_audit.cpp src/world/B3D.cpp src/world/Way.cpp
//   tools/out/road_block_audit original
//
// It repeats Collision's two filters (ground material and flat submeshes are
// skipped; a triangle whose normal is more than 0.8 vertical is a surface), then
// queries every `.way` polyline sample against the resulting triangles and tallies
// the blocking material.  A road centreline sample that is "solid" is either a
// real authored obstacle (debris, a parked/hidden car, a roadblock) or a phantom
// wall the port invented from scenery - this table is what separates the two.
#include "world/B3D.h"
#include "world/Way.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <dirent.h>
#include <string>
#include <unordered_map>
#include <vector>
using namespace ht2;

namespace {
constexpr float kAbsurdHeight = 1000.0f;
constexpr float kAbsurdXZ = 20000.0f;
constexpr float kDecalMinExtent = 0.5f;
constexpr float kHorizontalUp = 0.8f;

std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}
const char* const kGroundMat[] = {
    "road", "grass", "beton", "concrete", "gravel", "zebra", "spl", "ground",
    "ter_", "water", "cross", "mark", "asphalt", "sand", "dirt", "pavement",
    "sidewalk", "kerb", "curb", "trotoar", "trotuar", "stoika", "stoyanka",
    "parking", "runway", "taxiway", "apron", "swamp", "bshadow", "river",
    "tonnel", "zont", "bottom_bridge", "bridge_0", "forest",
};
const char* const kSolidMat[] = {
    "stolb", "pillar", "column", "kolonn", "wall", "fence", "confence",
    "ograjdenie", "rama", "beton_bridge", "side_beton",
};

bool isGroundMaterial(const std::string& material) {
    if (material.empty()) return false;
    const std::string m = lower(material);
    bool ground = false;
    for (const char* p : kGroundMat) if (m.find(p) != std::string::npos) { ground = true; break; }
    if (!ground) return false;
    for (const char* p : kSolidMat) if (m.find(p) != std::string::npos) return false;
    return true;
}

struct Tri {
    float ax, az, bx, bz, cx, cz;      // XZ footprint
    float h0, h1;                      // height range
    int material = -1;
};

// A drivable-surface triangle: a *surface* (|up| > 0.8) whose material is paint or
// asphalt.  Used to tell "a wall stands on the road" from "the `.way` polyline
// crosses a yard, where a fence is expected".
bool isRoadSurfaceMaterial(const std::string& material) {
    const std::string m = lower(material);
    for (const char* p : {"road", "asphalt", "apron", "runway", "taxiway", "zebra", "cross"})
        if (m.find(p) != std::string::npos) return true;
    return false;
}

// point in triangle (XZ)
bool inTri(const Tri& t, float x, float z) {
    const float d1 = (x - t.bx) * (t.az - t.bz) - (t.ax - t.bx) * (z - t.bz);
    const float d2 = (x - t.cx) * (t.bz - t.cz) - (t.bx - t.cx) * (z - t.cz);
    const float d3 = (x - t.ax) * (t.cz - t.az) - (t.cx - t.ax) * (z - t.az);
    const bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    const bool pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(neg && pos);
}
} // namespace


int main(int argc, char** argv) {
    const std::string data = argc > 1 ? argv[1] : "original";
    const std::string env = data + "/ENV";

    std::vector<std::string> b3d, way;
    DIR* d = opendir(env.c_str());
    if (!d) { fprintf(stderr, "no %s\n", env.c_str()); return 1; }
    while (struct dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".b3d") == 0) b3d.push_back(env + "/" + n);
        else if (n.size() > 4 && n.compare(n.size() - 4, 4, ".way") == 0) way.push_back(env + "/" + n);
    }
    closedir(d);
    std::sort(b3d.begin(), b3d.end());
    std::sort(way.begin(), way.end());

    // --- blocking triangles, exactly Collision's filters ---------------------
    std::vector<std::string> materials;
    std::unordered_map<std::string, int> matId;
    auto mid = [&](const std::string& m) {
        auto it = matId.find(m);
        if (it != matId.end()) return it->second;
        const int id = (int)materials.size();
        materials.push_back(m);
        matId[m] = id;
        return id;
    };
    std::vector<Tri> tris;
    std::vector<std::array<float,3>> wallPlanes;   // authored wall surfaces (nx, nz, d)
    std::vector<Tri> roadTris;      // drivable asphalt, to split "on the road" from "in a yard"
    size_t subs = 0, ground = 0, flat = 0, surface = 0;
    for (const std::string& p : b3d) {
        B3D t;
        if (!t.load(p)) continue;
        t.buildScene();
        for (const auto& hp : t.hitProxies())
            for (size_t i = 0; i + 3 < hp.planes.size(); i += 4)
                wallPlanes.push_back({hp.planes[i], hp.planes[i + 1], hp.planes[i + 3]});
        for (const auto& sm : t.submeshes()) {
            ++subs;
            const bool groundMat = isGroundMaterial(sm.material);
            const bool roadMat = isRoadSurfaceMaterial(sm.material);
            if (groundMat) ++ground;
            float lo = sm.pos.size() >= 3 ? sm.pos[2] : 0, hi = lo;
            for (size_t i = 5; i < sm.pos.size(); i += 3) {
                lo = std::min(lo, sm.pos[i]); hi = std::max(hi, sm.pos[i]);
            }
            if (groundMat && hi - lo < kDecalMinExtent) { ++flat; continue; }
            for (size_t k = 0; k + 8 < sm.pos.size(); k += 9) {
                const float* a = &sm.pos[k];
                const float* b = &sm.pos[k + 3];
                const float* c = &sm.pos[k + 6];
                if (std::fabs(a[0]) > kAbsurdXZ || std::fabs(b[0]) > kAbsurdXZ ||
                    std::fabs(c[0]) > kAbsurdXZ || std::fabs(a[2]) > kAbsurdHeight ||
                    std::fabs(b[2]) > kAbsurdHeight || std::fabs(c[2]) > kAbsurdHeight)
                    continue;
                const float e1x = b[0] - a[0], e1z = b[1] - a[1], e1h = b[2] - a[2];
                const float e2x = c[0] - a[0], e2z = c[1] - a[1], e2h = c[2] - a[2];
                const float nh = e1x * e2z - e1z * e2x;
                const float nx = e1z * e2h - e1h * e2z;
                const float nz = e1h * e2x - e1x * e2h;
                const float len = std::sqrt(nx * nx + nh * nh + nz * nz);
                const float up = len < 1e-9f ? 1.0f : std::fabs(nh) / len;
                if (up > kHorizontalUp) {
                    if (roadMat) {                       // remember the asphalt sheet
                        Tri tr;
                        tr.ax = a[0]; tr.az = a[1];
                        tr.bx = b[0]; tr.bz = b[1];
                        tr.cx = c[0]; tr.cz = c[1];
                        tr.h0 = std::min(a[2], std::min(b[2], c[2]));
                        tr.h1 = std::max(a[2], std::max(b[2], c[2]));
                        tr.material = mid(sm.material);
                        roadTris.push_back(tr);
                    }
                    ++surface;
                    continue;
                }
                if (groundMat) continue;
                Tri tr;
                tr.ax = a[0]; tr.az = a[1];
                tr.bx = b[0]; tr.bz = b[1];
                tr.cx = c[0]; tr.cz = c[1];
                tr.h0 = std::min(a[2], std::min(b[2], c[2]));
                tr.h1 = std::max(a[2], std::max(b[2], c[2]));
                tr.material = mid(sm.material);
                tris.push_back(tr);
            }
        }
    }
    printf("tiles=%zu submeshes=%zu (ground=%zu flat=%zu) blocking triangles=%zu asphalt triangles=%zu\n",
           b3d.size(), subs, ground, flat, tris.size(), roadTris.size());

    const float cell = 32.0f;
    std::unordered_map<long long, std::vector<int>> grid;
    auto key = [](int i, int j) { return ((long long)i << 32) ^ (unsigned)j; };
    for (size_t i = 0; i < tris.size(); ++i) {
        const Tri& t = tris[i];
        const float x0 = std::min(t.ax, std::min(t.bx, t.cx));
        const float x1 = std::max(t.ax, std::max(t.bx, t.cx));
        const float z0 = std::min(t.az, std::min(t.bz, t.cz));
        const float z1 = std::max(t.az, std::max(t.bz, t.cz));
        for (int i0 = (int)std::floor(x0 / cell); i0 <= (int)std::floor(x1 / cell); ++i0)
            for (int j0 = (int)std::floor(z0 / cell); j0 <= (int)std::floor(z1 / cell); ++j0)
                grid[key(i0, j0)].push_back((int)i);
    }


    size_t samples = 0, blocked = 0, blockedOnRoad = 0, onRoadAuthored = 0;
    std::unordered_map<int, size_t> byMat;
    std::unordered_map<int, size_t> onRoadByMat;
    std::unordered_map<int, std::pair<double, double>> example;
    std::unordered_map<int, std::pair<double, double>> exampleOnRoad;
    // asphalt lookup, same 32 m hash
    std::unordered_map<long long, std::vector<int>> roadGrid;
    for (size_t i = 0; i < roadTris.size(); ++i) {
        const Tri& t = roadTris[i];
        const float x0 = std::min(t.ax, std::min(t.bx, t.cx));
        const float x1 = std::max(t.ax, std::max(t.bx, t.cx));
        const float z0 = std::min(t.az, std::min(t.bz, t.cz));
        const float z1 = std::max(t.az, std::max(t.bz, t.cz));
        for (int i0 = (int)std::floor(x0 / cell); i0 <= (int)std::floor(x1 / cell); ++i0)
            for (int j0 = (int)std::floor(z0 / cell); j0 <= (int)std::floor(z1 / cell); ++j0)
                roadGrid[key(i0, j0)].push_back((int)i);
    }
    for (const std::string& p : way) {
        Way w;
        if (!w.load(p)) continue;
        for (const auto& sg : w.segments()) {
            for (const auto& pt : sg.pts) {
                ++samples;
                const float x = (float)pt.x, z = (float)pt.z, h = (float)pt.y;
                int mat = -1;
                auto it = grid.find(key((int)std::floor(x / cell), (int)std::floor(z / cell)));
                if (it != grid.end())
                    for (int ti : it->second) {
                        const Tri& t = tris[(size_t)ti];
                        if (h < t.h0 - 1.0f || h > t.h1 + 4.0f) continue;   // road under/over
                        if (!inTri(t, x, z)) continue;
                        mat = t.material;
                        break;
                    }
                if (mat < 0) continue;
                ++blocked;
                ++byMat[mat];
                if (!example.count(mat)) example[mat] = {x, z};
                // Is the *blocking* geometry standing on the authored asphalt?
                auto rg = roadGrid.find(key((int)std::floor(x / cell), (int)std::floor(z / cell)));
                bool onRoad = false;
                if (rg != roadGrid.end())
                    for (int ti : rg->second) {
                        const Tri& t = roadTris[(size_t)ti];
                        if (h < t.h0 - 1.0f || h > t.h1 + 1.0f) continue;
                        if (!inTri(t, x, z)) continue;
                        onRoad = true;
                        break;
                    }
                if (onRoad) {
                    ++blockedOnRoad;
                    float best = 1e30f;
                    for (const auto& w : wallPlanes) {
                        const float len = std::sqrt(w[0] * w[0] + w[1] * w[1]);
                        if (len < 1e-6f) continue;
                        const float d = std::fabs(w[0] * x + w[1] * z + w[2]) / len;
                        if (d < best) best = d;
                    }
                    if (best < 2.0f) ++onRoadAuthored;
                    ++onRoadByMat[mat];
                    if (!exampleOnRoad.count(mat)) exampleOnRoad[mat] = {x, z};
                }
            }
        }
    }
    printf("way samples=%zu blocked=%zu (%.2f%%), of them ON authored asphalt=%zu (%.2f%%)\n",
           samples, blocked, samples ? 100.0 * (double)blocked / (double)samples : 0.0,
           blockedOnRoad, samples ? 100.0 * (double)blockedOnRoad / (double)samples : 0.0);
    printf("   of the on-asphalt hits, %zu (%.1f%%) lie within 2 m of an authored wall plane "
           "(%zu planes total) -> authored, not phantom\n",
           onRoadAuthored,
           blockedOnRoad ? 100.0 * (double)onRoadAuthored / (double)blockedOnRoad : 0.0,
           wallPlanes.size());
    std::vector<std::pair<size_t, int>> ranked;
    for (const auto& kv : onRoadByMat) ranked.push_back({kv.second, kv.first});
    std::sort(ranked.rbegin(), ranked.rend());
    printf("-- blocking material, on the asphalt only --\n");
    for (size_t i = 0; i < ranked.size() && i < 25; ++i) {
        const int m = ranked[i].second;
        printf("  %6zu (%5.2f%%) %-24s first at (%.0f, %.0f)\n", ranked[i].first,
               100.0 * (double)ranked[i].first / (double)blockedOnRoad, materials[(size_t)m].c_str(),
               exampleOnRoad[m].first, exampleOnRoad[m].second);
    }
    ranked.clear();
    for (const auto& kv : byMat) ranked.push_back({kv.second, kv.first});
    std::sort(ranked.rbegin(), ranked.rend());
    printf("-- blocking material, all .way samples --\n");
    for (size_t i = 0; i < ranked.size() && i < 15; ++i) {
        const int m = ranked[i].second;
        printf("  %6zu (%5.2f%%) %-24s first at (%.0f, %.0f)\n", ranked[i].first,
               100.0 * (double)ranked[i].first / (double)blocked, materials[(size_t)m].c_str(),
               example[m].first, example[m].second);
    }
    return 0;
}

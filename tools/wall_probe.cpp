// Diagnostic (not part of the game): which authored triangle makes a world point
// solid?  Reproduces Collision's two filters (ground material / flat submesh are
// skipped, a triangle whose normal is more than 0.8 vertical is a surface) and
// prints every remaining triangle whose XZ footprint covers the query point.
//
//   c++ -std=c++17 -O2 -Isrc -o tools/out/wall_probe tools/wall_probe.cpp \
//       src/world/B3D.cpp
//   tools/out/wall_probe original/ENV -3100,-4625
#include "world/B3D.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <functional>
#include <string>
#include <vector>
using namespace ht2;

static std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}
// Same material rule as Collision.cpp (ground list wins unless a solid keyword).
static bool groundMaterial(const std::string& material) {
    static const char* kGround[] = {"road","grass","beton","concrete","gravel","zebra","spl",
        "ground","ter_","water","cross","mark","asphalt","sand","dirt","pavement","sidewalk",
        "kerb","curb","trotoar","trotuar","stoika","stoyanka","forest"};
    static const char* kSolid[] = {"wall","sten","stena","column","pillar","kolonna","fence",
        "zabor","store","house","dom"};
    if (material.empty()) return false;
    const std::string m = lower(material);
    bool ground = false;
    for (const char* p : kGround) if (m.find(p) != std::string::npos) { ground = true; break; }
    if (!ground) return false;
    for (const char* p : kSolid) if (m.find(p) != std::string::npos) return false;
    return true;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[2]) == "--hits") {
        // Walk the record tree (the render walker is not involved) and report
        // every hit_* node the format carries, with its depth.
        std::vector<std::string> files;
        DIR* d = opendir(argv[1]);
        if (!d) return 1;
        while (struct dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".b3d") == 0)
                files.push_back(std::string(argv[1]) + "/" + n);
        }
        closedir(d);
        std::sort(files.begin(), files.end());
        size_t total = 0;
        for (const std::string& p : files) {
            B3D t;
            if (!t.load(p)) continue;
            size_t hits = 0, meshed = 0;
            int minDepth = 99;
            std::function<void(const B3D::Node&, int)> walk = [&](const B3D::Node& n, int depth) {
                if (n.name.rfind("hit_", 0) == 0) {
                    ++hits;
                    if (n.meshIndex >= 0 || n.faceIndex >= 0) ++meshed;
                    if (depth < minDepth) minDepth = depth;
                }
                for (const auto& k : n.kids) walk(k, depth + 1);
            };
            for (const auto& n : t.tree()) walk(n, 0);
            // Structure of one proxy container: what carries the geometry?
            static bool printed = false;
            if (!printed) {
                std::function<void(const B3D::Node&, int)> show = [&](const B3D::Node& n, int d) {
                    if (d <= 3)
                        printf("    %*s%-22s subtag=%u mesh=%d face=%d kids=%zu params=%zu\n", d * 2,
                               "", n.name.c_str(), n.subtag, n.meshIndex, n.faceIndex,
                               n.kids.size(), n.params.size());
                    for (const auto& k : n.kids) show(k, d + 1);
                };
                std::function<bool(const B3D::Node&)> find = [&](const B3D::Node& n) {
                    if (n.name.rfind("hit_", 0) == 0) { show(n, 0); return true; }
                    for (const auto& k : n.kids) if (find(k)) return true;
                    return false;
                };
                for (const auto& n : t.tree()) if (find(n)) break;
                printed = true;
            }
            // Resolve the link records inside the FIRST hit_* container: what does
            // a collision proxy actually point at, and is that node in the tree?
            static bool printed2 = false;
            if (!printed2 && hits) {
                std::function<bool(const B3D::Node&)> scan = [&](const B3D::Node& n) {
                    if (n.name.rfind("hit_", 0) == 0) {
                        printf("proxy %s subtag=%u params=%zu kids=%zu\n", n.name.c_str(), n.subtag,
                               n.params.size(), n.kids.size());
                        for (const auto& k : n.kids) {
                            printf("   kid %-24s subtag=%u params=%zu", k.name.c_str(), k.subtag,
                                   k.params.size());
                            for (size_t pi = 0; pi < k.params.size(); ++pi) {
                                const uint32_t v = k.params[pi];
                                if (v < t.names().size() && !t.names()[v].empty())
                                    printf(" p%zu->'%s'", pi, t.names()[v].c_str());
                            }
                            printf("\n");
                            for (const auto& kk : k.kids)
                                printf("      kidkid %-20s subtag=%u mesh=%d face=%d\n",
                                       kk.name.c_str(), kk.subtag, kk.meshIndex, kk.faceIndex);
                        }
                        return true;
                    }
                    for (const auto& k : n.kids) if (scan(k)) return true;
                    return false;
                };
                for (const auto& n : t.tree()) if (scan(n)) break;
                printed2 = true;
            }
            total += hits;
            if (hits)
                printf("%-28s hit nodes=%zu vis=%zu\n",
                       p.c_str() + p.find_last_of('/') - 4, hits, t.submeshes().size());
        }
        printf("total hit_* record nodes: %zu\n", total);
        return 0;
    }
    if (argc >= 3 && std::string(argv[2]) == "--dump") {
        // Raw parameters of the first collision-proxy records: the shapes a
        // faithful Collision needs (subtag 40 = AAhitTerrain, 18 = link, 5 = obj).
        const std::string file = argc > 3 ? argv[3] : (std::string(argv[1]) + "/ap.b3d");
        B3D t;
        if (!t.load(file)) return 1;
        size_t shown = 0;
        std::function<void(const B3D::Node&)> walk = [&](const B3D::Node& n) {
            if (shown >= 14) return;
            if (n.name.rfind("hit_", 0) == 0) {
                printf("proxy %s subtag=%u params=%zu\n", n.name.c_str(), n.subtag, n.params.size());
                for (const auto& k : n.kids) {
                    printf("  %s subtag=%u params=%zu\n", k.name.c_str(), k.subtag, k.params.size());
                    for (size_t i = 0; i < k.params.size(); ++i) {
                        float f;
                        memcpy(&f, &k.params[i], 4);
                        printf("     [%2zu] 0x%08x  %12.4f\n", i, k.params[i], f);
                    }
                }
                ++shown;
            }
            for (const auto& k : n.kids) walk(k);
        };
        for (const auto& n : t.tree()) walk(n);
        return 0;
    }
    if (argc < 3) { fprintf(stderr, "usage: wall_probe <dataDir> x,z | --hits | --dump FILE\n"); return 1; }
    float qx = 0, qz = 0;
    if (sscanf(argv[2], "%f,%f", &qx, &qz) != 2) return 1;
    std::vector<std::string> files;
    DIR* d = opendir((std::string(argv[1])).c_str());
    if (!d) return 1;
    while (struct dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".b3d") == 0)
            files.push_back(std::string(argv[1]) + "/" + n);
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    size_t hits = 0, skippedGround = 0, skippedFlat = 0, skippedSurface = 0;
    for (const std::string& p : files) {
        B3D t;
        if (!t.load(p)) continue;
        t.buildScene();
        for (const auto& sm : t.submeshes()) {
            if (groundMaterial(sm.material)) { ++skippedGround; continue; }
            float lo = sm.pos.size() >= 3 ? sm.pos[2] : 0, hi = lo;
            for (size_t i = 5; i < sm.pos.size(); i += 3) {
                lo = std::min(lo, sm.pos[i]); hi = std::max(hi, sm.pos[i]);
            }
            if (hi - lo < 0.5f) { ++skippedFlat; continue; }
            for (size_t k = 0; k + 8 < sm.pos.size(); k += 9) {
                const float* a = &sm.pos[k];
                const float* b = &sm.pos[k + 3];
                const float* c = &sm.pos[k + 6];
                if (std::fabs(a[0]) > 20000 || std::fabs(b[0]) > 20000 ||
                    std::fabs(c[0]) > 20000 || std::fabs(a[2]) > 1000) continue;
                const float x0 = std::min(a[0], std::min(b[0], c[0]));
                const float x1 = std::max(a[0], std::max(b[0], c[0]));
                const float z0 = std::min(a[1], std::min(b[1], c[1]));
                const float z1 = std::max(a[1], std::max(b[1], c[1]));
                if (qx < x0 - 1.0f || qx > x1 + 1.0f || qz < z0 - 1.0f || qz > z1 + 1.0f) continue;
                const float e1x = b[0] - a[0], e1z = b[1] - a[1], e1h = b[2] - a[2];
                const float e2x = c[0] - a[0], e2z = c[1] - a[1], e2h = c[2] - a[2];
                const float nh = e1x * e2z - e1z * e2x;
                const float nx = e1z * e2h - e1h * e2z;
                const float nz = e1h * e2x - e1x * e2h;
                const float len = std::sqrt(nx * nx + nh * nh + nz * nz);
                const float up = len < 1e-9f ? 1.0f : std::fabs(nh) / len;
                if (up > 0.8f) { ++skippedSurface; continue; }
                ++hits;
                if (hits <= 40)
                    printf("%s %-18s %-22s footprint x[%.1f..%.1f] z[%.1f..%.1f] h[%.1f..%.1f] up=%.2f\n",
                           p.c_str() + p.find_last_of('/') - 4, sm.name.c_str(), sm.material.c_str(),
                           x0, x1, z0, z1,
                           std::min(a[2], std::min(b[2], c[2])),
                           std::max(a[2], std::max(b[2], c[2])), up);
            }
        }
    }
    printf("query (%.1f, %.1f): %zu blocking triangles (skipped: ground=%zu flat=%zu surface=%zu)\n",
           qx, qz, hits, skippedGround, skippedFlat, skippedSurface);
    return 0;
}

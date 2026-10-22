// tools/collision_check.cpp -- standalone harness for src/game/Collision.
//
// Not part of the game build (it is not in CMakeLists.txt).  Build it by hand:
//
//   c++ -O2 -std=c++17 -Isrc tools/collision_check.cpp \
//       src/game/Collision.cpp src/world/B3D.cpp src/world/Way.cpp \
//       -o tools/out/collision_check
//
// It loads every ENV/*.b3d exactly like App::loadWorldTiles(), builds the
// collision grid, prints the timing and the blocked fractions at the known
// places, checks every .way node / road-segment sample, and can dump the raw
// grid so tools/collision_probe.py can diff the Python rule against the C++ one.
#include "game/Collision.h"
#include "world/B3D.h"
#include "world/Way.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <set>
#include <string>
#include <vector>

using namespace ht2;

static std::vector<std::string> listTiles(const std::string& dir) {
    std::vector<std::string> files;
    DIR* d = opendir((dir + "/ENV").c_str());
    if (!d) return files;
    while (struct dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".b3d") == 0)
            files.push_back(dir + "/ENV/" + n);
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    return files;
}

static void fraction(const Collision& c, const char* what, float x, float z, float r) {
    const float step = 0.5f;
    int n = 0, hit = 0;
    int k = (int)std::ceil(r / step);
    for (int a = -k; a <= k; ++a)
        for (int b = -k; b <= k; ++b) {
            float px = x + a * step, pz = z + b * step;
            if ((px - x) * (px - x) + (pz - z) * (pz - z) > r * r) continue;
            ++n;
            if (c.blocked(px, pz, 0.0f)) ++hit;
        }
    printf("%-22s %9.1f %9.1f   %5.1f%%  (%d samples)  blocked(r=0)=%d\n",
           what, x, z, n ? 100.0 * hit / n : 0.0, n, (int)c.blocked(x, z, 0.0f));
}


// deterministic unit test: one wall quad at x = 2.5, circle queries and sliding
static int unitTest() {
    B3D::SubMesh wall;
    wall.name = "test_wall";
    wall.material = "mat_wall";
    wall.pos = {2.5f, -10, 0, 2.5f, 10, 0, 2.5f, 10, 3, 2.5f, -10, 3};
    wall.idx = {0, 1, 2, 0, 2, 3};
    B3D::SubMesh road;                     // ground material: never solid
    road.name = "test_road";
    road.material = "mat_road";
    road.pos = {-20, -20, 0, 20, -20, 0, 20, 20, 0};
    road.idx = {0, 1, 2};
    std::vector<B3D::SubMesh> ms{wall, road};
    Collision c;
    c.build(ms, 1.0f);
    int fails = 0;
    auto check = [&](const char* what, bool got, bool want) {
        printf("  %-48s %s (got %d want %d)\n", what, got == want ? "ok" : "FAIL",
               (int)got, (int)want);
        if (got != want) ++fails;
    };
    check("circle clear of the wall", c.blocked(0.0f, 0.0f, 0.5f), false);
    check("circle inside the wall cell", c.blocked(2.5f, 0.0f, 0.0f), true);
    check("radius reaches the wall", c.blocked(1.6f, 0.0f, 0.5f), true);
    check("radius just short of the wall", c.blocked(1.4f, 0.0f, 0.5f), false);
    check("road submesh is not solid", c.blocked(10.0f, 10.0f, 0.5f), false);
    float ox, oz;
    bool free = c.resolveMove(-5, 0, 5, 0, 0.5f, ox, oz);
    check("head-on move is not free", free, false);
    check("head-on stop stays clear", c.blocked(ox, oz, 0.5f), false);
    check("head-on did not pass the wall", ox <= 1.6f, true);
    printf("  head-on stop at (%.2f, %.2f)\n", ox, oz);
    free = c.resolveMove(-5, -5, 5, 5, 0.5f, ox, oz);
    check("diagonal move is not free", free, false);
    check("diagonal result stays clear", c.blocked(ox, oz, 0.5f), false);
    check("diagonal stopped at the wall", ox <= 1.6f, true);
    check("diagonal slid along the wall", oz > 2.0f, true);
    printf("  diagonal slide from (-5,-5) to (%.2f, %.2f)  [a dead stop would end near z=-5]\n", ox, oz);
    free = c.resolveMove(-1, -5, -1, 5, 0.5f, ox, oz);
    check("move parallel to the wall is free", free, true);
    free = c.resolveMove(-5, 0, -6, 0, 0.5f, ox, oz);
    check("move away from the wall is free", free, true);
    free = c.resolveMove(-5, 0, 5, 0, 0.5f, ox, oz);
    check("a long step does not tunnel through", free, false);

    // --- authored thin walls (docs/hit_proxies.md) --------------------------
    // A wall is a plane with a finite extent: the crossing test must stop at the
    // surface, ignore a move that stays on one side, ignore points outside the
    // extent and ignore a height band that does not overlap the body's slab.
    {
        B3D::HitProxy p;
        B3D::Wall w;
        w.n[0] = 1.0f; w.n[1] = 0.0f; w.n[2] = 0.0f; w.n[3] = -100.0f;   // plane x = 100
        w.ring = {99.5f, 0.0f, 4.0f, 100.5f, 0.0f, 4.0f,
                  100.5f, 10.0f, 4.0f, 99.5f, 10.0f, 4.0f};             // thin strip
        w.yMin = 0.0f;
        w.haveBottom = true;
        p.walls.push_back(w);
        Collision wc;
        wc.build({}, {p});
        check("proxy wall is collected", wc.wallCount(), 1);
        free = wc.resolveMove(98.0f, 5.0f, 102.0f, 5.0f, 1.0f, ox, oz, 0.0f, 2.0f);
        check("crossing an authored wall is not free", free, false);
        check("the car stops at the wall", ox < 100.0f && ox > 99.0f, true);
        free = wc.resolveMove(98.0f, 5.0f, 99.0f, 5.0f, 1.0f, ox, oz, 0.0f, 2.0f);
        check("stopping short of the wall is free", free, true);
        free = wc.resolveMove(98.0f, 40.0f, 102.0f, 40.0f, 1.0f, ox, oz, 0.0f, 2.0f);
        check("outside the wall extent is free", free, true);
        free = wc.resolveMove(98.0f, 5.0f, 102.0f, 5.0f, 1.0f, ox, oz, 5.0f, 9.0f);
        check("a body above the wall passes", free, true);
    }
    // A second wall in its own group blocks until that group is skipped —
    // the ambush roadblock is hidden this way when the player drives away.
    {
        B3D::SubMesh gate = wall;
        gate.name = "ambush_car";
        gate.material = "mat_body";
        gate.group = "ht2_ambush_0";
        gate.pos = {8.0f, -2, 0, 8.0f, 2, 0, 8.0f, 2, 3, 8.0f, -2, 3};
        std::set<std::string> skip;
        skip.insert(gate.group);
        Collision shown;
        shown.build({wall, gate}, 1.0f, nullptr);
        check("roadblock blocks while its group is shown", shown.blocked(8.0f, 0.0f, 0.0f), true);
        Collision hidden;
        hidden.build({wall, gate}, 1.0f, &skip);
        check("hidden roadblock does not block", hidden.blocked(8.0f, 0.0f, 0.0f), false);
        check("the other wall still blocks", hidden.blocked(2.5f, 0.0f, 0.0f), true);
    }
    printf("unit test: %d failure(s)\n\n", fails);
    return fails;
}

int main(int argc, char** argv) {
    std::string dir = "original";
    std::string dump, only, listPath;
    float probeX = 0, probeZ = 0; bool probe = false;
    float cell = Collision::defaultCellSize();
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--unit")) return unitTest();
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        else if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else if (!strcmp(argv[i], "--list") && i + 1 < argc) listPath = argv[++i];
        else if (!strcmp(argv[i], "--cell") && i + 1 < argc) cell = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--probe") && i + 2 < argc) {
            probeX = (float)atof(argv[++i]); probeZ = (float)atof(argv[++i]); probe = true;
        }
    }

    std::vector<B3D::SubMesh> world;
    auto files = listTiles(dir);
    if (!only.empty()) {
        std::vector<std::string> keep;
        for (auto& f : files)
            if (f.find("/" + only) != std::string::npos) keep.push_back(f);
        files.swap(keep);
    }
    auto t0 = std::chrono::steady_clock::now();
    FILE* lf = listPath.empty() ? nullptr : fopen(listPath.c_str(), "w");
    for (const std::string& p : files) {
        B3D t;
        if (!t.load(p)) continue;
        t.buildScene();
        if (lf)
            for (auto& sm : t.submeshes()) {
                float mn[2] = {1e30f, 1e30f}, mx[2] = {-1e30f, -1e30f};
                for (size_t i = 0; i + 2 < sm.pos.size(); i += 3) {
                    mn[0] = std::min(mn[0], sm.pos[i]);
                    mx[0] = std::max(mx[0], sm.pos[i]);
                    mn[1] = std::min(mn[1], sm.pos[i + 1]);
                    mx[1] = std::max(mx[1], sm.pos[i + 1]);
                }
                fprintf(lf, "%s|%s|%zu|%.2f|%.2f\n", sm.name.c_str(), sm.material.c_str(),
                        sm.idx.size() / 3, 0.5f * (mn[0] + mx[0]), 0.5f * (mn[1] + mx[1]));
            }
        for (auto& sm : t.submeshesMutable())
            world.push_back(std::move(sm));
    }
    if (lf) fclose(lf);
    auto t1 = std::chrono::steady_clock::now();

    if (probe) {
        printf("submeshes whose bbox contains (%.1f, %.1f):\n", probeX, probeZ);
        for (auto& sm : world) {
            float mnx = 1e30f, mxx = -1e30f, mnz = 1e30f, mxz = -1e30f;
            for (size_t i = 0; i + 2 < sm.pos.size(); i += 3) {
                mnx = std::min(mnx, sm.pos[i]); mxx = std::max(mxx, sm.pos[i]);
                mnz = std::min(mnz, sm.pos[i + 1]); mxz = std::max(mxz, sm.pos[i + 1]);
            }
            if (probeX >= mnx && probeX <= mxx && probeZ >= mnz && probeZ <= mxz)
                printf("   %-24s %-18s tris=%zu x[%.0f %.0f] z[%.0f %.0f]\n",
                       sm.name.c_str(), sm.material.c_str(), sm.idx.size() / 3,
                       mnx, mxx, mnz, mxz);
        }
    }
    Collision col;
    col.build(world, cell);
    auto t2 = std::chrono::steady_clock::now();
    double loadMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double buildMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
    const Collision::Stats& s = col.stats();
    printf("tiles=%zu submeshes=%zu (ground=%zu flat=%zu) solid triangles=%zu\n",
           files.size(), s.submeshes, s.ground, s.flat, s.triangles);
    printf("grid %dx%d cell=%.2f  %.1f MB  solid cells=%zu (%.2f%%)\n",
           col.width(), col.height(), col.cellSize(),
           (double)s.gridCells / 8.0 / 1e6, col.cells(),
           s.gridCells ? 100.0 * (double)col.cells() / (double)s.gridCells : 0.0);
    printf("assembly %.0f ms   collision build %.0f ms\n", loadMs, buildMs);

    if (probe) {
        int ix = (int)std::floor((probeX - col.minX()) / col.cellSize());
        int iz = (int)std::floor((probeZ - col.minZ()) / col.cellSize());
        printf("\nprobe (%.1f, %.1f): cell (%d,%d) solid=%d blocked(r=0)=%d\n",
               probeX, probeZ, ix, iz, (int)col.solidCell(ix, iz),
               (int)col.blocked(probeX, probeZ, 0.0f));
        int cnt = 0;
        for (int a = -60; a <= 60; ++a)
            for (int b = -60; b <= 60; ++b)
                if (col.solidCell(ix + a, iz + b)) ++cnt;
        printf("solid cells within +-60 cells: %d\n", cnt);
    }
    printf("\n%-22s %9s %9s   %s\n", "place", "x", "z", "blocked disc r=12");
    fraction(col, "airport apron", -3063.0f, -4658.0f, 12.0f);
    fraction(col, "highway", -2330.0f, -3760.0f, 12.0f);
    fraction(col, "player spawn", -3020.0f, -4625.0f, 12.0f);
    // STOS drive-in repair bay (aa.b3d) and its interior wall
    fraction(col, "warehouse STOS bay", 4750.5f, -5675.0f, 12.0f);
    fraction(col, "warehouse STOS wall", 4748.1f, -5675.0f, 12.0f);
    fraction(col, "repair shop af bay", -2400.0f, 2758.5f, 12.0f);

    // --- .way nodes and road segments ---------------------------------------
    size_t nodes = 0, nodesBlocked = 0, samples = 0, samplesBlocked = 0;
    std::vector<std::string> blockedNames;
    std::vector<std::string> blockedSamples;
    for (const std::string& p : files) {
        std::string wp = p.substr(0, p.size() - 4) + ".way";
        Way way;
        if (!way.load(wp)) continue;
        for (const Way::Room& r : way.rooms)
            for (const Way::Node& nd : r.nodes) {
                ++nodes;
                if (col.blocked((float)nd.pos.x, (float)nd.pos.z, 0.0f)) {
                    ++nodesBlocked;
                    if (blockedNames.size() < 15) blockedNames.push_back(nd.name);
                }
            }
        for (const Way::Seg& sg : way.segs) {
            for (size_t i = 0; i + 1 < sg.pts.size(); ++i) {
                const Way::Pos& a = sg.pts[i];
                const Way::Pos& b = sg.pts[i + 1];
                for (int k = 0; k <= 8; ++k) {
                    float t = (float)k / 8.0f;
                    float x = (float)(a.x + (b.x - a.x) * t);
                    float z = (float)(a.z + (b.z - a.z) * t);
                    ++samples;
                    if (col.blocked(x, z, 0.0f)) {
                        ++samplesBlocked;
                        if (blockedSamples.size() < 20 && (samplesBlocked % 97) == 1) {
                            char buf[160];
                            snprintf(buf, sizeof buf, "%s @ %.0f,%.0f w=%.1f",
                                     sg.name.empty() ? "?" : sg.name.c_str(), x, z, sg.width);
                            blockedSamples.push_back(buf);
                        }
                    }
                }
            }
        }
    }
    printf("\n.way: %zu nodes, %zu blocked at r=0 (%.1f%%)\n",
           nodes, nodesBlocked, nodes ? 100.0 * nodesBlocked / nodes : 0.0);
    printf("      first blocked nodes:");
    for (size_t i = 0; i < blockedNames.size(); ++i)
        printf(" %s", blockedNames[i].c_str());
    printf("\n.way: %zu road-segment samples, %zu blocked at r=0 (%.2f%%)\n",
           samples, samplesBlocked, samples ? 100.0 * samplesBlocked / samples : 0.0);
    for (const std::string& b : blockedSamples) printf("      %s\n", b.c_str());

    if (!dump.empty()) {
        FILE* f = fopen(dump.c_str(), "wb");
        if (f) {
            int32_t nx = col.width(), nz = col.height();
            float x0 = col.minX(), z0 = col.minZ(), cs = col.cellSize();
            fwrite(&nx, 4, 1, f); fwrite(&nz, 4, 1, f);
            fwrite(&x0, 4, 1, f); fwrite(&z0, 4, 1, f); fwrite(&cs, 4, 1, f);
            // repack the bit grid densely (word order is implementation-free)
            std::vector<unsigned char> bytes((size_t)nx * nz / 8 + 1, 0);
            for (int i = 0; i < nx; ++i)
                for (int j = 0; j < nz; ++j)
                    if (col.solidCell(i, j)) {
                        size_t idx = (size_t)i * nz + j;
                        bytes[idx >> 3] |= (unsigned char)(1u << (idx & 7));
                    }
            fwrite(bytes.data(), 1, bytes.size(), f);
            fclose(f);
            printf("wrote grid dump %s (%dx%d @ %.2f, origin %.1f %.1f)\n",
                   dump.c_str(), nx, nz, cs, x0, z0);
        }
    }
    return 0;
}

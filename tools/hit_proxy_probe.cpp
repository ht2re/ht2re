// Diagnostic: decode the authored collision proxies (`hit_obj_*` / `hit_road_*`).
//
//   c++ -std=c++17 -O2 -Isrc -o tools/out/hit_proxy_probe \
//       tools/hit_proxy_probe.cpp src/world/B3D.cpp
//   tools/out/hit_proxy_probe original/ENV [tile.b3d] [idSubstring]
//
// Prints, for every `hit_*` top-level container: its records (subtag + parameters
// as hex/float/ascii), the *raw* payload of the scan leaves (subtag 20/23, which
// B3D stores no parameters for - their bytes sit between the node's `off` and
// `end`), and the bounding box of the visible object with the same id, so the
// proxy fields can be matched against real geometry.
#include "world/B3D.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <functional>
#include <map>
#include <string>
#include <vector>
using namespace ht2;

static const char* subTagName(uint32_t s) {
    switch (s) {
        case 5: return "obj?"; case 9: return "plane?"; case 12: return "box?";
        case 18: return "link"; case 20: return "scan/box"; case 23: return "scan";
        case 40: return "AAhitTerrain"; default: return "";
    }
}
static std::string inlineName(const std::vector<uint32_t>& p, size_t dw) {
    if (dw + 3 > p.size()) return std::string();
    char b[13];
    memcpy(b, &p[dw], 12);
    b[12] = 0;
    return std::string(b);
}
static void dumpDwords(const uint8_t* d, size_t from, size_t to, const char* indent) {
    for (size_t q = from; q + 4 <= to && q - from < 256; q += 4) {
        uint32_t v;
        memcpy(&v, d + q, 4);
        float f;
        memcpy(&f, &v, 4);
        char ascii[5] = {0};
        for (int k = 0; k < 4; ++k) {
            const unsigned char c = (unsigned char)((v >> (8 * k)) & 0xFF);
            ascii[k] = (c >= 32 && c < 127) ? (char)c : '.';
        }
        printf("%s+%3zu 0x%08x %12.4f  '%s'%s\n", indent, q - from, v, f, ascii,
               (v == 444u || v == 555u) ? "  <-closer" : "");
    }
}

int main(int argc, char** argv) {
    // --verify: validate the field decode statistically against the visible
    // geometry (a polygon must lie inside its object's footprint; a plane must be
    // tangent to it, not float in the middle of the map).
    if (argc > 2 && std::string(argv[2]) == "--verify") {
        const std::string vdir = argc > 1 ? argv[1] : "original/ENV";
        std::vector<std::string> vfiles;
        DIR* vd = opendir(vdir.c_str());
        if (!vd) return 1;
        while (struct dirent* e = readdir(vd)) {
            std::string n = e->d_name;
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".b3d") == 0) vfiles.push_back(n);
        }
        closedir(vd);
        std::sort(vfiles.begin(), vfiles.end());
        size_t polyN = 0, polyInside = 0, polyNear = 0;
        size_t planeN = 0, planeTangent = 0, planeThrough = 0, planeFar = 0;
        size_t linkN = 0, linkFound = 0;
        double worstPoly = 0;
        for (const std::string& name : vfiles) {
            B3D t;
            if (!t.load(vdir + "/" + name)) continue;
            t.buildScene();
            FILE* f = fopen((vdir + "/" + name).c_str(), "rb");
            if (!f) continue;
            std::vector<uint8_t> raw;
            fseek(f, 0, SEEK_END);
            raw.resize((size_t)ftell(f));
            fseek(f, 0, SEEK_SET);
            if (fread(raw.data(), 1, raw.size(), f) != raw.size()) { fclose(f); continue; }
            fclose(f);
            std::function<void(const B3D::Node&)> scan = [&](const B3D::Node& n) {
                if (n.name.rfind("hit_", 0) != 0) {
                    for (const auto& k : n.kids) scan(k);
                    return;
                }
                std::string id = n.name.substr(4);          // obj_ap_048 / road_ap_048
                const size_t us = id.find('_');
                const std::string core = us == std::string::npos ? id : id.substr(us + 1);
                float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
                for (const auto& sm : t.submeshes()) {
                    if (sm.name.find(core) == std::string::npos || sm.pos.size() < 3) continue;
                    for (size_t i = 0; i + 2 < sm.pos.size(); i += 3) {
                        x0 = std::min(x0, sm.pos[i]); x1 = std::max(x1, sm.pos[i]);
                        z0 = std::min(z0, sm.pos[i + 1]); z1 = std::max(z1, sm.pos[i + 1]);
                        y0 = std::min(y0, sm.pos[i + 2]); y1 = std::max(y1, sm.pos[i + 2]);
                    }
                }
                if (x0 > x1) {
                    for (const auto& k : n.kids) scan(k);
                    return;
                }
                const float cx = 0.5f * (x0 + x1), cz = 0.5f * (z0 + z1);
                const float ext = std::max(x1 - x0, z1 - z0) + 1.0f;
                for (const auto& k : n.kids) {
                    if (k.subtag == 20 && k.params.empty()) {
                        uint32_t hdr[8] = {0};
                        for (int i = 0; i < 8; ++i)
                            memcpy(&hdr[i], raw.data() + k.off + 40 + 4 * (size_t)i, 4);
                        const uint32_t cnt = hdr[4];
                        if (cnt < 3 || cnt > 64) continue;
                        bool inside = true, near = false;
                        double worst = 0;
                        for (uint32_t v = 0; v < cnt; ++v) {
                            float p[3];
                            memcpy(p, raw.data() + k.off + 72 + 12 * (size_t)v, 12);
                            const float dx = std::max(0.0f, std::max(x0 - p[0], p[0] - x1));
                            const float dz = std::max(0.0f, std::max(z0 - p[1], p[1] - z1));
                            const double miss = std::sqrt(dx * dx + dz * dz);
                            worst = std::max(worst, miss);
                            if (miss > 8.0f) inside = false;
                            if (miss < 1.0e-3) near = true;
                        }
                        ++polyN;
                        if (inside) ++polyInside;
                        if (near) ++polyNear;
                        worstPoly = std::max(worstPoly, worst);
                    } else if (k.subtag == 9 && k.params.size() >= 8) {
                        float nx, nz, ny, d;
                        memcpy(&nx, &k.params[4], 4); memcpy(&nz, &k.params[5], 4);
                        memcpy(&ny, &k.params[6], 4); memcpy(&d, &k.params[7], 4);
                        const float len = std::sqrt(nx * nx + nz * nz + ny * ny);
                        if (len < 1e-6f) continue;
                        const float s = (nx * cx + nz * cz + ny * 0.5f * (y0 + y1) + d) / len;
                        ++planeN;
                        if (std::fabs(s) <= 0.05f * ext) ++planeTangent;
                        else if (std::fabs(s) <= 1.5f * ext) ++planeThrough;
                        else ++planeFar;
                    } else if (k.subtag == 18 && k.params.size() >= 20) {
                        ++linkN;
                        const std::string to = inlineName(k.params, 12);
                        bool found = false;
                        std::function<void(const B3D::Node&)> look = [&](const B3D::Node& q) {
                            if (q.name == to) { found = true; return; }
                            for (const auto& kk : q.kids) if (!found) look(kk);
                        };
                        for (const auto& q : t.tree()) if (!found) look(q);
                        if (found) ++linkFound;
                    }
                    for (const auto& kk : k.kids) scan(kk);
                }
            };
            for (const auto& n : t.tree()) scan(n);
        }
        printf("polygons(subtag 20): %zu, vertices inside object footprint: %zu (%.1f%%), "
               "touching an edge: %zu (%.1f%%), worst miss %.1f m\n",
               polyN, polyInside, polyN ? 100.0 * (double)polyInside / (double)polyN : 0.0,
               polyNear, polyN ? 100.0 * (double)polyNear / (double)polyN : 0.0, worstPoly);
        printf("planes(subtag 9): %zu - tangent to the object %zu (%.1f%%), through it %zu (%.1f%%), "
               "far %zu (%.1f%%)\n",
               planeN, planeTangent, planeN ? 100.0 * (double)planeTangent / (double)planeN : 0.0,
               planeThrough, planeN ? 100.0 * (double)planeThrough / (double)planeN : 0.0,
               planeFar, planeN ? 100.0 * (double)planeFar / (double)planeN : 0.0);
        printf("links(subtag 18): %zu, target container found in the same tile: %zu (%.1f%%)\n",
               linkN, linkFound, linkN ? 100.0 * (double)linkFound / (double)linkN : 0.0);
        return 0;
    }

    // --lines: one compact line per proxy (child subtags + payload sizes) so the
    // shape vocabulary can be aggregated with sort|uniq -c.
    if (argc > 2 && std::string(argv[2]) == "--lines") {
        const std::string ldir = argc > 1 ? argv[1] : "original/ENV";
        std::vector<std::string> lfiles;
        DIR* ld = opendir(ldir.c_str());
        if (!ld) return 1;
        while (struct dirent* e = readdir(ld)) {
            std::string n = e->d_name;
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".b3d") == 0) lfiles.push_back(n);
        }
        closedir(ld);
        std::sort(lfiles.begin(), lfiles.end());
        for (const std::string& name : lfiles) {
            B3D t;
            if (!t.load(ldir + "/" + name)) continue;
            t.buildScene();
            FILE* f = fopen((ldir + "/" + name).c_str(), "rb");
            if (!f) continue;
            std::vector<uint8_t> raw;
            fseek(f, 0, SEEK_END);
            raw.resize((size_t)ftell(f));
            fseek(f, 0, SEEK_SET);
            if (fread(raw.data(), 1, raw.size(), f) != raw.size()) { fclose(f); continue; }
            fclose(f);
            std::function<void(const B3D::Node&)> scan = [&](const B3D::Node& n) {
                if (n.name.rfind("hit_", 0) != 0) {
                    for (const auto& k : n.kids) scan(k);
                    return;
                }
                std::string shape;
                for (const auto& k : n.kids) {
                    if (k.subtag == 9) {
                        float nx, nz, ny, d;
                        memcpy(&nx, &k.params[4], 4); memcpy(&nz, &k.params[5], 4);
                        memcpy(&ny, &k.params[6], 4); memcpy(&d, &k.params[7], 4);
                        char b[64];
                        snprintf(b, sizeof b, "plane(%.3f,%.3f,%.3f,%.1f)", nx, nz, ny, d);
                        shape += b;
                    } else if (k.subtag == 20 || k.subtag == 23) {
                        // header dwords before the vertices; the count sat at +16 in
                        // every sample so far - print it and the payload length
                        uint32_t hdr[8] = {0};
                        for (int i = 0; i < 8 && k.off + 40 + 4 * (size_t)i + 4 <= k.end; ++i)
                            memcpy(&hdr[i], raw.data() + k.off + 40 + 4 * (size_t)i, 4);
                        char b[80];
                        snprintf(b, sizeof b, "poly%u[h4=%u,%u,%u,%u len=%zu]", k.subtag, hdr[0],
                                 hdr[4], hdr[5], hdr[6], k.end > k.off + 40 ? k.end - k.off - 40 : 0);
                        shape += b;
                    } else if (k.subtag == 18) {
                        shape += "link->" + inlineName(k.params, 12);
                    } else {
                        char b[32];
                        snprintf(b, sizeof b, "s%u", k.subtag);
                        shape += b;
                    }
                    shape += ",";
                }
                printf("%s %s %s\n", name.c_str(), n.name.c_str(), shape.c_str());
                for (const auto& k : n.kids) scan(k);
            };
            for (const auto& n : t.tree()) scan(n);
        }
        return 0;
    }

    // --summary: the vocabulary of the proxies across every tile.
    if (argc > 2 && std::string(argv[2]) == "--summary") {
        const std::string sdir = argc > 1 ? argv[1] : "original/ENV";
        std::vector<std::string> sfiles;
        DIR* sd = opendir(sdir.c_str());
        if (!sd) { fprintf(stderr, "no %s\n", sdir.c_str()); return 1; }
        while (struct dirent* e = readdir(sd)) {
            std::string n = e->d_name;
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".b3d") == 0) sfiles.push_back(n);
        }
        closedir(sd);
        std::sort(sfiles.begin(), sfiles.end());
        std::map<uint32_t, size_t> subTagCount;
        std::map<std::string, size_t> targets;
        std::map<std::string, std::string> targetTile;
        size_t inLink = 0, inPoly = 0, terrainOnly = 0;
        for (const std::string& name : sfiles) {
            B3D t;
            if (!t.load(sdir + "/" + name)) continue;
            std::map<std::string, size_t> submeshByName;
            t.buildScene();
            for (const auto& sm : t.submeshes()) submeshByName[sm.name]++;
            std::function<void(const B3D::Node&)> scan = [&](const B3D::Node& n) {
                if (n.name.rfind("hit_", 0) != 0) {
                    for (const auto& k : n.kids) scan(k);
                    return;
                }
                for (const auto& k : n.kids) {
                    ++subTagCount[k.subtag];
                    if (k.subtag == 18 && k.params.size() >= 20) {
                        ++inLink;
                        const std::string to = inlineName(k.params, 12);
                        if (!to.empty()) {
                            ++targets[to];
                            if (!targetTile.count(to)) targetTile[to] = name;
                        }
                    } else if (k.subtag == 20 || k.subtag == 23) {
                        ++inPoly;
                    } else if (k.subtag == 40) {
                        ++terrainOnly;
                    }
                    for (const auto& kk : k.kids) scan(kk);
                }
            };
            for (const auto& n : t.tree()) scan(n);
        }
        printf("child subtags inside hit_* containers:\n");
        for (const auto& kv : subTagCount)
            printf("  subtag %-3u %-14s %zu\n", kv.first, subTagName(kv.first), kv.second);
        printf("links=%zu polygons=%zu terrain-records=%zu distinct targets=%zu\n", inLink, inPoly,
               terrainOnly, targets.size());
        std::vector<std::pair<size_t, std::string>> ranked;
        for (const auto& kv : targets) ranked.push_back({kv.second, kv.first});
        std::sort(ranked.rbegin(), ranked.rend());
        for (size_t i = 0; i < ranked.size() && i < 30; ++i)
            printf("  target %-34s x%-4zu tile %s\n", ranked[i].second.c_str(), ranked[i].first,
                   targetTile[ranked[i].second].c_str());
        return 0;
    }

    const std::string dir = argc > 1 ? argv[1] : "original/ENV";
    const std::string onlyTile = argc > 2 ? argv[2] : "";
    const std::string onlyId = argc > 3 ? argv[3] : "";
    std::vector<std::string> files;
    DIR* d = opendir(dir.c_str());
    if (!d) { fprintf(stderr, "no %s\n", dir.c_str()); return 1; }
    while (struct dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".b3d") == 0) files.push_back(n);
    }
    closedir(d);
    std::sort(files.begin(), files.end());

    size_t proxies = 0, links = 0, boxes = 0, planes = 0, terrains = 0;
    for (const std::string& name : files) {
        if (!onlyTile.empty() && name != onlyTile) continue;
        const std::string path = dir + "/" + name;
        B3D t;
        if (!t.load(path)) continue;
        t.buildScene();
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) continue;
        std::vector<uint8_t> raw;
        fseek(f, 0, SEEK_END);
        raw.resize((size_t)ftell(f));
        fseek(f, 0, SEEK_SET);
        size_t got = fread(raw.data(), 1, raw.size(), f);
        (void)got;
        fclose(f);

        std::function<void(const B3D::Node&)> scan = [&](const B3D::Node& n) {
            if (n.name.rfind("hit_", 0) != 0) {
                for (const auto& k : n.kids) scan(k);
                return;
            }
            const std::string id = n.name.substr(4);          // obj_ap_048 / road_ap_048
            if (!onlyId.empty() && id.find(onlyId) == std::string::npos) {
                for (const auto& k : n.kids) scan(k);
                return;
            }
            std::string core = id;
            const size_t us = core.find('_');
            if (us != std::string::npos) core = core.substr(us + 1);   // ap_048
            size_t vis = 0;
            float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
            for (const auto& sm : t.submeshes()) {
                if (sm.name.find(core) == std::string::npos || sm.pos.size() < 3) continue;
                ++vis;
                for (size_t i = 0; i + 2 < sm.pos.size(); i += 3) {
                    x0 = std::min(x0, sm.pos[i]); x1 = std::max(x1, sm.pos[i]);
                    z0 = std::min(z0, sm.pos[i + 1]); z1 = std::max(z1, sm.pos[i + 1]);
                    y0 = std::min(y0, sm.pos[i + 2]); y1 = std::max(y1, sm.pos[i + 2]);
                }
            }
            ++proxies;
            printf("\n=== %s / %s subtag=%u kids=%zu\n", name.c_str(), n.name.c_str(), n.subtag,
                   n.kids.size());
            if (vis)
                printf("  visible '%s': %zu submeshes bbox x[%.1f..%.1f] z[%.1f..%.1f] y[%.1f..%.1f]\n",
                       core.c_str(), vis, x0, x1, z0, z1, y0, y1);
            else
                printf("  visible '%s': (none)\n", core.c_str());
            for (const auto& k : n.kids) {
                printf("  child %-24s subtag=%-2u (%s) params=%zu mesh=%d kids=%zu\n",
                       k.name.empty() ? "(unnamed)" : k.name.c_str(), k.subtag,
                       subTagName(k.subtag), k.params.size(), k.meshIndex, k.kids.size());
                for (size_t i = 0; i < k.params.size(); ++i) {
                    float fl;
                    memcpy(&fl, &k.params[i], 4);
                    char ascii[5] = {0};
                    for (int b = 0; b < 4; ++b) {
                        const unsigned char c = (unsigned char)((k.params[i] >> (8 * b)) & 0xFF);
                        ascii[b] = (c >= 32 && c < 127) ? (char)c : '.';
                    }
                    printf("      p%-2zu 0x%08x %14.4f  '%s'\n", i, k.params[i], fl, ascii);
                }
                if (k.subtag == 18 && k.params.size() >= 20) {
                    printf("      link from '%s' to '%s'\n", inlineName(k.params, 4).c_str(),
                           inlineName(k.params, 12).c_str());
                    ++links;
                }
                if (k.subtag == 20 || k.subtag == 23) {
                    ++boxes;
                    printf("      raw payload:\n");
                    dumpDwords(raw.data(), k.off + 40, std::min(k.end, raw.size()), "        ");
                }
                if (k.subtag == 9) ++planes;
                if (k.subtag == 40) {
                    ++terrains;
                    printf("      raw payload (subtag 40):\n");
                    dumpDwords(raw.data(), k.off + 40, std::min(k.end, raw.size()), "        ");
                }
                for (const auto& kk : k.kids) {
                    printf("      child-of-child %-20s subtag=%-2u params=%zu mesh=%d\n",
                           kk.name.empty() ? "(unnamed)" : kk.name.c_str(), kk.subtag,
                           kk.params.size(), kk.meshIndex);
                    for (size_t i = 0; i < kk.params.size(); ++i) {
                        float fl; memcpy(&fl, &kk.params[i], 4);
                        printf("          p%-2zu 0x%08x %14.4f\n", i, kk.params[i], fl);
                    }
                    if (kk.subtag == 20 || kk.subtag == 23)
                        dumpDwords(raw.data(), kk.off + 40, std::min(kk.end, raw.size()), "        ");
                    for (const auto& k3 : kk.kids)
                        printf("          deeper %-16s subtag=%u kids=%zu\n",
                               k3.name.empty() ? "(unnamed)" : k3.name.c_str(), k3.subtag,
                               k3.kids.size());
                    scan(kk);
                }
            }
        };
        for (const auto& n : t.tree()) scan(n);
    }
    printf("\ntotal hit_* containers=%zu (links=%zu scan-boxes=%zu planes=%zu terrain=%zu)\n",
           proxies, links, boxes, planes, terrains);
    return 0;
}

// tools/road_points.cpp - dump a subsample of the world's road-material vertices
// (b3d order x, z, height) so the .way road graph can be checked against the
// authored asphalt by tools/road_check.py.  Build:
//   c++ -std=c++17 -O2 -Isrc tools/road_points.cpp src/world/B3D.cpp -o build/road_points
#include "world/B3D.h"
#include <dirent.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace ht2;

static bool roadish(const std::string& m) {
    std::string s = m;
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s.find("road") != std::string::npos || s.find("asfalt") != std::string::npos ||
           s.find("asphalt") != std::string::npos;
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "original/ENV";
    const int stride = argc > 2 ? atoi(argv[2]) : 1;
    DIR* d = opendir(dir.c_str());
    if (!d) { fprintf(stderr, "cannot open %s\n", dir.c_str()); return 1; }
    std::vector<std::string> files;
    while (struct dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".b3d") == 0) files.push_back(dir + "/" + n);
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    for (const std::string& p : files) {
        B3D b;
        if (!b.load(p)) continue;
        b.buildScene();
        std::string base = p.substr(p.find_last_of('/') + 1);
        base = base.substr(0, base.size() - 4);
        size_t tris = 0, written = 0;
        for (const auto& sm : b.submeshes()) {
            if (!roadish(sm.material)) continue;
            const size_t nv = sm.pos.size() / 3;
            for (size_t v = 0; v < nv; v += (size_t)stride) {
                printf("%s %.2f %.2f %.2f\n", base.c_str(),
                       sm.pos[v * 3 + 0], sm.pos[v * 3 + 1], sm.pos[v * 3 + 2]);
                ++written;
            }
            for (size_t t = 0; t + 2 < sm.idx.size(); t += 3) ++tris;
        }
        if (tris) fprintf(stderr, "%s: %zu road tris, %zu verts dumped\n", base.c_str(), tris, written);
    }
    return 0;
}

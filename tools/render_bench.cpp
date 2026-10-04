// Standalone render benchmark for the world draw path.
//
// It assembles the same world the game builds (all ENV/*.b3d tiles + the .way
// ground map + the terrain mesh), then renders the road/curve view N times with
// the real Renderer::draw() + drawScene() calls and reports ms/frame.  Used to
// check that anisotropic filtering does not cost measurable frame time.
//
// Build (reuses the engine's object files, see tools/bench.sh):
//   clang++ -std=c++17 -Isrc tools/render_bench.cpp <objs except main/App> ...
//   HT2_ANISO=1|8 tools/render_bench [frames]
#include "gfx/Renderer.h"
#include "world/B3D.h"
#include "world/Terrain.h"
#include "world/Way.h"
#include "world/GroundMap.h"
#include "world/SceneTextures.h"
#include "util/Log.h"
#include <SDL.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include <vector>

using namespace ht2;

int main(int argc, char** argv) {
    const int frames = argc > 1 ? atoi(argv[1]) : 120;
    const std::string data = argc > 2 ? argv[2] : "original";
    const bool offscreen = getenv("HT2_BENCH_VISIBLE") == nullptr;

    Terrain terrain;
    Way way;
    GroundMap ground;
    if (!terrain.load(data + "/MTR/terrain0.raw")) LOG("bench: no terrain");
    if (!way.load(data + "/ENV/aa.way")) LOG("bench: no way");

    std::vector<std::string> files;
    DIR* d = opendir((data + "/ENV").c_str());
    if (!d) { LOG("bench: no ENV dir"); return 1; }
    while (struct dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".b3d") == 0)
            files.push_back(data + "/ENV/" + n);
    }
    closedir(d);
    std::sort(files.begin(), files.end());

    std::vector<B3D::SubMesh> meshes;
    for (const std::string& p : files) {
        B3D t;
        if (!t.load(p)) continue;
        t.buildScene();
        for (auto& sm : t.submeshesMutable()) meshes.push_back(std::move(sm));
    }
    std::vector<float> roadPts;
    for (const auto& sg : way.segments())
        for (const auto& p : sg.pts) {
            roadPts.push_back((float)p.x);
            roadPts.push_back((float)p.z);
            roadPts.push_back((float)p.y);
        }
    ground.build(meshes, roadPts, 12.0f, &terrain);
    LOG("bench: %zu tiles, %zu submeshes", files.size(), meshes.size());

    Renderer r;
    r.setTerrain(&terrain);
    r.setRoads(&way);
    r.setGroundMap(&ground);
    r.setGroundEnabled(true);
    r.setUseRawTerrain(false);
    r.setSky(0.44f, 0.60f, 0.82f);
    r.setFog(2600.0f);
    r.setSun(0.40f, 0.74f, 0.34f);
    r.setAmbient(0.78f);
    if (!r.init(1280, 720, offscreen)) { LOG("bench: init failed"); return 2; }

    SceneTextures::loadPalette(data + "/COMMON/common1.res");
    SceneTextures tex;
    for (const std::string& p : files) tex.load(r, p.substr(0, p.size() - 4));
    tex.load(r, data + "/COMMON/TRUCKS");
    r.setSceneMeshes(&meshes, &tex);
    for (const char* cand : {"grass1", "grass0", "grass2", "ground", "dirt", "ter_road"}) {
        unsigned t = tex.textureFor(cand);
        if (t) { r.setGroundTexture(t); break; }
    }

    typedef void (*PFN_Finish)(void);
    PFN_Finish finish = (PFN_Finish)SDL_GL_GetProcAddress("glFinish");

    // the measured road/curve view (same pose as tools/road_shimmer.py)
    float cam[5] = {580.0f, 30.0f, -70.0f, 174.5f, -3.0f};
    for (int i = 0; i < 8; ++i) { r.clear(); r.draw(cam[0], cam[1], cam[2], cam[3], cam[4]);
                                  r.drawScene(cam[0], cam[1], cam[2], cam[3], cam[4]); }
    if (finish) finish();

    double best = 1e9, total = 0;
    std::vector<double> all;
    all.reserve(frames);
    for (int i = 0; i < frames; ++i) {
        cam[3] = 174.5f + (i % 16) * 0.02f;          // avoid an identical-frame fast path
        auto t0 = std::chrono::steady_clock::now();
        r.clear();
        r.draw(cam[0], cam[1], cam[2], cam[3], cam[4]);
        r.drawScene(cam[0], cam[1], cam[2], cam[3], cam[4]);
        if (finish) finish();
        double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0).count();
        best = std::min(best, ms);
        total += ms;
        all.push_back(ms);
    }
    std::sort(all.begin(), all.end());
    const double median = all[all.size() / 2];
    LOG("bench: %d frames, renderer '%s', aniso %.0fx: mean %.2f ms/frame, median %.2f, best %.2f",
        frames, r.gpuRenderer().c_str(), r.anisotropy(), total / frames, median, best);
    std::printf("BENCH mean_ms=%.3f median_ms=%.3f best_ms=%.3f aniso=%.0f renderer=%s\n",
                total / frames, median, best, r.anisotropy(), r.gpuRenderer().c_str());
    r.shutdown();
    return 0;
}

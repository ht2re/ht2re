// Standalone cockpit + rear-view-mirror harness (docs/mirror.md).
//
// Renders the cockpit view of a ZilR on the spawn pose used by tools/shot_test.py
// with and without the mirror, and with the truck driven forward, writing one
// BMP per frame so tools/mirror_check.sh can compare the mirror region.  It
// assembles the world exactly like tools/render_bench.cpp (all ENV tiles + the
// .way ground map + the terrain mesh), then uses the real Cockpit and Mirror
// classes and the Renderer's public draw path - no App.cpp is involved, which
// keeps the feature testable while App.cpp is owned by another change.
//
// Build + run: tools/mirror_check.sh [frames] [data-dir]
#include "gfx/Renderer.h"
#include "gfx/Mirror.h"
#include "game/Cockpit.h"
#include "game/Vehicle.h"
#include "world/B3D.h"
#include "world/Terrain.h"
#include "world/Way.h"
#include "world/GroundMap.h"
#include "world/SceneTextures.h"
#include "util/Log.h"
#include <SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

using namespace ht2;

namespace {

struct Args {
    std::string data = "original";
    std::string out = "shots/mirror";
    std::string model = "ZilR";
    std::string pos = "top";
    std::string size = "M";
    float spawnX = 4700.0f, spawnZ = -5700.0f, spawnYaw = 180.0f;
    float drive = 3.0f;      // seconds at full throttle for the "truck moved" frame
    int frames = 60;         // timing loop length
    bool visible = false;    // --visible opens a real window (debugging)
};

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto next = [&]() { return (i + 1 < argc) ? argv[++i] : ""; };
        if (k == "--data") a.data = next();
        else if (k == "--out") a.out = next();
        else if (k == "--model") a.model = next();
        else if (k == "--pos") a.pos = next();
        else if (k == "--size") a.size = next();
        else if (k == "--spawn") { float x, z, y; if (sscanf(next(), "%f,%f,%f", &x, &z, &y) == 3) { a.spawnX = x; a.spawnZ = z; a.spawnYaw = y; } }
        else if (k == "--drive") a.drive = (float)atof(next());
        else if (k == "--frames") a.frames = atoi(next());
        else if (k == "--visible") a.visible = true;
    }
    return a;
}

// App::viewPose() for viewMode_ == 1 (cockpit), including its fallback pose.
void cockpitEye(const Cockpit& ck, const Vehicle& v, float& ex, float& ey, float& ez,
                float& yaw, float& pitch) {
    float lx = -0.75f, ly = 2.35f, lz = 2.10f, extraPitch = 0.0f;
    if (!ck.eyePose(lx, ly, lz, extraPitch)) lx = -0.75f, ly = 2.35f, lz = 2.10f;
    const Mat4 vm = v.modelMatrix();
    const float* m = vm.m;
    ex = m[0] * lx + m[4] * ly + m[8] * lz + m[12];
    ey = m[1] * lx + m[5] * ly + m[9] * lz + m[13];
    ez = m[2] * lx + m[6] * ly + m[10] * lz + m[14];
    const float fwdX = m[8], fwdY = m[9], fwdZ = m[10];
    yaw = atan2f(fwdX, -fwdZ) * 180.0f / 3.14159265f;
    const float len = sqrtf(fwdX * fwdX + fwdY * fwdY + fwdZ * fwdZ);
    pitch = (len > 1e-6f) ? asinf(fwdY / len) * 180.0f / 3.14159265f : 0.0f;
    pitch += extraPitch;
}

// One App::drawWorld() frame for the cockpit view (world + vehicle + cockpit
// instruments + optional mirror).  `mirror` may be null or hidden.
void drawCockpitFrame(Renderer& r, Mirror* mirror, Cockpit& ck, const Vehicle& v,
                      bool haveScene, bool haveTrucks) {
    float cx, cy, cz, camYaw, camPitch;
    cockpitEye(ck, v, cx, cy, cz, camYaw, camPitch);
    r.clear();
    r.setNearPlane(ck.loaded() ? 2.0f : 0.4f);
    const bool instruments = ck.loaded();
    r.setVehicleTwoSided(!instruments);
    r.draw(cx, cy, cz, camYaw, camPitch);
    if (haveScene) r.drawScene(cx, cy, cz, camYaw, camPitch);
    if (haveTrucks && !instruments) {
        r.setModel(v.modelMatrix());
        r.drawVehicleScene(cx, cy, cz, camYaw, camPitch);
        r.clearModel();
    }
    ck.setVisible(true);
    if (instruments) { r.clearDepth(); r.setNearPlane(0.4f); }
    ck.draw(r, v.modelMatrix(), v, cx, cy, cz, camYaw, camPitch);
    if (mirror) mirror->draw(r, v.modelMatrix());
    r.setNearPlane(2.0f);
}

} // namespace

int main(int argc, char** argv) {
    const Args a = parse(argc, argv);
    Terrain terrain;
    Way way;
    GroundMap ground;
    if (!terrain.load(a.data + "/MTR/terrain0.raw")) LOG("mirror_check: no terrain");
    if (!way.load(a.data + "/ENV/aa.way")) LOG("mirror_check: no way");

    std::vector<std::string> files;
    DIR* d = opendir((a.data + "/ENV").c_str());
    if (!d) { LOG("mirror_check: no ENV dir"); return 1; }
    while (struct dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".b3d") == 0)
            files.push_back(a.data + "/ENV/" + n);
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
    LOG("mirror_check: %zu tiles, %zu submeshes", files.size(), meshes.size());

    B3D trucks;
    const bool haveTrucks = trucks.load(a.data + "/COMMON/TRUCKS.B3D") &&
                            trucks.buildNamedScene(a.model);
    if (!haveTrucks) LOG("mirror_check: no %s in COMMON/TRUCKS.B3D", a.model.c_str());

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
    if (!r.init(1280, 720, !a.visible)) { LOG("mirror_check: init failed"); return 2; }

    SceneTextures::loadPalette(a.data + "/COMMON/common1.res");
    SceneTextures tex, vehicleTex;
    for (const std::string& p : files) tex.load(r, p.substr(0, p.size() - 4));
    tex.load(r, a.data + "/COMMON/TRUCKS");
    vehicleTex.load(r, a.data + "/COMMON/TRUCKS");
    r.setSceneMeshes(&meshes, &tex);
    if (haveTrucks) r.setVehicleMeshes(&trucks.submeshes(), &vehicleTex);
    for (const char* cand : {"grass1", "grass0", "grass2", "ground", "dirt", "ter_road"}) {
        unsigned t = tex.textureFor(cand);
        if (t) { r.setGroundTexture(t); break; }
    }

    Cockpit ck;
    if (!ck.load(r, a.data, a.model))
        LOG("mirror_check: cockpit not loaded for %s", a.model.c_str());

    Mirror mirror;
    const bool haveMirror = mirror.load(r, a.data, &trucks.anchorList, a.model);
    if (a.pos == "left") mirror.setPos(Mirror::PosLeft);
    else if (a.pos == "right") mirror.setPos(Mirror::PosRight);
    else mirror.setPos(Mirror::PosTop);
    if (a.size == "S") mirror.setSize(Mirror::SizeS);
    else if (a.size == "L") mirror.setSize(Mirror::SizeL);
    else if (a.size == "H") mirror.setSize(Mirror::SizeH);
    else mirror.setSize(Mirror::SizeM);

    Vehicle v;
    v.setGround(&ground);
    v.reset(a.spawnX, a.spawnZ, a.spawnYaw);
    Vehicle::Input idle;
    for (int i = 0; i < 80; ++i) v.update(idle, 1.0f / 60.0f);   // settle on the road

    typedef void (*PFN_Finish)(void);
    PFN_Finish finish = (PFN_Finish)SDL_GL_GetProcAddress("glFinish");

    const std::string outNo = a.out + "_nomirror.bmp";
    const std::string outYes = a.out + "_mirror.bmp";

    // (a) without the mirror
    mirror.setVisible(false);
    drawCockpitFrame(r, &mirror, ck, v, true, haveTrucks);
    if (finish) finish();
    r.screenshot(outNo);

    // (b) with the mirror, same camera and vehicle pose
    mirror.setVisible(true);
    drawCockpitFrame(r, &mirror, ck, v, true, haveTrucks);
    if (finish) finish();
    r.screenshot(outYes);

    // (b2) the same frame without the horizontal flip (the harness proves the
    // mirror really is the horizontal mirror of the unflipped reflection)
    const std::string outNoFlip = a.out + "_mirror_noflip.bmp";
    mirror.setFlipU(false);
    drawCockpitFrame(r, &mirror, ck, v, true, haveTrucks);
    if (finish) finish();
    r.screenshot(outNoFlip);
    mirror.setFlipU(true);

    int fx, fy, fw, fh, gx, gy, gw, gh;
    mirror.frameRect(fx, fy, fw, fh);
    mirror.glassRect(gx, gy, gw, gh);
    float mx, my, mz, myaw, mpitch;
    mirror.camera(mx, my, mz, myaw, mpitch);
    std::printf("MIRROR loaded=%d anchor=%s frame=%d,%d,%d,%d glass=%d,%d,%d,%d "
                "target=%dx%d flip=%d camera=%.2f,%.2f,%.2f,%.1f,%.1f\n",
                haveMirror ? 1 : 0, mirror.anchorName().c_str(),
                fx, fy, fw, fh, gx, gy, gw, gh, mirror.targetWidth(), mirror.targetHeight(),
                mirror.flipU() ? 1 : 0, mx, my, mz, myaw, mpitch);
    std::printf("COCKPIT brand=%s needles=%d anchor_t=%.3f,%.3f,%.3f\n",
                ck.brand().c_str(), ck.needleCount(), mx, my, mz);

    // cost: the same frame with the mirror hidden (the app's other view modes
    // call draw() with setVisible(false): it must cost one branch) and drawn
    const int frames = a.frames > 0 ? a.frames : 60;
    double noMs = 0, yesMs = 0, passMs = 0;
    for (int i = 0; i < 4; ++i) {                       // warm-up
        drawCockpitFrame(r, &mirror, ck, v, true, haveTrucks);
    }
    if (finish) finish();
    mirror.setVisible(false);
    for (int i = 0; i < frames; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        drawCockpitFrame(r, &mirror, ck, v, true, haveTrucks);
        if (finish) finish();
        noMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    mirror.setVisible(true);
    for (int i = 0; i < frames; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        drawCockpitFrame(r, &mirror, ck, v, true, haveTrucks);
        if (finish) finish();
        yesMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        passMs += mirror.lastCostMs();
    }
    std::printf("COST frames=%d nomirror_ms=%.3f mirror_ms=%.3f delta_ms=%.3f "
                "reflection_pass_ms=%.3f target=%dx%d\n",
                frames, noMs / frames, yesMs / frames, (yesMs - noMs) / frames,
                passMs / frames, mirror.targetWidth(), mirror.targetHeight());

    // (c) the truck drives off: same mirror, different world in front of it
    const std::string outMoved = a.out + "_mirror_moved.bmp";
    const std::string outMovedNo = a.out + "_nomirror_moved.bmp";
    Vehicle::Input driveIn;
    driveIn.throttle = 1.0f;
    const int steps = (int)(a.drive * 60.0f);
    for (int i = 0; i < steps; ++i) v.update(driveIn, 1.0f / 60.0f);
    mirror.setVisible(false);
    drawCockpitFrame(r, &mirror, ck, v, true, haveTrucks);
    if (finish) finish();
    r.screenshot(outMovedNo);
    mirror.setVisible(true);
    drawCockpitFrame(r, &mirror, ck, v, true, haveTrucks);
    if (finish) finish();
    r.screenshot(outMoved);
    std::printf("MOVED pos=%.1f,%.1f yaw=%.1f speed_kmh=%.1f odo=%.1f\n",
                v.x(), v.z(), v.yaw(), v.speedKmh(), v.odometer());
    std::printf("WROTE %s %s %s %s %s\n", outNo.c_str(), outYes.c_str(), outNoFlip.c_str(),
                outMovedNo.c_str(), outMoved.c_str());

    mirror.unload(r);
    r.shutdown();
    return haveMirror ? 0 : 3;
}

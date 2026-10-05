// Regression for right-hand lanes in the mirrored D3D world and bend offsets.
// c++ -std=c++17 -O2 -Isrc tools/traffic_lane_test.cpp src/game/Traffic.cpp \
//     src/game/Collision.cpp src/world/Way.cpp -o /tmp/ht2-lane-test
#include "game/Traffic.h"
#include "gfx/Math3D.h"
#include <cmath>
#include <cstdio>

using namespace ht2;
static int failures = 0;
static void check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

int main() {
    constexpr float pi = 3.14159265358979f;
    for (float yaw : {0.f, 45.f, 90.f, 180.f}) {
        const float a = yaw * pi / 180.f;
        const float fx = std::sin(a), fz = std::cos(a);
        Way::Seg road;
        road.pts = {{-200 * fx, 10, -200 * fz}, {200 * fx, 10, 200 * fz}};
        Traffic tr;
        tr.build({road}, {});
        tr.setModels({0});
        for (int dir : {-1, 1}) {
            const float qx = dir * fz * 2, qz = -dir * fx * 2;
            float x, z, h, heading; int edge;
            const bool found = tr.nearestLane(qx, qz, x, z, h, heading, edge);
            check(found && std::hypot(x-qx, z-qz) < .001f,
                  "nearest lane retains the requested right-side position");
            check(found && std::cos(heading*pi/180)*fz*dir +
                          std::sin(heading*pi/180)*fx*dir > .999f,
                  "right-side lane has the correct travel direction");
            check(found && tr.distanceToLane(edge, dir, qx, qz) < .001f &&
                          tr.distanceToLane(edge, -dir, qx, qz) > 3.99f,
                  "lane distance distinguishes opposing lanes");
        }
        tr.spawn(0, 0, 150, 1);
        check(tr.cars().size() == 1, "spawned a test car");
        for (const auto& car : tr.cars()) {
            check((car.x*fz-car.z*fx)*car.dir > 1.99f,
                  "spawn uses right-hand lane, not merely either side of centre");
            const float ca = car.yaw*pi/180;
            const Mat4 view = Mat4::lookAt(0, 10, 0, std::sin(ca), 10, std::cos(ca));
            Mat4 proj = Mat4::perspective(60, 1, 2, 1800);
            proj.m[0] = -proj.m[0];
            check(proj.m[0]*(view.m[0]*car.x + view.m[8]*car.z) > 0,
                  "actual mirrored projection places car on screen right");
        }
    }
    for (float turn : {-90.f, -45.f, 45.f, 90.f}) {
    const float turnRad = turn*pi/180;
    Way::Seg bend;
    bend.pts = {{0,10,-150}, {0,10,0}, {150*std::sin(turnRad),10,150*std::cos(turnRad)}};
    Traffic tr;
    tr.build({bend}, {});
    tr.setModels({0});
    tr.spawn(0, 0, 130, 1);
    int bendSamples = 0, transitions = 0;
    bool laneOK = true, continuous = true;
    for (int tick = 0; tick < 900; ++tick) {
        const auto prev = tr.cars().front();
        tr.update(1.f/60, 0, 0, 130);
        const auto& c = tr.cars().front();
        const float moved = std::hypot(c.x-prev.x,c.z-prev.z);
        if (moved > 10) continue; // out-of-view recycling at the synthetic dead ends
        if (c.edge != prev.edge) ++transitions;
        continuous &= moved <= c.speed/60 + .5f;
        if (c.s > 8 && c.s < tr.edgeLength(c.edge)-8) continue;
        ++bendSamples;
        const float tangent = c.edge == 0 ? 0 : turnRad;
        const float offset = (c.x*std::cos(tangent)-c.z*std::sin(tangent))*c.dir;
        laneOK &= std::fabs(offset-c.lane) < .01f;
    }
    check(bendSamples > 0 && transitions > 0, "exercised a real bend transition");
    check(laneOK, "bend retains perpendicular lane offset");
    check(continuous, "bend has no lateral position jump");
    std::printf("BEND turn=%.0f samples=%d transitions=%d\n", turn, bendSamples, transitions);
    }
    std::printf("RESULT failures=%d\n", failures);
    return failures ? 1 : 0;
}

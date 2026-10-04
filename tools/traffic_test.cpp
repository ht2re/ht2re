// traffic_test.cpp -- headless harness for src/game/Traffic.{h,cpp}.
//
// Loads every original/ENV/*.way, builds the road graph, spawns AI cars around
// the player start and ticks the simulation, printing the same metrics as
// tools/traffic_sim.py -- but measured on the real C++ module.
//
// Build (it is not part of the app target on purpose):
//   c++ -std=c++17 -O2 -I src tools/traffic_test.cpp src/game/Traffic.cpp \
//       src/world/Way.cpp -o build/traffic_test
// Run:
//   ./build/traffic_test [dataDir] [cars] [ticks] [pathfinding 0|1]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <map>
#include <string>
#include <vector>

#include "../src/game/Traffic.h"

using namespace ht2;

static bool loadWorld(const std::string& dir, std::vector<Way::Seg>& segs,
                      std::map<std::string, Way::Node>& nodes) {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    std::vector<std::string> files;
    while (struct dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".way") == 0)
            files.push_back(dir + "/" + n);
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    for (const std::string& p : files) {
        Way w;
        if (!w.load(p)) continue;
        for (const auto& room : w.rooms)
            for (const auto& n : room.nodes)
                if (!n.name.empty()) nodes[n.name] = n;
        for (const auto& s : w.segments()) segs.push_back(s);
    }
    return true;
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "original/ENV";
    const int wantCars = argc > 2 ? atoi(argv[2]) : 300;
    const int ticks = argc > 3 ? atoi(argv[3]) : 900;
    const bool pathfinding = argc > 4 ? atoi(argv[4]) != 0 : true;
    const float dt = 1.0f / 30.0f;
    const float px = -3020.0f, pz = -4625.0f, radius = 900.0f;   // ENV/GAME.CNF start

    std::vector<Way::Seg> segs;
    std::map<std::string, Way::Node> nodes;
    if (!loadWorld(dir, segs, nodes)) { std::printf("no %s\n", dir.c_str()); return 1; }

    Traffic tr;
    tr.setSeed(0x1234567u);
    tr.setPathfinding(pathfinding);
    tr.build(segs, nodes);
    tr.setModels({0, 1, 2, 3, 4});
    tr.spawn(px, pz, radius, wantCars);

    std::printf("mode: %s | graph: %zu vertices, %zu edges | waypoints %zu/%zu resolved\n",
                pathfinding ? "shortest-path" : "straight-line-only",
                tr.vertices(), tr.edges(), tr.waypoints(), tr.waypointNames());
    std::printf("spawn: %zu cars | lane offset %.2f m\n", tr.cars().size(), tr.laneOffset());

    const size_t n = tr.cars().size();
    std::vector<int> prevEdge(n, -1), prevDir(n, 0), halt(n, 0), maxHalt(n, 0);
    std::vector<float> prevYaw(n, 0.0f), prevX(n, 0.0f), prevZ(n, 0.0f), prevS(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        const auto& c = tr.cars()[i];
        prevYaw[i] = c.yaw; prevX[i] = c.x; prevZ[i] = c.z; prevS[i] = c.s;
        prevEdge[i] = c.edge; prevDir[i] = c.dir;
    }

    size_t uturnSameEdge = 0, uturnJunction = 0, relocated = 0;
    size_t laneOk = 0, laneSamples = 0, oncomingOk = 0, oncoming = 0;
    double midDist = 0.0, midTime = 0.0, lastSpeed = 0.0, lastDist = 0.0, lastTime = 0.0;

    for (int t = 0; t < ticks; ++t) {
        tr.update(dt, px, pz, radius);
        const auto& cars = tr.cars();
        for (size_t i = 0; i < cars.size(); ++i) {
            const auto& c = cars[i];
            float dy = c.yaw - prevYaw[i];
            while (dy > 180.0f) dy -= 360.0f;
            while (dy < -180.0f) dy += 360.0f;
            const float moved = std::sqrt((c.x - prevX[i]) * (c.x - prevX[i]) +
                                          (c.z - prevZ[i]) * (c.z - prevZ[i]));
            // A driving step moves speed*dt along the road; a junction adds up
            // to 2*lane of lateral lane flip. Anything beyond that is a
            // streaming relocation, which is not a U-turn.
            const bool teleport = moved > c.speed * dt + 2.0 * tr.laneOffset() + 0.5;
            if (teleport) {
                ++relocated;
                halt[i] = 0;
            } else {
                const float ds = std::fabs(c.s - prevS[i]);
                if (c.edge == prevEdge[i] && c.dir != prevDir[i] && ds <= c.speed * dt + 0.5)
                    ++uturnSameEdge;
                else if (c.edge != prevEdge[i] && std::fabs(dy) > 150.0f) ++uturnJunction;
                if (moved < 0.03f) {                 // < ~0.9 m/s
                    ++halt[i];
                    if (halt[i] > maxHalt[i]) maxHalt[i] = halt[i];
                } else {
                    halt[i] = 0;
                }
            }
            prevYaw[i] = c.yaw; prevX[i] = c.x; prevZ[i] = c.z; prevS[i] = c.s;
            prevEdge[i] = c.edge; prevDir[i] = c.dir;
        }
        if (ticks > 300 && t == ticks - 301) {
            midDist = tr.stats().distance;
            midTime = tr.stats().time;
        }
        if (t == ticks - 1) {
            lastDist = tr.stats().distance;
            lastTime = tr.stats().time;
            // final state: lane adherence and oncoming separation
            for (size_t i = 0; i < cars.size(); ++i) {
                const auto& c = cars[i];
                const float off = tr.distanceToEdge(c.edge, c.x, c.z);
                ++laneSamples;
                if (std::fabs(off - c.lane) < 0.25f) ++laneOk;
                for (size_t j = i + 1; j < cars.size(); ++j) {
                    const auto& o = cars[j];
                    if (o.edge != c.edge || o.dir == c.dir) continue;
                    const float dx = c.x - o.x, dz = c.z - o.z;
                    const float sep = std::sqrt(dx * dx + dz * dz);
                    if (sep < 0.5f) continue;
                    ++oncoming;
                    // both on their own right of the centreline -> separated by ~2 lanes
                    if (sep >= 2.0f * std::min(c.lane, o.lane) - 0.25f) ++oncomingOk;
                }
            }
        }
    }
    const Traffic::Stats& s = tr.stats();
    if (ticks > 300) lastSpeed = (lastDist - midDist) / (lastTime - midTime);
    else lastSpeed = s.meanSpeed();

    size_t longHalt = 0, leaderlessHalt = 0;
    for (size_t i = 0; i < n; ++i) {
        if (maxHalt[i] >= 90) ++longHalt;                 // >= 3 s without moving
        const auto& c = tr.cars()[i];
        // "really stuck" = halted, nobody ahead on this edge, and not simply
        // waiting at a junction line for the next edge to free up.
        const bool atJunction = c.s > tr.edgeLength(c.edge) - 1.5f;
        bool leader = false;
        for (size_t j = 0; j < n && !leader; ++j) {
            const auto& o = tr.cars()[j];
            if (j == i || o.edge != c.edge || o.dir != c.dir) continue;
            const float gap = o.s - c.s;
            if (gap > 0.0f && gap < 20.0f) leader = true;
        }
        if (maxHalt[i] >= 90 && !leader && !atJunction) ++leaderlessHalt;
    }
    std::printf("sim: %d ticks @ %.4f s, %zu cars\n", ticks, (double)dt, n);
    std::printf("mean speed: %.2f m/s (%.1f km/h), last-300-ticks %.2f m/s\n",
                s.meanSpeed(), s.meanSpeed() * 3.6, lastSpeed);
    std::printf("lane: %zu/%zu cars within 0.25 m of their lane offset | "
                "oncoming separated: %zu/%zu\n", laneOk, laneSamples, oncomingOk, oncoming);
    std::printf("uturns: %zu (module) | same-edge dir flips %zu | junction turns >150deg %zu\n",
                s.uturns, uturnSameEdge, uturnJunction);
    std::printf("recycles: %zu | respawns: %zu | reroutes(timeout): %zu | stalls: %zu | "
                "arrivals: %zu | relocations seen %zu\n",
                s.recycles, s.respawns, s.reroutes, s.stalls, s.arrivals, relocated);
    std::printf("stuck: %zu cars >= 3 s under 0.9 m/s; %zu of them neither queued nor "
                "waiting at a junction\n", longHalt, leaderlessHalt);
    std::printf("distance: %.0f m | car-time: %.0f s | mean %.0f m driven per arrival\n",
                s.distance, s.time, s.arrivals ? s.distance / (double)s.arrivals : 0.0);
    for (size_t i = 0; i < std::min<size_t>(5, n); ++i) {
        const auto& c = tr.cars()[i];
        std::printf("  car %zu edge %d dir %+d s %.1f pos (%.1f, %.1f, %.1f) yaw %.0f "
                    "v %.1f lane %.2f dest %s\n",
                    i, c.edge, c.dir, (double)c.s, (double)c.x, (double)c.h, (double)c.z,
                    (double)c.yaw, (double)c.speed, (double)c.lane, c.dest.c_str());
    }
    return 0;
}

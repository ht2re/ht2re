// traffic_report.cpp -- headless measurement harness for the world AI
// (src/game/Traffic.{h,cpp}).
//
// It uses only the public Traffic API, so it builds against both the old and
// the new module and gives a before/after number for the same world, the same
// seed and the same scripted player.  The metrics mirror `--traffic-report`:
//
//   mean speed        stats().meanSpeed()
//   U-turns           stats().uturns + direction flips observed per car
//   off-road frames   frames a car was > 2 m off its lane line
//   pose jumps        frames a car moved more than speed*dt + 0.5 m
//                     (the old code stepped the lane offset sideways at a
//                     junction; the new lane path is continuous)
//   overlaps          pair-frames in which two car circles interpenetrated
//   contacts          pair-frames whose overlap *started* this frame
//   junction hits     contacts between cars that are on different edges
//   min gap           closest centre distance - body radius sum, over the run
//   queued            cars stopped in the player's lane within 30 m
//
// The harness also builds against the *pre-change* Traffic module (for the
// before/after numbers in docs/traffic_ai.md): compile that one with
// -DHT2_TRAFFIC_OLD and the contact / player measurements report 0, exactly the
// fields the old module did not have.
//
// Build (not part of the app target):
//   c++ -std=c++17 -O2 -Isrc tools/traffic_report.cpp src/game/Traffic.cpp \
//       src/game/Collision.cpp src/world/Way.cpp -o build/traffic_report
// Run:
//   ./build/traffic_report [dataDir] [seconds] [cars] [--diag] [--drive]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <set>
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
    std::string dir = "original/ENV";
    float seconds = 60.0f;
    int wantCars = 16;
    bool diag = false, drive = false, havePlayer = false;
    float ppx = 0, ppz = 0, ppyaw = 0;
    const char* pos[16];
    int npos = 0;
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-') {
            if (!strcmp(argv[i], "--diag")) diag = true;
            else if (!strcmp(argv[i], "--drive")) drive = true;
            else if (!strcmp(argv[i], "--player") && i + 1 < argc) {
                if (sscanf(argv[++i], "%f,%f,%f", &ppx, &ppz, &ppyaw) == 3) havePlayer = true;
            }
        } else if (npos < 3) pos[npos++] = argv[i];
    }
    if (npos > 0) dir = pos[0];
    if (npos > 1) seconds = (float)atof(pos[1]);
    if (npos > 2) wantCars = atoi(pos[2]);

    const float dt = 1.0f / 60.0f;
    const int steps = (int)(seconds / dt);
    // ENV/GAME.CNF start, the same point --traffic-report parks the player on.
    float px = -3020.0f, pz = -4625.0f, pyaw = 0.0f;
    const float radius = 900.0f;
    const float carR = 1.3f;                    // harness circle for both builds

    std::vector<Way::Seg> segs;
    std::map<std::string, Way::Node> nodes;
    if (!loadWorld(dir, segs, nodes)) { std::printf("no %s\n", dir.c_str()); return 1; }

    Traffic tr;
    tr.setSeed(0x51ED2701u);
    tr.build(segs, nodes);
    tr.setModels({0, 1, 2, 3, 4});
    tr.spawn(px, pz, radius, wantCars);
    std::printf("graph: %zu vertices, %zu edges | waypoints %zu/%zu | spawn %zu cars\n",
                tr.vertices(), tr.edges(), tr.waypoints(), tr.waypointNames(),
                tr.cars().size());

    if (havePlayer) {
        px = ppx; pz = ppz; pyaw = ppyaw;          // same pose for both builds
        std::printf("player parked at (%.0f, %.0f) yaw %.0f\n",
                    (double)px, (double)pz, (double)pyaw);
    }
#ifndef HT2_TRAFFIC_OLD
    else {
        // Park the scripted player in a lane of a long edge, exactly like
        // --traffic-report does.
        float lx = 0, lz = 0, lh = 0, lyaw = 0;
        int le = -1;
        if (tr.nearestLane(px, pz, lx, lz, lh, lyaw, le, 40.0f)) {
            px = lx; pz = lz; pyaw = lyaw;
            std::printf("player parked in the lane of edge %d at (%.0f, %.0f) yaw %.0f\n",
                        le, (double)px, (double)pz, (double)pyaw);
        }
    }
#endif
    const size_t n0 = tr.cars().size();
    std::vector<float> prevX(n0, 0), prevZ(n0, 0);
    std::vector<int> prevEdge(n0, -1), prevDir(n0, 0);
    std::vector<float> prevS(n0, 0);
    std::vector<std::vector<char>> touching(n0, std::vector<char>(n0, 0));
    for (size_t i = 0; i < n0; ++i) {
        prevX[i] = tr.cars()[i].x; prevZ[i] = tr.cars()[i].z;
        prevEdge[i] = tr.cars()[i].edge; prevDir[i] = tr.cars()[i].dir;
    }
    double dist0 = tr.stats().distance, time0 = tr.stats().time;
    size_t offRoad = 0, frames = 0, jumps = 0, overlaps = 0, contacts = 0, junctionHits = 0;
    size_t dirFlips = 0, playerContacts = 0;
    double maxJump = 0.0, minGap = 1e9;
    size_t sameEdgeOpp = 0, sameEdgeSame = 0, crossEdge = 0;
    size_t ovSame = 0, ovHead = 0, ovCross = 0;
    std::map<std::pair<int,int>,int> ovPair;
    double worstDepth = 0.0;
    double sumDepth = 0.0;

    for (int t = 0; t < steps; ++t) {
        // Scripted player: parked in its lane (or creeping straight ahead).
        if (drive) { px += 8.0f * dt; }
#ifndef HT2_TRAFFIC_OLD
        Traffic::Player p;
        p.valid = true;
        p.x = px; p.z = pz; p.yaw = pyaw;
        p.vx = drive ? 8.0f : 0.0f; p.vz = 0.0f;
        p.radius = 1.62f; p.length = 12.04f;   // the ZilR truck App parks there
        tr.setPlayer(p);
#endif
        tr.update(dt, px, pz, radius);

        const auto& cars = tr.cars();
        for (size_t i = 0; i < cars.size(); ++i) {
            const auto& c = cars[i];
            ++frames;
            const float dist = tr.distanceToEdge(c.edge, c.x, c.z);
            if (std::fabs(dist - c.lane) > 2.0f) ++offRoad;
            const float moved = std::sqrt((c.x - prevX[i]) * (c.x - prevX[i]) +
                                          (c.z - prevZ[i]) * (c.z - prevZ[i]));
            // A streaming relocation teleports a car tens of metres; only the
            // short pose jumps (the old junction lane step) are of interest.
            if (moved > c.speed * dt + 0.5f && moved < 12.0f) {
                ++jumps;
                if (jumps <= 6) {
                    std::printf("  JUMP t=%d car %zu moved %.2f v %.2f edge %d prevEdge %d s %.2f prevS %.2f pos (%.1f,%.1f) prev (%.1f,%.1f)\n",
                                t, i, (double)moved, (double)c.speed, c.edge, prevEdge[i],
                                (double)c.s, (double)prevS[i], (double)c.x, (double)c.z,
                                (double)prevX[i], (double)prevZ[i]);
                }
                if (moved > maxJump) {
                    maxJump = moved;
                    std::printf("  JUMP t=%d car %zu moved %.2f speed %.2f edge %d s %.2f "
                                "(len %.2f) prevS %.2f dir %+d turn %.1f\n",
                                t, i, (double)moved, (double)c.speed, c.edge, (double)c.s,
                                (double)tr.edgeLength(c.edge), (double)prevS[i], c.dir,
                                (double)c.turn);
                }
            }
            prevS[i] = c.s;
            if (c.edge == prevEdge[i] && c.dir != prevDir[i]) ++dirFlips;
            prevX[i] = c.x; prevZ[i] = c.z;
            prevEdge[i] = c.edge; prevDir[i] = c.dir;
        }
        for (size_t i = 0; i < cars.size(); ++i) {
            for (size_t j = i + 1; j < cars.size(); ++j) {
                const auto& a = cars[i];
                const auto& b = cars[j];
                const float d = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z));
                const float gap = d - 2.0f * carR;
                if (gap < minGap) minGap = gap;
                const bool hit = gap < 0.0f;
                if (hit) {
                    ++overlaps;
                    const std::pair<int,int> key{(int)std::min(i,j),(int)std::max(i,j)};
                    if (ovPair[key]++ == 0 && ovPair.size() <= 4) {
                        std::printf("  FIRST-OVERLAP t=%d pair(#%zu,#%zu) depth %.2f edges %d/%d "
                                    "dirs %+d/%+d s %.2f/%.2f lens %.2f/%.2f pos (%.1f,%.1f)/(%.1f,%.1f) "
                                    "dAB %.2f dBA %.2f dAA %.2f dBB %.2f\n",
                                    t, i, j, (double)-gap, a.edge, b.edge, a.dir, b.dir,
                                    (double)a.s, (double)b.s, (double)tr.edgeLength(a.edge),
                                    (double)tr.edgeLength(b.edge),
                                    (double)a.x, (double)a.z, (double)b.x, (double)b.z,
                                    (double)tr.distanceToEdge(b.edge, a.x, a.z),
                                    (double)tr.distanceToEdge(a.edge, b.x, b.z),
                                    (double)tr.distanceToEdge(a.edge, a.x, a.z),
                                    (double)tr.distanceToEdge(b.edge, b.x, b.z));
                    }
                    sumDepth += -gap;
                    if (a.edge == b.edge) { if (a.dir == b.dir) ++ovSame; else ++ovHead; }
                    else ++ovCross;
                    if (-gap > worstDepth) {
                        worstDepth = -gap;
                        std::printf("  OVERLAP t=%d (%.2f m) edges %d/%d dirs %+d/%+d s %.2f/%.2f "
                                    "speeds %.1f/%.1f pos (%.1f,%.1f)/(%.1f,%.1f)\n",
                                    t, (double)-gap, a.edge, b.edge, a.dir, b.dir,
                                    (double)a.s, (double)b.s, (double)a.speed, (double)b.speed,
                                    (double)a.x, (double)a.z, (double)b.x, (double)b.z);
                        std::printf("      A->edgeB %.2f m, B->edgeA %.2f m, A->edgeA %.2f, B->edgeB %.2f\n",
                                    (double)tr.distanceToEdge(b.edge, a.x, a.z),
                                    (double)tr.distanceToEdge(a.edge, b.x, b.z),
                                    (double)tr.distanceToEdge(a.edge, a.x, a.z),
                                    (double)tr.distanceToEdge(b.edge, b.x, b.z));
                    }
                    if (!touching[i][j]) {
                        ++contacts;
                        const bool cross = a.edge != b.edge;
                        if (cross) ++crossEdge;
                        else if (a.dir != b.dir) ++sameEdgeOpp;
                        else ++sameEdgeSame;
                        if (cross) ++junctionHits;      // crossing traffic at a junction
                    }
                }
                touching[i][j] = hit ? 1 : 0;
            }
            // player
            const auto& a = cars[i];
            const float dx = a.x - px, dz = a.z - pz;
            const float d = std::sqrt(dx * dx + dz * dz);
            if (d - (carR + 1.45f) < minGap) minGap = d - (carR + 1.45f);
            if (d < carR + 1.45f) ++playerContacts;
        }
    }
    // Cars queued behind the parked player (in its lane, stopped).
    const float pfx = std::sin(pyaw * 3.14159265f / 180.0f);
    const float pfz = std::cos(pyaw * 3.14159265f / 180.0f);
    size_t queued = 0;
    float nearest = 1e9f;
    for (const auto& c : tr.cars()) {
        const float dx = c.x - px, dz = c.z - pz;
        const float along = dx * pfx + dz * pfz;
        const float lat = dx * -pfz + dz * pfx;
        if (along < 0.0f || along > 30.0f || std::fabs(lat) > 4.0f) continue;
        if (c.speed < 1.0f) { ++queued; if (along < nearest) nearest = along; }
    }
    const Traffic::Stats& s = tr.stats();
    const double mean = (s.time - time0) > 0.0 ? (s.distance - dist0) / (s.time - time0) : 0.0;
    std::printf("mean speed %.2f m/s (%.1f km/h) | distance %.0f m over %.0f s\n",
                mean, mean * 3.6, s.distance - dist0, s.time - time0);
#ifdef HT2_TRAFFIC_OLD
    std::printf("uturns(module) %zu | observed dir flips %zu | dead-end 0\n",
                s.uturns, dirFlips);
#else
    std::printf("uturns(module) %zu | observed dir flips %zu | dead-end %zu\n",
                s.uturns, dirFlips, s.deadEndTurns);
#endif
    std::printf("off-road frames %zu of %zu (%.3f%%) | pose jumps %zu (max %.2f m) | "
                "height steps >0.5 m %zu (max %.2f m)\n",
                offRoad, frames, frames ? 100.0 * offRoad / frames : 0.0, jumps, maxJump,
                s.heightJumps, s.maxHeightStep);
    {
        std::vector<std::pair<int,std::pair<int,int>>> v;
        for (auto&kv : ovPair) v.push_back({kv.second, kv.first});
        std::sort(v.begin(), v.end(), [](auto&a, auto&b){return a.first>b.first;});
        std::printf("distinct overlapping pairs: %zu; worst:", v.size());
        for (size_t k = 0; k < v.size() && k < 5; ++k) std::printf(" (#%d,#%d)x%d", v[k].second.first, v[k].second.second, v[k].first);
        std::printf("\n");
    }
    std::printf("overlap breakdown: cross-edge %zu, head-on %zu, same-dir %zu, worst %.2f m\n",
                ovCross, ovHead, ovSame, worstDepth);
    std::printf("overlap pair-frames %zu | contacts %zu (cross-edge %zu, head-on %zu, "
                "same-dir %zu) | mean depth %.2f m\n",
                overlaps, contacts, crossEdge, sameEdgeOpp, sameEdgeSame,
                overlaps ? sumDepth / overlaps : 0.0);
    std::printf("junction hits %zu | player contact frames %zu | min gap %.2f m\n",
                junctionHits, playerContacts, minGap);
#ifdef HT2_TRAFFIC_OLD
    const double dmg = 0.0, dmgP = 0.0;
    const size_t deadEnd = 0;
#else
    const double dmg = s.damageDealt, dmgP = s.damageToPlayer;
    const size_t deadEnd = s.deadEndTurns;
#endif
    std::printf("stalls %zu | respawns %zu | reroutes %zu | recycles %zu | arrivals %zu\n",
                s.stalls, s.respawns, s.reroutes, s.recycles, s.arrivals);
    std::printf("damage dealt %.3f (player %.3f) | queued behind player %zu (nearest %.1f m)\n",
                dmg, dmgP, queued, queued ? (double)nearest : 0.0);
    std::printf("RESULT cars=%zu mean=%.2f uturns=%zu deadend=%zu offroad=%zu frames=%zu "
                "jumps=%zu maxjump=%.2f hjumps=%zu maxhstep=%.2f contacts=%zu overlaps=%zu "
                "junctionhits=%zu mingap=%.2f damage=%.3f playerdamage=%.3f queued=%zu\n",
                s.cars, mean, s.uturns, deadEnd, offRoad, frames, jumps, maxJump,
                s.heightJumps, s.maxHeightStep, contacts, overlaps, junctionHits,
                minGap > 1e8 ? 0.0 : minGap, dmg, dmgP, queued);
    if (diag) {
        for (size_t i = 0; i < std::min<size_t>(8, tr.cars().size()); ++i) {
            const auto& c = tr.cars()[i];
            std::printf("  car %zu edge %d dir %+d s %.1f (len %.1f) v %.1f model %d "
                        "pos (%.1f,%.1f,%.1f) yaw %.0f dmg %.2f\n",
                        i, c.edge, c.dir, (double)c.s, (double)tr.edgeLength(c.edge),
                        (double)c.speed, c.model, (double)c.x, (double)c.h, (double)c.z,
                        (double)c.yaw,
#ifndef HT2_TRAFFIC_OLD
                        (double)c.damage
#else
                        0.0
#endif
                        );
        }
    }
    return 0;
}

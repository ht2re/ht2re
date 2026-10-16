// Diagnostic-only probe (not part of the game build): why does an authored
// ambush fail to find an armable road position inside its [250,600] band?
//
//   c++ -std=c++17 -O1 -Isrc -o tools/out/ambush_debug tools/ambush_debug.cpp \
//       src/script/ScriptRules.cpp src/script/Sexp.cpp src/script/Sdte.cpp \
//       src/script/GameData.cpp src/world/Way.cpp
//   tools/out/ambush_debug original [AMBUSH_ID ...] 2>&1 | grep -v '^\[ht2re\]'
//
// It prints the road-graph shape (degree histogram, components, dead ends), the
// anchor link and the target vertex, then - for every road sample inside the
// band - which endpoint of the anchor link the shortest path arrives through.
// That is what decides the sign of dot(dir, V) in ScriptRuntime. Findings and
// the decoded original rule: docs/script_gameplay.md 10.5.1/10.5.2.
//
// It reaches into ScriptRuntime's private road graph through a test-only access
// shim; it is never linked into the game.
#define private public
#define protected public
#include "script/ScriptRules.h"
#undef private
#undef protected
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <dirent.h>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <vector>
using namespace ht2;

int main(int argc, char** argv) {
    const std::string data = argc > 1 ? argv[1] : "original";
    std::set<std::string> want;
    for (int i = 2; i < argc; ++i) want.insert(argv[i]);
    std::vector<Way::Seg> roads;
    std::vector<ScriptNodePos> nodes;
    DIR* dir = opendir((data + "/ENV").c_str());
    if (!dir) return 1;
    std::vector<std::string> paths;
    while (auto* e = readdir(dir)) {
        std::string n = e->d_name;
        if (n.size() > 4 && n.substr(n.size() - 4) == ".way") paths.push_back(n);
    }
    closedir(dir);
    std::sort(paths.begin(), paths.end());
    for (const auto& p : paths) {
        Way w;
        if (!w.load(data + "/ENV/" + p)) return 1;
        roads.insert(roads.end(), w.segs.begin(), w.segs.end());
        for (const auto& r : w.rooms)
            for (const auto& n : r.nodes) nodes.push_back({n.name, n.pos.x, n.pos.z, n.pos.y});
    }
    GameData original;
    original.load(data);
    const auto actualRoads = scriptRoadSegments(roads, nodes);
    // Optional mode: report the ambush band state at a player position, the way
    // App arms it (one load-change with mafia ineligible -> POLICE roll only).
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::string(argv[i]) != "--player") continue;
        double px = 0, pz = 0;
        if (sscanf(argv[i + 1], "%lf,%lf", &px, &pz) != 2) return 1;
        GameData fixture = original;
        ScriptRuntime rt(fixture);
        rt.setNodePositions(nodes);
        rt.setRoadSegments(actualRoads);
        rt.onLoadStateChanged(false, false);            // App's adapter
        size_t band = 0, armed = 0, approachable = 0;
        for (size_t k = 0; k < rt.ambushes().size(); ++k) {
            const auto& s = rt.ambushes()[k];
            if (!s.havePos) continue;
            const double d = std::hypot(s.x - px, s.z - pz);
            if (d < 250.0 || d > 600.0) continue;
            ++band;
            ScriptWorldState st;
            st.haveHeight = true;
            st.px = px; st.pz = pz; st.py = s.y;
            const bool ok = rt.ambushRoadApproach(s, st);
            if (s.armed) ++armed;
            if (ok) ++approachable;
            printf("  band %s %s dir=%s d=%.1f armed=%d approach=%d\n",
                   s.def->id.c_str(), s.def->klass.c_str(), s.def->dir.c_str(), d,
                   (int)s.armed, (int)ok);
        }
        printf("player (%.1f, %.1f): %zu in band, %zu armed, %zu armable\n",
               px, pz, band, armed, approachable);
        return 0;
    }
    {
        GameData probe;
        probe.ambushes.push_back(GameData::Ambush{});
        ScriptRuntime rt(probe);
        rt.setNodePositions(nodes);
        rt.setRoadSegments(actualRoads);
        std::vector<size_t> degree(rt.routeVertices_.size());
        for (const auto& v : rt.routeVertices_) degree[&v - &rt.routeVertices_[0]] = v.links.size();
        std::map<size_t, size_t> hist;
        for (size_t d : degree) ++hist[d];
        printf("graph: verts=%zu edges=%zu degree histogram:", rt.routeVertices_.size(),
               rt.routeEdges_.size());
        for (const auto& [d, c] : hist) printf(" deg%zu=%zu", d, c);
        printf("\n");
        std::vector<int> comp(rt.routeVertices_.size(), -1);
        size_t components = 0;
        for (size_t i = 0; i < comp.size(); ++i) {
            if (comp[i] != -1) continue;
            std::vector<size_t> stack{i};
            comp[i] = (int)components;
            while (!stack.empty()) {
                size_t v = stack.back();
                stack.pop_back();
                for (const auto& l : rt.routeVertices_[v].links)
                    if (comp[l.first] == -1) { comp[l.first] = (int)components; stack.push_back(l.first); }
            }
            ++components;
        }
        std::map<size_t, size_t> sizes;
        for (int c : comp) ++sizes[(size_t)c];
        std::vector<std::pair<size_t, size_t>> ranked(sizes.begin(), sizes.end());
        std::sort(ranked.begin(), ranked.end(),
                  [](auto& x, auto& y) { return x.second > y.second; });
        printf("graph: components=%zu largest:", components);
        for (size_t i = 0; i < std::min<size_t>(6, ranked.size()); ++i)
            printf(" %zu", ranked[i].second);
        printf("\n");
        size_t deadEnds = 0;
        for (size_t i = 0; i < rt.routeVertices_.size(); ++i) {
            if (rt.routeVertices_[i].links.size() != 1) continue;
            ++deadEnds;
            if (deadEnds <= 12)
                printf("  dead-end #%zu (%.1f %.1f %.1f)\n", i, rt.routeVertices_[i].x,
                       rt.routeVertices_[i].z, rt.routeVertices_[i].y);
        }
        printf("graph: dead ends=%zu\n", deadEnds);
    }
    for (const auto& a : original.ambushes) {
        if (!want.empty() && !want.count(a.id)) continue;
        GameData fixture;
        fixture.ambushes = {a};
        ScriptRuntime rt(fixture);
        rt.setNodePositions(nodes);
        rt.setRoadSegments(actualRoads);
        for (int i = 0; i < 200 && !rt.ambushes()[0].armed; ++i)
            rt.onLoadStateChanged(true, true);
        const auto* anchor = rt.nodePos(a.wayNode);
        printf("=== %s klass=%s dir=%s anchor=%.3f %.3f %.3f armed=%d edges=%zu verts=%zu\n",
               a.id.c_str(), a.klass.c_str(), a.dir.c_str(),
               anchor->x, anchor->z, anchor->y, (int)rt.ambushes()[0].armed,
               rt.routeEdges_.size(), rt.routeVertices_.size());
        auto anchorIt = rt.routeAnchors_.find(a.wayNode);
        if (anchorIt != rt.routeAnchors_.end()) {
            const auto& e = rt.routeEdges_[anchorIt->second.first];
            const auto& va = rt.routeVertices_[e.a];
            const auto& vb = rt.routeVertices_[e.b];
            printf("  anchor edge #%zu a=%.3f %.3f %.3f b=%.3f %.3f %.3f t=%.3f width=%.2f/%.2f\n",
                   anchorIt->second.first, va.x, va.z, va.y, vb.x, vb.z, vb.y,
                   anchorIt->second.second, e.widthA, e.widthB);
            for (const auto [name, idx] : {std::pair<const char*, size_t>{"a", e.a}, {"b", e.b}}) {
                printf("    endpoint %s #%zu links=%zu:", name, idx, rt.routeVertices_[idx].links.size());
                for (const auto& l : rt.routeVertices_[idx].links)
                    printf(" (%.1f %.1f)", rt.routeVertices_[l.first].x,
                           rt.routeVertices_[l.first].z);
                printf("\n");
            }
        } else {
            printf("  no anchor edge resolved\n");
        }
        size_t target = rt.routeNodes_.count(a.wayNode) ? rt.routeNodes_[a.wayNode] : SIZE_MAX;
        if (target != SIZE_MAX) {
            const auto& tv = rt.routeVertices_[target];
            printf("  target vertex #%zu (%.3f %.3f %.3f) links=%zu\n", target, tv.x, tv.z, tv.y,
                   tv.links.size());
            for (const auto& l : tv.links)
                printf("    -> #%zu dist=%.3f (%.3f %.3f %.3f)\n", l.first, l.second,
                       rt.routeVertices_[l.first].x, rt.routeVertices_[l.first].z,
                       rt.routeVertices_[l.first].y);
        } else {
            printf("  no target vertex\n");
        }
        size_t inBand = 0, approach = 0;
        double firstInBandDist = 0, firstApproachDist = 0;
        size_t edgeMiss = 0, viaA = 0, viaB = 0, unreached = 0, eastSamples = 0;
        double minPx = 1e30, maxPx = -1e30, minPz = 1e30, maxPz = -1e30;
        int anchorEdgeIdx = anchorIt != rt.routeAnchors_.end() ? (int)anchorIt->second.first : -1;
        // Replicates ambushRoadApproach's edge selection + Dijkstra so the
        // debug output can show *why* the direction test fails.
        auto classify = [&](double px, double py, double pz) -> char {
            size_t edge = rt.routeEdges_.size();
            double best = 1e30, fraction = 0;
            for (size_t i = 0; i < rt.routeEdges_.size(); ++i) {
                const auto& e = rt.routeEdges_[i];
                const auto& a = rt.routeVertices_[e.a];
                const auto& b = rt.routeVertices_[e.b];
                const double vx = b.x - a.x, vz = b.z - a.z, l2 = vx * vx + vz * vz;
                if (l2 < 1e-12) continue;
                const double t = std::clamp(((px - a.x) * vx + (pz - a.z) * vz) / l2, 0.0, 1.0);
                const double y = a.y + t * (b.y - a.y);
                if (e.haveHeight && std::fabs(py - y) > 4.0) continue;
                const double dist = std::hypot(px - a.x - t * vx, pz - a.z - t * vz);
                const double half = std::max(2.0, (e.widthA + t * (e.widthB - e.widthA)) * 0.5);
                if (dist > half) continue;
                const double score = dist * dist + (e.haveHeight ? (py - y) * (py - y) : 0);
                if (score < best) { best = score; edge = i; fraction = t; }
            }
            if (edge == rt.routeEdges_.size()) return 'X';
            if (anchorEdgeIdx >= 0 && (int)edge == anchorEdgeIdx) return 'S';
            const auto& e = rt.routeEdges_[edge];
            const auto& a = rt.routeVertices_[e.a];
            const auto& b = rt.routeVertices_[e.b];
            const double length = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z) +
                                            (b.y - a.y) * (b.y - a.y));
            const size_t count = rt.routeVertices_.size();
            std::vector<double> distance(count, 1e30);
            std::vector<size_t> previous(count, count);
            using Pending = std::pair<double, size_t>;
            std::priority_queue<Pending, std::vector<Pending>, std::greater<Pending>> pending;
            distance[e.a] = fraction * length;
            distance[e.b] = (1 - fraction) * length;
            pending.push({distance[e.a], e.a});
            pending.push({distance[e.b], e.b});
            while (!pending.empty()) {
                const auto [cost, id] = pending.top();
                pending.pop();
                if (cost != distance[id]) continue;
                if (id == target) {
                    if (previous[id] == count) return 'S';
                    if (previous[id] == rt.routeEdges_[anchorEdgeIdx].a) return 'a';
                    if (previous[id] == rt.routeEdges_[anchorEdgeIdx].b) return 'b';
                    return 'o';
                }
                for (const auto& link : rt.routeVertices_[id].links) {
                    const double candidate = cost + link.second;
                    if (candidate < distance[link.first]) {
                        distance[link.first] = candidate;
                        previous[link.first] = id;
                        pending.push({candidate, link.first});
                    }
                }
            }
            return 'N';
        };
        for (const auto& road : actualRoads) {
            const size_t n = road.xz.size() / 2;
            for (size_t i = 1; i < n; ++i)
                for (int sample = 0; sample <= 10; ++sample) {
                    const double t = sample / 10.0;
                    ScriptWorldState st;
                    st.haveHeight = true;
                    st.px = road.xz[2 * i - 2] + t * (road.xz[2 * i] - road.xz[2 * i - 2]);
                    st.pz = road.xz[2 * i - 1] + t * (road.xz[2 * i + 1] - road.xz[2 * i - 1]);
                    st.py = road.heights.size() == n
                                ? road.heights[i - 1] + t * (road.heights[i] - road.heights[i - 1])
                                : anchor->y;
                    const double d = rt.distTo(anchor->x, anchor->z, st, anchor->y);
                    if (d < 250 || d > 600) continue;
                    ++inBand;
                    if (!firstInBandDist) firstInBandDist = d;
                    minPx = std::min(minPx, st.px); maxPx = std::max(maxPx, st.px);
                    minPz = std::min(minPz, st.pz); maxPz = std::max(maxPz, st.pz);
                    if (st.px > anchor->x) ++eastSamples;
                    switch (classify(st.px, st.py, st.pz)) {
                        case 'X': ++edgeMiss; break;
                        case 'N': ++unreached; break;
                        case 'a': ++viaA; break;
                        case 'b': ++viaB; break;
                        default: break;
                    }
                    if (rt.ambushRoadApproach(rt.ambushes()[0], st)) {
                        ++approach;
                        if (!firstApproachDist) firstApproachDist = d;
                    }
                }
        }
        printf("  in-band=%zu approach-true=%zu firstDist=%.3f firstApproach=%.3f\n"
               "  sides: noEdge=%zu unreached=%zu viaA=%zu viaB=%zu\n"
               "  x range [%.1f, %.1f] z range [%.1f, %.1f] east-of-anchor=%zu\n",
               inBand, approach, firstInBandDist, firstApproachDist, edgeMiss, unreached, viaA, viaB,
               minPx, maxPx, minPz, maxPz, eastSamples);
    }
    return 0;
}

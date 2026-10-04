#include "Traffic.h"
#include "../util/Log.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <queue>
#include <utility>

namespace ht2 {

// ---------------------------------------------------------------------------
// Tuning. Everything here is a compile-time constant so the Python mirror in
// tools/traffic_sim.py can reproduce the simulation number for number.
// ---------------------------------------------------------------------------
namespace {
constexpr float kMinEdgeLen    = 0.5f;    // shorter consecutive pairs are dropped
constexpr float kDefaultWidth  = 8.0f;    // WDTH fallback (also for 0/NaN)
constexpr float kMinWidth      = 2.5f;
constexpr float kMaxWidth      = 40.0f;
constexpr float kCarLength     = 4.5f;    // fallback body length
constexpr float kAccel         = 3.0f;    // m/s^2
constexpr float kBrake         = 7.0f;    // m/s^2
constexpr float kHeadwayMin    = 3.0f;    // m
constexpr float kHeadwayTime   = 0.7f;    // s
constexpr float kStandGap      = 2.0f;    // m, standing gap the clamp enforces
constexpr float kUturnDot      = -0.85f;  // sharper than this counts as a U-turn
constexpr float kStuckSpeed    = 0.4f;    // m/s below which a car is "halted"
constexpr float kStuckTime     = 3.0f;    // s halted before a recovery attempt
constexpr float kGridlockTime  = 10.0f;   // s queued/yielding before the same (jam breaker)
constexpr float kDestTimeout   = 240.0f;  // s before a destination is dropped
constexpr int   kMaxTransitions = 4;      // junction hops per tick
constexpr int   kRelocateTries = 64;
constexpr int   kRouteCache    = 24;      // cached destination fields
constexpr int   kRouteBudget   = 6;       // Dijkstras per update()
constexpr float kSpawnGap      = 20.0f;   // m, minimum free space when (re)placing a car
constexpr float kSpawnClear    = 12.0f;   // m, world-space clearance when relocating
constexpr float kRelocateSelf  = 60.0f;   // m, a relocated car never appears near its old spot
constexpr float kMinDestDist   = 300.0f;  // m, nearest destination
constexpr float kMaxDestDist   = 1800.0f; // m, farthest destination (ambient trips)
constexpr float kLaneBlend     = 6.0f;    // m, junction heading blend window (per side)
constexpr float kTurnSlow      = 7.5f;    // m/s target speed through a 90 deg turn
constexpr float kTurnSlowDist  = 26.0f;   // m before the vertex the cap applies
constexpr float kBodyRadius    = 1.1f;    // fallback body half width
constexpr float kContactSlack  = 0.05f;   // m, keep a hair of daylight between bodies
constexpr float kShiftMax      = 1.2f;    // m, max lateral pushback a car keeps
constexpr float kShiftDecay    = 2.5f;    // 1/s, pushback relaxation
constexpr float kRestitution   = 0.15f;   // impulse: 0 = plastic, 1 = elastic
constexpr float kDamagePerMs   = 0.028f;  // damage per m/s of closing speed
constexpr float kMaxHitDamage  = 0.30f;   // damage of one impact (0..1)
constexpr float kContactMinRel = 0.4f;    // m/s closing speed that counts as a hit
constexpr float kJunctionContact = 8.0f;  // m around a vertex = junction contact
constexpr float kPairReach     = 16.0f;   // m, pairs closer than this can touch
constexpr float kPlayerLook    = 30.0f;   // m, the player is a leader within this
constexpr float kPlayerSlow    = 0.9f;    // 1/s, approach rate of the player cap
constexpr float kRelocateMin   = 90.0f;   // m, never relocate closer than this
constexpr float kViewCos       = 0.55f;   // player view cone (~57 deg half angle)
constexpr float kViewFar       = 400.0f;  // m, beyond this the player cannot see it
constexpr float kDeadEndDist   = 140.0f;  // m, turn around at a dead end beyond this
constexpr float kVertexZone    = 5.0f;    // m, junction conflict radius (bumper to vertex)
constexpr float kVertexMargin  = 1.0f;    // m, a waiting car keeps its bumper this far out
constexpr float kMaxSepStep    = 0.6f;    // m/frame, cap on the contact separation
constexpr float kNarrowPass    = 0.1f;    // m, slack of the "too narrow to pass" test
constexpr float kPi            = 3.14159265358979f;

// The fastest speed from which the car can still stop inside `gap` metres of
// free road (7 m/s^2 of braking).  Without this a follower only reacts to the
// leader's *current* speed and overshoots by metres when the leader brakes
// hard, which the hard no-overlap clamp then has to undo in one frame - a
// visible backwards pop of the whole queue.
inline float stopCap(float gap) {
    const float d = gap - kStandGap;
    return d <= 0.0f ? 0.0f : std::sqrt(2.0f * kBrake * d);
}

inline float dist2(const float ax, const float az, const float bx, const float bz) {
    const float dx = ax - bx, dz = az - bz;
    return dx * dx + dz * dz;
}
// Signed shortest difference of two angles in degrees (-180, 180].
inline float shortAngle(float d) {
    while (d > 180.0f) d -= 360.0f;
    while (d <= -180.0f) d += 360.0f;
    return d;
}
// Distance from (px,pz) to the segment (ax,az)-(bx,bz), squared.
inline float pointSeg2(float px, float pz, float ax, float az, float bx, float bz) {
    const float dx = bx - ax, dz = bz - az;
    const float l2 = dx * dx + dz * dz;
    float t = l2 > 1e-9f ? ((px - ax) * dx + (pz - az) * dz) / l2 : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float qx = ax + dx * t, qz = az + dz * t;
    return dist2(px, pz, qx, qz);
}
// Parameter of the closest point on that segment (0..1).
inline float l2t(float px, float pz, float ax, float az, float bx, float bz) {
    const float dx = bx - ax, dz = bz - az;
    const float l2 = dx * dx + dz * dz;
    float t = l2 > 1e-9f ? ((px - ax) * dx + (pz - az) * dz) / l2 : 0.0f;
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}
} // namespace

// ---------------------------------------------------------------------------
// deterministic RNG
// ---------------------------------------------------------------------------
uint32_t Traffic::nextU32() {
    rng_ = rng_ * 1664525u + 1013904223u;
    return rng_;
}
float Traffic::nextFloat() { return (float)((nextU32() >> 8) & 0xFFFFFFu) / 16777216.0f; }
int Traffic::nextInt(int n) { return n > 0 ? (int)(nextU32() % (uint32_t)n) : 0; }

// ---------------------------------------------------------------------------
// build
// ---------------------------------------------------------------------------
void Traffic::clear() {
    verts_.clear();
    edges_.clear();
    poly_.clear();
    roadNames_.clear();
    nodeVertex_.clear();
    vertexNames_.clear();
    resolvedVerts_.clear();
    resolvedNames_.clear();
    namesOffered_ = 0;
    resolved_ = 0;
    cars_.clear();
    leaders_.clear();
    order_.clear();
    sweep_.clear();
    prevX_.clear();
    prevZ_.clear();
    prevH_.clear();
    holdS_.clear();
    s0_.clear();
    contacts_.clear();
    routes_.clear();
    routeStamp_ = 0;
    routeBudget_ = 0;
    stats_ = Stats();
}

// A reverse Dijkstra from the destination gives, for every vertex, the edge a
// car standing there should take: this is what makes the straight-line rule of
// the junction logic actually converge on a winding road network. Fields are
// cached (LRU) and computed lazily under a per-update budget so a spawn burst
// never stalls a frame.
const Traffic::RouteField* Traffic::routeField(int dest) {
    if (dest < 0 || dest >= (int)verts_.size()) return nullptr;
    for (RouteField& f : routes_)
        if (f.dest == dest) { f.stamp = ++routeStamp_; return &f; }
    if (routeBudget_ <= 0) return nullptr;
    --routeBudget_;
    RouteField* slot = nullptr;
    if ((int)routes_.size() < kRouteCache) { routes_.emplace_back(); slot = &routes_.back(); }
    else {
        for (RouteField& f : routes_) if (!slot || f.stamp < slot->stamp) slot = &f;
    }
    computeRoute(dest, *slot);
    slot->stamp = ++routeStamp_;
    return slot;
}

void Traffic::computeRoute(int dest, RouteField& f) {
    const int n = (int)verts_.size();
    const float kInf = std::numeric_limits<float>::max();
    f.dest = dest;
    f.dist.assign((size_t)n, kInf);
    f.next.assign((size_t)n, -1);
    using QE = std::pair<float, int>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
    f.dist[dest] = 0.0f;
    pq.push(QE(0.0f, dest));
    while (!pq.empty()) {
        const QE top = pq.top();
        pq.pop();
        const int v = top.second;
        if (top.first > f.dist[v] + 1e-4f) continue;
        for (int ei : verts_[v].edges) {
            const Edge& e = edges_[ei];
            const int o = (e.a == v) ? e.b : e.a;
            const float nd = top.first + e.len;
            if (nd < f.dist[o] - 1e-4f) {          // strict: deterministic ties
                f.dist[o] = nd;
                f.next[o] = ei;                    // from o, take ei
                pq.push(QE(nd, o));
            }
        }
    }
}

void Traffic::build(const std::vector<Way::Seg>& segs, const std::map<std::string, Way::Node>& nodes) {
    clear();

    // Weld endpoints that coincide: round x/y/z to 0.01 and hash. The shipped
    // RSEG end points match exactly, so this only removes duplicates.
    std::map<std::array<long long, 3>, int> weld;
    auto vertexOf = [&](double px, double py, double pz) -> int {
        const std::array<long long, 3> k{(long long)llround(px * 100.0),
                                         (long long)llround(py * 100.0),
                                         (long long)llround(pz * 100.0)};
        auto it = weld.find(k);
        if (it != weld.end()) return it->second;
        const int id = (int)verts_.size();
        Vertex v;
        v.x = (float)px; v.h = (float)py; v.z = (float)pz;
        verts_.push_back(std::move(v));
        weld.emplace(k, id);
        return id;
    };

    // Roads are drawn more than once in the shipped data (the same RSEG appears
    // in several tiles / twice in one .way): 422 vertex pairs carry 2+ identical
    // edges, 1134 edges of the 7586 in total.  They are not two roads - a car on
    // each of the "two" edges sits in the *same* lane, so they interpenetrate at
    // every junction and double-count junction degree.  The edge is welded into
    // one, keeping the widest width and the first name/kind seen.
    std::map<std::pair<int, int>, int> edgeOf;
    size_t segCount = 0, dropped = 0, dupEdges = 0;
    for (const auto& sg : segs) {
        if (sg.pts.size() < 2) continue;
        ++segCount;
        float width = (float)sg.width;
        if (!(width >= kMinWidth)) width = kDefaultWidth;   // 0 / NaN / bogus WDTH
        if (width > kMaxWidth) width = kMaxWidth;
        int nameId = -1;
        if (!sg.name.empty()) {
            auto it = std::find(roadNames_.begin(), roadNames_.end(), sg.name);
            if (it == roadNames_.end()) { nameId = (int)roadNames_.size(); roadNames_.push_back(sg.name); }
            else nameId = (int)(it - roadNames_.begin());
        }
        for (size_t k = 0; k + 1 < sg.pts.size(); ++k) {
            const Way::Pos& p0 = sg.pts[k];
            const Way::Pos& p1 = sg.pts[k + 1];
            const int a = vertexOf(p0.x, p0.y, p0.z);
            const int b = vertexOf(p1.x, p1.y, p1.z);
            if (a == b) { ++dropped; continue; }
            const float dx = verts_[b].x - verts_[a].x;
            const float dz = verts_[b].z - verts_[a].z;
            const float len = std::sqrt(dx * dx + dz * dz);
            if (len < kMinEdgeLen) { ++dropped; continue; }
            const std::pair<int, int> key(a < b ? a : b, a < b ? b : a);
            auto dup = edgeOf.find(key);
            if (dup != edgeOf.end()) {                     // the same road twice
                Edge& prev = edges_[(size_t)dup->second];
                if (width > prev.width) prev.width = width;
                if (prev.nameId < 0) { prev.nameId = nameId; prev.attr = sg.attr; }
                ++dupEdges;
                continue;
            }
            Edge e;
            e.a = a; e.b = b; e.len = len; e.width = width; e.attr = sg.attr; e.nameId = nameId;
            e.first = (int)poly_.size();
            e.count = 2;
            // The edge owns its polyline (not just the chord): today two points,
            // but the sampling code walks the whole range so longer polylines
            // (merged curves) work unchanged.
            poly_.push_back(PolyPt{verts_[a].x, verts_[a].h, verts_[a].z, 0.0f});
            poly_.push_back(PolyPt{verts_[b].x, verts_[b].h, verts_[b].z, len});
            const int id = (int)edges_.size();
            edgeOf.emplace(key, id);
            edges_.push_back(e);
            verts_[a].edges.push_back(id);
            verts_[b].edges.push_back(id);
        }
    }

    // Named waypoints (RNOD): name -> nearest graph vertex. A name counts as
    // resolved when that vertex is within maxWaypointDist_.
    const float maxD = maxWaypointDist_;
    for (const auto& kv : nodes) {
        const Way::Node& n = kv.second;
        ++namesOffered_;
        int best = -1;
        double bd = std::numeric_limits<double>::max();
        for (size_t i = 0; i < verts_.size(); ++i) {
            const double dx = (double)verts_[i].x - n.pos.x;
            const double dz = (double)verts_[i].z - n.pos.z;
            const double d = dx * dx + dz * dz;
            if (d < bd) { bd = d; best = (int)i; }   // strict: lowest index wins ties
        }
        if (best < 0) continue;
        nodeVertex_[kv.first] = best;                 // every name is mapped ...
        if (vertexNames_.find(best) == vertexNames_.end()) vertexNames_[best] = kv.first;
        if (std::sqrt(bd) <= (double)maxD) {          // ... but only close ones resolve
            ++resolved_;
            resolvedVerts_.push_back(best);
            resolvedNames_.push_back(kv.first);
        }
    }

    LOG("traffic: road graph %zu vertices, %zu edges from %zu RSEG (%zu dropped, "
        "%zu duplicate edges welded), %zu road names; waypoints %zu/%zu resolved "
        "(<= %.0f m)",
        verts_.size(), edges_.size(), segCount, dropped, dupEdges, roadNames_.size(),
        resolved_, namesOffered_, (double)maxD);
}

bool Traffic::empty() const { return edges_.empty(); }
size_t Traffic::vertices() const { return verts_.size(); }
size_t Traffic::edges() const { return edges_.size(); }
size_t Traffic::waypoints() const { return resolved_; }
size_t Traffic::waypointNames() const { return namesOffered_; }
int Traffic::waypointVertex(size_t i) const {
    return i < resolvedVerts_.size() ? resolvedVerts_[i] : -1;
}
const std::string& Traffic::waypointName(size_t i) const {
    static const std::string kEmpty;
    return i < resolvedNames_.size() ? resolvedNames_[i] : kEmpty;
}

float Traffic::edgeWidth(int e) const {
    return (e >= 0 && e < (int)edges_.size()) ? edges_[e].width : 0.0f;
}
float Traffic::edgeLength(int e) const {
    return (e >= 0 && e < (int)edges_.size()) ? edges_[e].len : 0.0f;
}
int Traffic::edgeAttr(int e) const {
    return (e >= 0 && e < (int)edges_.size()) ? edges_[e].attr : 0;
}
const std::string& Traffic::edgeName(int e) const {
    static const std::string kEmpty;
    if (e < 0 || e >= (int)edges_.size()) return kEmpty;
    const int id = edges_[e].nameId;
    return (id >= 0 && id < (int)roadNames_.size()) ? roadNames_[id] : kEmpty;
}

void Traffic::setModels(const std::vector<int>& ids) { models_ = ids; }

// ---------------------------------------------------------------------------
// car bodies
// ---------------------------------------------------------------------------
void Traffic::setBody(int model, float length, float radius) {
    if (model < 0) return;
    if ((int)bodies_.size() <= model) bodies_.resize((size_t)model + 1);
    Body b;
    b.radius = radius > 0.2f ? radius : kBodyRadius;
    b.length = length > 1.0f ? length : kCarLength;
    bodies_[(size_t)model] = b;
}
Traffic::Body Traffic::bodyOf(int model) const {
    if (model >= 0 && model < (int)bodies_.size()) return bodies_[(size_t)model];
    Body b;
    b.radius = kBodyRadius;
    b.length = kCarLength;
    return b;
}

// ---------------------------------------------------------------------------
// geometry
// ---------------------------------------------------------------------------
float Traffic::distanceToEdge(int e, float x, float z) const {
    if (e < 0 || e >= (int)edges_.size()) return std::numeric_limits<float>::max();
    const Edge& ed = edges_[e];
    if (ed.count < 2) return std::numeric_limits<float>::max();
    float best = std::numeric_limits<float>::max();
    for (int k = ed.first; k + 1 < ed.first + ed.count; ++k)
        best = std::min(best, pointSeg2(x, z, poly_[k].x, poly_[k].z,
                                        poly_[k + 1].x, poly_[k + 1].z));
    return std::sqrt(best);
}

// Distance to the lane polyline: the edge centreline moved `lane` metres to the
// right of the travel direction (`dir`), i.e. the line the car is supposed to
// follow.
float Traffic::distanceToLane(int e, int dir, float x, float z) const {
    if (e < 0 || e >= (int)edges_.size()) return std::numeric_limits<float>::max();
    const Edge& ed = edges_[e];
    const float lane = laneFor(ed);
    const float sgn = dir > 0 ? 1.0f : -1.0f;
    float best = std::numeric_limits<float>::max();
    for (int k = ed.first; k + 1 < ed.first + ed.count; ++k) {
        float ax = poly_[k].x, az = poly_[k].z;
        float bx = poly_[k + 1].x, bz = poly_[k + 1].z;
        float dx = (bx - ax) * sgn, dz = (bz - az) * sgn;
        const float l = std::sqrt(dx * dx + dz * dz);
        if (l < 1e-6f) continue;
        dx /= l; dz /= l;
        const float rx = -dz * lane, rz = dx * lane;      // right of travel
        ax += rx; az += rz; bx += rx; bz += rz;
        best = std::min(best, pointSeg2(x, z, ax, az, bx, bz));
    }
    return std::sqrt(best);
}

bool Traffic::nearestLane(float x, float z, float& outX, float& outZ, float& outH,
                          float& outYawDeg, int& outEdge, float minEdgeLen) const {
    if (edges_.empty()) return false;
    int best = -1;
    float bestD = std::numeric_limits<float>::max();
    float bx = 0.0f, bz = 0.0f, bh = 0.0f, byaw = 0.0f, blane = 0.0f;
    for (int e = 0; e < (int)edges_.size(); ++e) {
        const Edge& ed = edges_[e];
        if (ed.len < minEdgeLen) continue;
        const float lane = laneFor(ed);
        for (int dir = -1; dir <= 1; dir += 2) {
            const float sgn = dir > 0 ? 1.0f : -1.0f;
            for (int k = ed.first; k + 1 < ed.first + ed.count; ++k) {
                float ax = poly_[k].x, az = poly_[k].z;
                float cx = poly_[k + 1].x, cz = poly_[k + 1].z;
                float dx = (cx - ax) * sgn, dz = (cz - az) * sgn;
                const float l = std::sqrt(dx * dx + dz * dz);
                if (l < 1e-6f) continue;
                dx /= l; dz /= l;
                const float rx = -dz * lane, rz = dx * lane;
                ax += rx; az += rz; cx += rx; cz += rz;
                const float d2 = pointSeg2(x, z, ax, az, cx, cz);
                if (d2 < bestD) {
                    bestD = d2;
                    best = e;
                    const float t0 = l2t(x, z, ax, az, cx, cz);
                    bx = ax + (cx - ax) * t0; bz = az + (cz - az) * t0;
                    bh = poly_[k].h + (poly_[k + 1].h - poly_[k].h) * t0;
                    byaw = std::atan2(dx, dz) * 180.0f / kPi;
                    blane = lane;
                }
            }
        }
    }
    (void)blane;
    if (best < 0) return false;
    outX = bx; outZ = bz; outH = bh; outYawDeg = byaw; outEdge = best;
    return true;
}

float Traffic::laneDistance(const Car& c) const {
    float best = std::numeric_limits<float>::max();
    if (c.edge >= 0) best = std::min(best, distanceToLane(c.edge, c.dir, c.x, c.z));
    if (c.prevEdge >= 0) best = std::min(best, distanceToLane(c.prevEdge, c.prevDir, c.x, c.z));
    if (c.nextEdge >= 0) best = std::min(best, distanceToLane(c.nextEdge, c.nextDir, c.x, c.z));
    return best;
}

float Traffic::destinationDistance(const Car& c) const {
    if (c.destVertex < 0 || c.destVertex >= (int)verts_.size()) return -1.0f;
    return std::sqrt(dist2(c.x, c.z, verts_[c.destVertex].x, verts_[c.destVertex].z));
}

void Traffic::sample(int edge, int dir, float s, float& x, float& h, float& z,
                     float& fx, float& fz) const {
    const Edge& e = edges_[edge];
    const PolyPt* p = &poly_[e.first];
    const int n = e.count;
    x = p[0].x; h = p[0].h; z = p[0].z; fx = 0.0f; fz = 1.0f;
    if (n < 2) return;
    if (s < 0.0f) s = 0.0f;
    if (s > e.len) s = e.len;
    // `d` is the arclength from the edge's first polyline point, so forward and
    // reverse travel share one walk (and one height profile).
    const float d = (dir > 0) ? s : (e.len - s);
    int k = 0;
    while (k < n - 2 && p[k + 1].d < d) ++k;
    const float segLen = p[k + 1].d - p[k].d;
    float t = segLen > 1e-6f ? (d - p[k].d) / segLen : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    x = p[k].x + (p[k + 1].x - p[k].x) * t;
    h = p[k].h + (p[k + 1].h - p[k].h) * t;
    z = p[k].z + (p[k + 1].z - p[k].z) * t;
    float dx = p[k + 1].x - p[k].x, dz = p[k + 1].z - p[k].z;
    float l = std::sqrt(dx * dx + dz * dz);
    if (l < 1e-6f) { dx = 0.0f; dz = 1.0f; l = 1.0f; }
    fx = (dx / l) * (float)dir;
    fz = (dz / l) * (float)dir;
}

float Traffic::headingAt(int edge, int dir, float s) const {
    if (edge < 0 || edge >= (int)edges_.size()) return 0.0f;
    float x, h, z, fx, fz;
    sample(edge, dir, s, x, h, z, fx, fz);
    return std::atan2(fx, fz) * 180.0f / kPi;
}

// The lane path through a junction: the pose heading is the current edge's
// tangent, pulled towards the heading of the edge the car came from over the
// first `B` metres and towards the planned outgoing edge over the last `B`
// metres, with B = min(kLaneBlend, len/2) so the two windows never overlap and
// the value at a vertex is exactly the midpoint of the two road headings --
// which is also what the edge on the other side of the vertex computes, so the
// heading (and with it the lane offset) is continuous across junctions.
void Traffic::blendWeights(const Car& c, float& wPrev, float& wNext) const {
    wPrev = 0.0f;
    wNext = 0.0f;
    if (c.edge < 0 || c.edge >= (int)edges_.size()) return;
    const Edge& e = edges_[c.edge];
    const float B = std::min(kLaneBlend, 0.5f * e.len);
    if (B < 0.05f) return;
    const float tOut = c.s / B;                     // 0 at the edge start
    if (tOut < 1.0f && c.prevEdge >= 0 && c.prevDir != 0 &&
        c.prevEdge < (int)edges_.size())
        wPrev = 0.5f * (1.0f - tOut);
    const float tIn = (e.len - c.s) / B;            // 0 at the edge end
    if (tIn < 1.0f && c.nextEdge >= 0 && c.nextDir != 0 &&
        c.nextEdge < (int)edges_.size())
        wNext = 0.5f * (1.0f - tIn);
}

float Traffic::blendedHeading(const Car& c, float raw) const {
    if (c.edge < 0 || c.edge >= (int)edges_.size()) return raw;
    float wPrev = 0.0f, wNext = 0.0f;
    blendWeights(c, wPrev, wNext);
    float h = raw;
    if (wPrev > 0.0f) {
        const float hp = headingAt(c.prevEdge, c.prevDir, edges_[c.prevEdge].len);
        h += wPrev * shortAngle(hp - h);
    }
    if (wNext > 0.0f) {
        const float hn = headingAt(c.nextEdge, c.nextDir, 0.0f);
        h += wNext * shortAngle(hn - h);
    }
    return h;
}

void Traffic::refreshPose(Car& c) {
    if (c.edge < 0 || c.edge >= (int)edges_.size()) return;
    const Edge& e = edges_[c.edge];
    if (c.s < 0.0f) c.s = 0.0f;
    if (c.s > e.len) c.s = e.len;
    float fx = 0.0f, fz = 1.0f;
    float hC = 0.0f;
    sample(c.edge, c.dir, c.s, c.x, hC, c.z, fx, fz);
    const float raw = std::atan2(fx, fz) * 180.0f / kPi;
    float wPrev = 0.0f, wNext = 0.0f;
    blendWeights(c, wPrev, wNext);
    float head = raw;
    float h = hC;
    // The height follows the road profile of the *incoming* and *outgoing* edge
    // as well, with the same weights as the heading: a car crossing a junction
    // whose two roads sit at different heights climbs the difference over the
    // blend window (a ramp) instead of stepping up or dropping through it.
    if (wPrev > 0.0f) {
        const float hp = headingAt(c.prevEdge, c.prevDir, edges_[c.prevEdge].len);
        head += wPrev * shortAngle(hp - head);
        float x, ph, z, pf;
        sample(c.prevEdge, c.prevDir, edges_[c.prevEdge].len, x, ph, z, pf, pf);
        h += wPrev * (ph - hC);
    }
    if (wNext > 0.0f) {
        const float hn = headingAt(c.nextEdge, c.nextDir, 0.0f);
        head += wNext * shortAngle(hn - head);
        float x, nh, z, nf;
        sample(c.nextEdge, c.nextDir, 0.0f, x, nh, z, nf, nf);
        h += wNext * (nh - hC);
    }
    c.yaw = head;
    c.h = h;
    // Right-hand traffic: right = forward x up = (-fz, fx) in the XZ plane.  The
    // offset direction follows the *blended* heading, so the car sweeps a
    // rounded corner instead of stepping sideways at the vertex.
    c.lane = laneFor(e);
    const float hr = head * kPi / 180.0f;
    const float off = c.lane + c.shift;
    c.x += -std::cos(hr) * off;
    c.z += std::sin(hr) * off;
}

// ---------------------------------------------------------------------------
// destinations
// ---------------------------------------------------------------------------
int Traffic::pickResolvedVertex(float fromX, float fromZ, float minDist) {
    if (resolvedVerts_.empty()) {
        if (verts_.empty()) return -1;
        return nextInt((int)verts_.size());
    }
    const int n = (int)resolvedVerts_.size();
    // Ambient trips stay inside the player's bubble when there is one, so cars
    // are not recycled by the streaming radius right after they set off.
    float maxD = kMaxDestDist;
    if (activeRadius_ > 0.0f) maxD = std::min(maxD, std::max(minDist + 100.0f, 0.8f * activeRadius_));
    const float lo = minDist * minDist, hi = maxD * maxD;
    int fallback = -1;
    float fbD = std::numeric_limits<float>::max();
    for (int t = 0; t < 32; ++t) {
        const int v = resolvedVerts_[nextInt(n)];
        if (verts_[v].edges.size() < 2) continue;                 // skip dead ends
        const float d = dist2(verts_[v].x, verts_[v].z, fromX, fromZ);
        if (d >= lo && d <= hi) return v;                         // a local trip
        if (d >= lo && d < fbD) { fbD = d; fallback = v; }
    }
    return fallback >= 0 ? fallback : resolvedVerts_[nextInt(n)];
}

void Traffic::newDestination(Car& c, float x, float z) {
    c.destVertex = pickResolvedVertex(x, z, kMinDestDist);
    c.dest.clear();
    if (c.destVertex >= 0) {
        auto it = vertexNames_.find(c.destVertex);
        if (it != vertexNames_.end()) c.dest = it->second;
    }
    c.destTimer = 0.0f;
}

// ---------------------------------------------------------------------------
// spawning / recycling
// ---------------------------------------------------------------------------
// Occupancy of an entry zone is measured in the *entry frame*: a car on the
// same edge but travelling the other way sits at `len - o.s` from the entry
// vertex, so oncoming traffic blocks an entry exactly like traffic ahead.
bool Traffic::entryBlocked(int edge, int dir, float s, const Car* self) const {
    // A car may only enter an edge when its entry zone holds nobody: without
    // this, queues crossing a junction in the same tick pile up at s = 0 and
    // deadlock (the no-overlap clamp cannot push a car back past the vertex).
    if (edge < 0 || edge >= (int)edges_.size()) return true;
    const float len = edges_[edge].len;
    for (const Car& o : cars_) {
        if (&o == self || o.edge != edge) continue;
        const float os = (o.dir == dir) ? o.s : (len - o.s);
        const float d = os - s;
        if (d > -kCarLength && d < kCarLength + kStandGap) return true;
    }
    return false;
}

// True when no car (other than `self`) is within `minDist` metres of (x,z).
bool Traffic::worldClear(float x, float z, const Car* self, float minDist) const {
    const float d2 = minDist * minDist;
    for (const Car& o : cars_) {
        if (&o == self) continue;
        if (dist2(o.x, o.z, x, z) < d2) return false;
    }
    return true;
}

bool Traffic::occupied(int edge, int dir, float s, float minGap) const {
    if (edge < 0 || edge >= (int)edges_.size()) return true;
    const float len = edges_[edge].len;
    for (const Car& o : cars_) {
        if (o.edge != edge) continue;
        const float os = (o.dir == dir) ? o.s : (len - o.s);
        if (std::fabs(os - s) < minGap) return true;
    }
    return false;
}

// One car at a time inside a junction.  A car *occupies* the vertex while the
// nearest point of its body (its front bumper) is still inside the conflict
// disc, i.e. dist(centre, vertex) < zone + length/2.  A car waiting for the
// junction stops with its bumper kVertexMargin *outside* that disc, so two
// waiting cars never count as occupying it and can never block each other.
bool Traffic::vertexBusy(int v, const Car* self, float zone) const {
    if (v < 0 || v >= (int)verts_.size()) return false;
    for (const Car& o : cars_) {
        if (&o == self) continue;
        const float reach = zone + 0.5f * bodyOf(o.model).length;
        if (dist2(o.x, o.z, verts_[v].x, verts_[v].z) < reach * reach) return true;
    }
    return false;
}

void Traffic::placeOn(Car& c, int edge, int dir, float s) {
    c.edge = edge;
    c.dir = dir >= 0 ? 1 : -1;
    c.s = s;
    c.stuck = 0.0f;
    c.recoveries = 0;
    c.prevEdge = -1;
    c.prevDir = 0;
    c.shift = 0.0f;
    planTurn(c);
    refreshPose(c);
}

// True when (x,z) is inside the player's view: too close to ignore, and inside a
// ~57 degree cone up to kViewFar.  Used so a car is never recycled or relocated
// where the player would see it pop.
bool Traffic::inView(float x, float z) const {
    if (!player_.valid) return false;
    const float dx = x - player_.x, dz = z - player_.z;
    const float d2 = dx * dx + dz * dz;
    if (d2 < kRelocateMin * kRelocateMin) return true;
    if (d2 > kViewFar * kViewFar) return false;
    const float a = player_.yaw * kPi / 180.0f;
    const float fx = std::sin(a), fz = std::cos(a);
    const float d = std::sqrt(d2);
    return (dx * fx + dz * fz) / d > kViewCos;
}

bool Traffic::relocate(Car& c, float px, float pz, float radius) {
    if (edges_.empty()) return false;
    const float r2 = radius > 0.0f ? radius * radius : std::numeric_limits<float>::max();
    int fallbackEdge = -1;
    float fallbackD = -1.0f;
    float fs = 0.0f;
    int fdir = 1;
    for (int t = 0; t < kRelocateTries; ++t) {
        const int ei = nextInt((int)edges_.size());
        const float d = distanceToEdge(ei, px, pz);
        if (d * d > r2) continue;
        const Edge& e = edges_[ei];
        const int dir = (nextU32() & 1u) ? 1 : -1;
        const float s = (0.15f + 0.7f * nextFloat()) * e.len;
        if (occupied(ei, dir, s, kSpawnGap)) continue;   // never drop a car into traffic
        // Prefer a spot the player cannot see; remember the farthest candidate
        // (outside the streaming radius anyway) for the case where the whole
        // neighbourhood is in view.
        float sx, sh, sz, sfx, sfz;
        sample(ei, dir, s, sx, sh, sz, sfx, sfz);
        // A relocation is a *streaming* move: it must not reappear within sight
        // of where it vanished either, or the player sees the car teleport.
        if (dist2(sx, sz, c.x, c.z) < kRelocateSelf * kRelocateSelf) continue;
        // `occupied` only knows the cars on *this* edge: a car dropped 2 m from
        // one on a neighbouring edge is a permanent overlap, so the world-space
        // clearance is checked as well.
        if (!worldClear(sx, sz, &c, kSpawnClear)) continue;
        const float d2p = dist2(sx, sz, player_.valid ? player_.x : px,
                                player_.valid ? player_.z : pz);
        if (inView(sx, sz)) {
            if (d2p > fallbackD) { fallbackD = d2p; fallbackEdge = ei; fs = s; fdir = dir; }
            continue;
        }
        placeOn(c, ei, dir, s);
        newDestination(c, c.x, c.z);
        return true;
    }
    if (fallbackEdge >= 0) {                 // everything nearby is in view: farthest
        placeOn(c, fallbackEdge, fdir, fs);
        newDestination(c, c.x, c.z);
        return true;
    }
    // Fallback: the nearest edge, so a car is never left without a road.
    int best = -1;
    float bd = std::numeric_limits<float>::max();
    for (int i = 0; i < (int)edges_.size(); ++i) {
        const float d = distanceToEdge(i, px, pz);
        if (d < bd) { bd = d; best = i; }
    }
    if (best < 0) return false;
    placeOn(c, best, 1, 0.5f * edges_[best].len);
    newDestination(c, c.x, c.z);
    return true;
}

void Traffic::spawn(float px, float pz, float radius, int count) {
    cars_.clear();
    contacts_.clear();
    playerHit_ = PlayerHit();
    if (radius > 0.0f) activeRadius_ = radius;
    if (edges_.empty() || count <= 0) { stats_.cars = 0; return; }
    const float r2 = radius > 0.0f ? radius * radius : std::numeric_limits<float>::max();
    std::vector<int> near;
    near.reserve(edges_.size());
    for (int i = 0; i < (int)edges_.size(); ++i) {
        const float d = distanceToEdge(i, px, pz);
        if (d * d <= r2) near.push_back(i);
    }
    if (near.empty())
        for (int i = 0; i < (int)edges_.size(); ++i) near.push_back(i);

    cars_.reserve((size_t)count);
    for (int k = 0; k < count; ++k) {
        Car c;
        c.model = models_.empty() ? 0 : models_[(size_t)k % models_.size()];
        c.cruise = 11.0f + 6.0f * nextFloat();       // 11 .. 17 m/s
        c.speed = c.cruise * (0.6f + 0.4f * nextFloat());
        int placed = -1;
        for (int attempt = 0; attempt < 8 && placed < 0; ++attempt) {
            const int ei = near[nextInt((int)near.size())];
            const int dir = ((k + attempt) & 1) ? 1 : -1;
            const float s = (0.1f + 0.8f * nextFloat()) * edges_[ei].len;
            if (occupied(ei, dir, s, kSpawnGap)) continue;   // keep spawn gaps honest
            placeOn(c, ei, dir, s);
            placed = ei;
        }
        if (placed < 0) continue;
        newDestination(c, c.x, c.z);
        cars_.push_back(std::move(c));
    }
    prevX_.assign(cars_.size(), 0.0f);
    prevZ_.assign(cars_.size(), 0.0f);
    prevH_.assign(cars_.size(), 0.0f);
    for (size_t i = 0; i < cars_.size(); ++i) {
        prevX_[i] = cars_[i].x;
        prevZ_[i] = cars_[i].z;
        prevH_[i] = cars_[i].h;
    }
    stats_.cars = cars_.size();
    LOG("traffic: %zu cars spawned on %zu candidate edges near (%.0f, %.0f) r=%.0f",
        cars_.size(), near.size(), (double)px, (double)pz, (double)radius);
}

// ---------------------------------------------------------------------------
// routing
// ---------------------------------------------------------------------------
void Traffic::buildOrder() {
    order_.resize(cars_.size());
    for (size_t i = 0; i < cars_.size(); ++i) order_[i] = (int)i;
    std::sort(order_.begin(), order_.end(), [this](int a, int b) {
        const Car& ca = cars_[a];
        const Car& cb = cars_[b];
        if (ca.edge != cb.edge) return ca.edge < cb.edge;
        if (ca.dir != cb.dir) return ca.dir < cb.dir;
        if (ca.s != cb.s) return ca.s < cb.s;
        return a < b;
    });
    leaders_.assign(cars_.size(), -1);
    for (size_t k = 0; k + 1 < order_.size(); ++k) {
        const Car& a = cars_[order_[k]];
        const Car& b = cars_[order_[k + 1]];
        if (a.edge == b.edge && a.dir == b.dir) leaders_[order_[k]] = order_[k + 1];
    }
}

// ---------------------------------------------------------------------------
// junction resolution
// ---------------------------------------------------------------------------
// Which outgoing edge does a car arriving on c.edge at vertex `v` take?
//   (0) the next hop of the destination's shortest-path field, when it is ready
//       and survives the no-U-turn test,
//   (a) an edge that shortens the straight line to the destination,
//   (b) otherwise the straightest forward continuation.
// `avoid` is skipped (used to re-plan when the first pick is occupied).
// Returns -1 when nothing legal exists, i.e. a genuine dead end.
int Traffic::chooseNext(const Car& c, int v, int& outDir, float& outTurn, int avoid) {
    outDir = 1;
    outTurn = 0.0f;
    if (c.edge < 0 || c.edge >= (int)edges_.size()) return -1;
    const Edge& e = edges_[c.edge];
    const int prev = (c.dir > 0) ? e.a : e.b;           // vertex we came from
    const float inHead = headingAt(c.edge, c.dir, e.len);

    const float curDist = (c.destVertex >= 0)
        ? std::sqrt(dist2(verts_[v].x, verts_[v].z,
                          verts_[c.destVertex].x, verts_[c.destVertex].z))
        : std::numeric_limits<float>::max();
    int routed = -1;                                   // shortest-path next hop
    if (pathfinding_ && c.destVertex >= 0) {
        const RouteField* rf = routeField(c.destVertex);
        if (rf && v < (int)rf->next.size()) routed = rf->next[v];
    }

    int best = -1, bestDir = 1, fwd = -1, fwdDir = 1;
    int onRoute = -1, routeDir = 1;
    float bestD = std::numeric_limits<float>::max();
    float fwdDot = -2.0f;
    float ifx = 0.0f, ifz = 1.0f;
    {
        float ix, ih, iz;
        sample(c.edge, c.dir, e.len, ix, ih, iz, ifx, ifz);
    }
    for (int j : verts_[v].edges) {
        if (j == c.edge) continue;                      // never reverse on the same edge
        if (j == avoid) continue;
        const Edge& ne = edges_[j];
        const int other = (ne.a == v) ? ne.b : ne.a;
        if (other == prev) continue;                    // never go back where we came from
        const int ndir = (ne.a == v) ? 1 : -1;
        float ox, oh, oz, ofx, ofz;
        sample(j, ndir, 0.0f, ox, oh, oz, ofx, ofz);
        const float dot = ifx * ofx + ifz * ofz;
        if (dot < kUturnDot) continue;                  // a hairpin is a U-turn as well
        // The shortest-path field may point back along the road we arrived on
        // (missed turn / destination behind us); it is only honoured here, as a
        // legal candidate -- otherwise it would be a U-turn.
        if (j == routed) { onRoute = j; routeDir = ndir; }
        if (dot > fwdDot) { fwdDot = dot; fwd = j; fwdDir = ndir; }
        if (c.destVertex < 0) continue;
        const float d = std::sqrt(dist2(verts_[other].x, verts_[other].z,
                                        verts_[c.destVertex].x, verts_[c.destVertex].z));
        if (d < curDist - 1.0f && d < bestD) { bestD = d; best = j; bestDir = ndir; }
    }
    int pick, pickDir;
    if (onRoute >= 0) { pick = onRoute; pickDir = routeDir; }
    else if (best >= 0) { pick = best; pickDir = bestDir; }
    else { pick = fwd; pickDir = fwdDir; }
    if (pick < 0) return -1;
    if (pick == c.edge) return -1;                      // defensive: never reverse
    {
        const float outHead = headingAt(pick, pickDir, 0.0f);
        outTurn = std::fabs(shortAngle(outHead - inHead));
        if (outTurn > 179.0f) return -1;                // a hairpin is not a turn
    }
    outDir = pickDir;
    return pick;
}

void Traffic::planTurn(Car& c) {
    c.nextEdge = -1;
    c.nextDir = 0;
    c.turn = 0.0f;
    if (c.edge < 0 || c.edge >= (int)edges_.size()) return;
    const Edge& e = edges_[c.edge];
    const int v = (c.dir > 0) ? e.b : e.a;
    int d = 1;
    float turn = 0.0f;
    const int nx = chooseNext(c, v, d, turn);
    if (nx >= 0) { c.nextEdge = nx; c.nextDir = d; c.turn = turn; }
}

bool Traffic::advanceJunction(Car& c, float px, float pz, float radius) {
    const Edge& e = edges_[c.edge];
    const float over = c.s - e.len;                     // metres past the vertex
    const int v = (c.dir > 0) ? e.b : e.a;              // vertex just reached

    if (c.destVertex == v) {
        ++stats_.arrivals;
        newDestination(c, verts_[v].x, verts_[v].z);
    }

    int pickDir = 1;
    float turn = 0.0f;
    int pick = chooseNext(c, v, pickDir, turn);
    if (pick < 0) {
        // A genuine dead end: no legal continuation exists.  Beyond the player's
        // view the car simply turns around (counted separately, never as a
        // driving U-turn); in view it is recycled out of sight instead.
        const float dpx = (player_.valid ? player_.x : px);
        const float dpz = (player_.valid ? player_.z : pz);
        if (dist2(c.x, c.z, dpx, dpz) > kDeadEndDist * kDeadEndDist) {
            c.prevEdge = c.edge;
            c.prevDir = c.dir;
            c.dir = -c.dir;
            c.s = std::min(over > 0.0f ? over : 0.0f, e.len);
            ++stats_.deadEndTurns;
            planTurn(c);
            refreshPose(c);
            return false;
        }
        ++stats_.recycles;
        relocate(c, px, pz, radius);
        return false;
    }

    // Invariant: the pick must neither be the incoming edge nor a heading
    // reversal - the only way a driving U-turn could be counted here.
    if (pick == c.edge) ++stats_.uturns;
    else {
        const float inHead = headingAt(c.edge, c.dir, e.len);
        const float outHead = headingAt(pick, pickDir, 0.0f);
        if (std::fabs(shortAngle(outHead - inHead)) > 150.0f) ++stats_.uturns;
    }
    const float entry = over < 0.0f ? 0.0f : over;
    if (entryBlocked(pick, pickDir, entry, &c)) {       // wait at the junction line
        // Re-plan towards the best *free* alternative so the lane blend does not
        // keep aiming at an edge the car cannot enter.
        int altDir = 1;
        float altTurn = 0.0f;
        const int alt = chooseNext(c, v, altDir, altTurn, pick);
        if (alt >= 0 && !entryBlocked(alt, altDir, entry, &c)) {
            pick = alt; pickDir = altDir; turn = altTurn;
        } else {
            c.s = e.len;
            c.speed = 0.0f;
            c.yielding = true;                          // queueing != being stuck
            if (c.nextEdge == pick || c.nextEdge < 0) planTurn(c);
            return false;
        }
    }
    c.prevEdge = c.edge;
    c.prevDir = c.dir;
    c.edge = pick;
    c.dir = pickDir;
    c.turn = turn;
    c.s = entry;
    planTurn(c);
    return true;
}

// ---------------------------------------------------------------------------
// car bodies and contacts
// ---------------------------------------------------------------------------
// The player as a stopped/slow leader in front of `c`: returns the speed cap
// (1e9 when the player is not in this car's path) and the car's forward vector.
float Traffic::playerLeaderCap(const Car& c, float& fx, float& fz) const {
    if (!player_.valid) return 1e9f;
    const float a = c.yaw * kPi / 180.0f;
    fx = std::sin(a); fz = std::cos(a);
    const float dx = player_.x - c.x, dz = player_.z - c.z;
    const float along = dx * fx + dz * fz;
    if (along <= 0.0f || along > kPlayerLook) return 1e9f;
    const float rx = -fz, rz = fx;
    const float lat = dx * rx + dz * rz;
    const Body cb = bodyOf(c.model);
    if (std::fabs(lat) > cb.radius + player_.radius + 0.6f) return 1e9f;
    const float gap = along - 0.5f * (cb.length + player_.length);
    if (gap > 14.0f) return 1e9f;
    float cap = std::max(0.0f, (gap - kStandGap) * kPlayerSlow);
    cap = std::min(cap, stopCap(gap));                     // brake in time
    const float pv = player_.vx * fx + player_.vz * fz;    // player speed along us
    if (pv > 0.2f) cap = std::max(cap, std::min(pv, c.cruise));
    return cap;
}

void Traffic::resolveContacts() {
    contacts_.clear();
    playerHit_ = PlayerHit();
    const size_t n = cars_.size();
    if (n == 0) return;
    // A car may be in several contacts at once (a jam): the total pushback of
    // one frame is capped, so a deep overlap is shoved apart over a few frames
    // instead of teleporting.
    s0_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        s0_[i] = cars_[i].s;
        cars_[i].contact = false;      // set again below for every touching car
    }

    // Sweep on x so only pairs that can actually touch are tested.  `sweep_` is
    // module scratch (no per-frame allocation once it has grown).
    sweep_.resize(n);
    for (size_t i = 0; i < n; ++i) sweep_[i] = (int)i;
    std::sort(sweep_.begin(), sweep_.end(), [this](int a, int b) {
        return cars_[(size_t)a].x < cars_[(size_t)b].x;
    });

    for (size_t si = 0; si < n; ++si) {
        const int ia = sweep_[si];
        Car& A = cars_[(size_t)ia];
        const Body ba = bodyOf(A.model);
        for (size_t sj = si + 1; sj < n; ++sj) {
            const int ib = sweep_[sj];
            Car& B = cars_[(size_t)ib];
            if (B.x - A.x > kPairReach) break;             // sorted: nothing closer
            const Body bb = bodyOf(B.model);
            const float reach = 0.5f * (ba.length + bb.length) + 1.0f;
            if (dist2(A.x, A.z, B.x, B.z) > reach * reach) continue;
            float nx = 0.0f, nz = 0.0f;
            // Oncoming traffic on a road narrower than the two bodies is let
            // through (the lanes overlap physically; the original does the same
            // rather than deadlocking on every narrow street).  The same holds
            // for two *different* edges that the shipped data draws on top of
            // each other (shallow merges): no shove can separate them without
            // pushing both cars off the tarmac, so they pass.
            {
                const float sep = (A.edge == B.edge)
                    ? 2.0f * laneFor(edges_[A.edge])
                    : std::min(distanceToEdge(B.edge, A.x, A.z),
                               distanceToEdge(A.edge, B.x, B.z));
                const float afx = std::sin(A.yaw * kPi / 180.0f), afz = std::cos(A.yaw * kPi / 180.0f);
                const float bfx = std::sin(B.yaw * kPi / 180.0f), bfz = std::cos(B.yaw * kPi / 180.0f);
                const float facing = afx * bfx + afz * bfz;
                if (sep + kNarrowPass < ba.radius + bb.radius && facing < -0.7f) continue;
                if (A.edge == B.edge && A.dir != B.dir &&
                    sep + kNarrowPass < ba.radius + bb.radius) continue;
            }
            const float overlap = Collision::bodyOverlap(A.x, A.z, A.yaw, ba, B.x, B.z, B.yaw, bb, nx, nz);
            const float gap = -overlap;                    // surface distance
            if (gap < (float)stats_.minGap) stats_.minGap = gap;
            if (overlap <= 0.0f) continue;

            // --- separate the two bodies along their own path -----------------
            Car* pair[2] = {&A, &B};
            const float sign[2] = {-1.0f, 1.0f};
            for (int k = 0; k < 2; ++k) {
                Car& C = *pair[k];
                if (C.edge < 0 || C.edge >= (int)edges_.size()) continue;
                float cx, ch, cz, cfx, cfz;
                sample(C.edge, C.dir, C.s, cx, ch, cz, cfx, cfz);
                const float rx = -cfz, rz = cfx;
                // The separation moves the body along its own path; when the
                // contact normal is at a shallow angle to the path (two roads
                // that the data draws nearly on top of each other) the move is
                // scaled up so it still separates the two bodies in world
                // space.  Capped per frame, so it is a shove, not a teleport.
                // `sign[k]` is the direction along the contact normal (A moves
                // by -n, B by +n); the projection on the car's own tangent and
                // on its lateral axis gives the exact world displacement.
                const float push = std::min(kMaxSepStep, overlap * 0.5f + kContactSlack) * sign[k];
                C.s += (nx * cfx + nz * cfz) * push;
                if (C.s < 0.0f) C.s = 0.0f;
                C.shift += (nx * rx + nz * rz) * push;
                C.shift = std::max(-kShiftMax, std::min(kShiftMax, C.shift));
            }

            // --- impulse along the contact normal -----------------------------
            const float afx = std::sin(A.yaw * kPi / 180.0f), afz = std::cos(A.yaw * kPi / 180.0f);
            const float bfx = std::sin(B.yaw * kPi / 180.0f), bfz = std::cos(B.yaw * kPi / 180.0f);
            const float na = afx * nx + afz * nz;           // A towards B?
            const float nb = bfx * nx + bfz * nz;           // B towards A?
            const float approach = A.speed * na - B.speed * nb;
            if (approach > 0.0f) {
                const float j = (1.0f + kRestitution) * approach * 0.5f;
                A.speed = std::max(0.0f, A.speed - j * na);
                B.speed = std::max(0.0f, B.speed + j * nb);
            }
            if (approach > kContactMinRel) {
                const float dmg = std::min(kMaxHitDamage, kDamagePerMs * approach);
                A.damage = std::min(1.0f, A.damage + dmg);
                B.damage = std::min(1.0f, B.damage + dmg);
                stats_.damageDealt += 2.0 * dmg;
                ++stats_.contacts;
                ++stats_.carContacts;
                Contact ct;
                ct.a = ia; ct.b = ib;
                ct.x = 0.5f * (A.x + B.x); ct.z = 0.5f * (A.z + B.z);
                ct.rel = approach; ct.depth = overlap; ct.damage = dmg;
                ct.junction = (A.edge != B.edge);          // crossing traffic
                if (ct.junction) ++stats_.junctionHits;
                contacts_.push_back(ct);
            }
            ++stats_.overlapFrames;
            A.contact = true;
            B.contact = true;
            A.stuck = 0.0f;
            B.stuck = 0.0f;
        }
    }

    // --- the player ---------------------------------------------------------
    if (!player_.valid) return;
    Body pb;
    pb.radius = player_.radius;
    pb.length = player_.length;
    for (size_t i = 0; i < n; ++i) {
        Car& A = cars_[i];
        const Body ba = bodyOf(A.model);
        float nx = 0.0f, nz = 0.0f;
        const float overlap = Collision::bodyOverlap(A.x, A.z, A.yaw, ba,
                                          player_.x, player_.z, player_.yaw, pb, nx, nz);
        const float gap = -overlap;
        if (gap < (float)stats_.minGap) stats_.minGap = gap;
        if (overlap <= 0.0f) continue;
        // separate: the car gives way along its own path, the player is pushed
        if (A.edge >= 0 && A.edge < (int)edges_.size()) {
            float cx, ch, cz, cfx, cfz;
            sample(A.edge, A.dir, A.s, cx, ch, cz, cfx, cfz);
            const float rx = -cfz, rz = cfx;
            const float push = std::min(kMaxSepStep, overlap * 0.5f + kContactSlack);
            A.s -= (nx * cfx + nz * cfz) * push;
            if (A.s < 0.0f) A.s = 0.0f;
            A.shift -= (nx * rx + nz * rz) * push;
            A.shift = std::max(-kShiftMax, std::min(kShiftMax, A.shift));
        }
        const float sep = overlap * 0.5f + kContactSlack;
        playerHit_.pushX += nx * sep;
        playerHit_.pushZ += nz * sep;
        playerHit_.hit = true;

        // closed speed: the car's velocity minus the player's, along the normal
        const float afx = std::sin(A.yaw * kPi / 180.0f), afz = std::cos(A.yaw * kPi / 180.0f);
        const float rel = A.speed * (afx * nx + afz * nz) - (player_.vx * nx + player_.vz * nz);
        if (rel > 0.0f) {
            const float j = (1.0f + kRestitution) * rel * 0.5f;
            A.speed = std::max(0.0f, A.speed - j * (afx * nx + afz * nz));
        }
        if (rel > kContactMinRel) {
            const float dmg = std::min(kMaxHitDamage, kDamagePerMs * rel);
            A.damage = std::min(1.0f, A.damage + dmg);
            playerHit_.damage = std::min(1.0f, playerHit_.damage + dmg);
            playerHit_.impact = std::max(playerHit_.impact, rel);
            ++playerHit_.contacts;
            ++stats_.contacts;
            ++stats_.playerContacts;
            stats_.damageDealt += dmg;
            stats_.damageToPlayer += dmg;
            Contact ct;
            ct.a = (int)i; ct.b = -1;
            ct.x = 0.5f * (A.x + player_.x); ct.z = 0.5f * (A.z + player_.z);
            ct.rel = rel; ct.depth = overlap; ct.damage = dmg;
            ct.junction = false;                      // rear-end, not a crossing
            if (ct.junction) ++stats_.junctionHits;
            contacts_.push_back(ct);
        }
        ++stats_.overlapFrames;
        A.contact = true;
        A.stuck = 0.0f;
    }
    for (size_t i = 0; i < n; ++i) {
        Car& c = cars_[i];
        if (c.edge < 0 || c.edge >= (int)edges_.size()) continue;
        if (c.s > s0_[i] + kMaxSepStep) c.s = s0_[i] + kMaxSepStep;
        if (c.s < s0_[i] - kMaxSepStep) c.s = s0_[i] - kMaxSepStep;
        if (c.s < 0.0f) c.s = 0.0f;
    }
}

void Traffic::separateSameLane() {
    buildOrder();
    for (size_t k = order_.size(); k-- > 1; ) {
        Car& back = cars_[order_[k - 1]];
        Car& front = cars_[order_[k]];
        if (back.edge != front.edge || back.dir != front.dir) continue;
        const float lead = 0.5f * (bodyOf(back.model).length + bodyOf(front.model).length);
        const float limit = front.s - lead - kStandGap;
        if (back.s > limit) {
            back.s = std::max(0.0f, limit);
            if (back.speed > front.speed) back.speed = front.speed;
        }
    }
}

// ---------------------------------------------------------------------------
// update
// ---------------------------------------------------------------------------
void Traffic::update(float dt, float px, float pz, float radius) {
    if (edges_.empty() || cars_.empty() || dt <= 0.0f) return;
    if (radius > 0.0f) activeRadius_ = radius;
    if (dt > 0.25f) dt = 0.25f;                          // a hitch must not teleport cars
    stats_.cars = cars_.size();
    stats_.time += (double)dt * cars_.size();
    routeBudget_ = kRouteBudget;                         // warm up a few fields per tick
    if (prevX_.size() != cars_.size()) {
        prevX_.assign(cars_.size(), 0.0f);
        prevZ_.assign(cars_.size(), 0.0f);
        prevH_.assign(cars_.size(), 0.0f);
        for (size_t i = 0; i < cars_.size(); ++i) {
            prevX_[i] = cars_[i].x;
            prevZ_[i] = cars_[i].z;
            prevH_[i] = cars_[i].h;
        }
    }
    holdS_.assign(cars_.size(), -1.0f);

    // Leader of every car on the same (edge, dir) - built from the previous pose.
    buildOrder();

    // --- acceleration + integration ----------------------------------------
    for (size_t i = 0; i < cars_.size(); ++i) {
        Car& c = cars_[i];
        if (c.edge < 0 || c.edge >= (int)edges_.size()) { relocate(c, px, pz, radius); continue; }
        c.yielding = false;
        float target = c.cruise * (1.0f - 0.35f * c.damage);
        const int li = leaders_[i];
        if (li >= 0 && li < (int)cars_.size()) {
            const Car& l = cars_[li];
            const float lead = 0.5f * (bodyOf(c.model).length + bodyOf(l.model).length);
            const float gap = l.s - c.s - lead;          // bumper to bumper
            const float want = kHeadwayMin + kHeadwayTime * c.speed;
            if (gap <= 0.0f) target = 0.0f;
            else {
                if (gap < want) target = std::min(target, l.speed * (gap / want));
                target = std::min(target, l.speed + gap);   // never close the gap up
            }
            target = std::min(target, stopCap(gap));         // brake in time
        }
        // The player is a solid leader too: a car stops behind a stopped truck
        // and follows a moving one instead of driving through it.
        {
            float pfx = 0.0f, pfz = 0.0f;
            const float cap = playerLeaderCap(c, pfx, pfz);
            if (cap < 1e8f) target = std::min(target, cap);
        }
        // Slow down for a planned turn (a 90 degree corner is taken at kTurnSlow).
        if (c.nextEdge >= 0 && c.turn > 20.0f && edges_[c.edge].len - c.s < kTurnSlowDist) {
            const float t = std::min(1.0f, c.turn / 90.0f);
            target = std::min(target, c.cruise + (kTurnSlow - c.cruise) * t);
        }
        // Junction reservation: one car at a time inside a vertex zone, so two
        // crossing cars can never interpenetrate.  A car eases to a stop at the
        // zone boundary, so it never has to be snapped backwards.
        bool holdJunction = false;
        float hold = 1e9f;
        if (c.nextEdge >= 0 && c.edge >= 0 && c.edge < (int)edges_.size()) {
            const Edge& e = edges_[c.edge];
            // Where the car has to stop so its *bumper* stays kVertexZone +
            // kVertexMargin away from the vertex (never negative: on an edge
            // shorter than that the car waits at its entry).
            hold = std::max(0.0f, e.len - kVertexZone - kVertexMargin -
                                        0.5f * bodyOf(c.model).length);
            if (c.s > hold - 6.0f && c.s <= e.len) {
                const int v = (c.dir > 0) ? e.b : e.a;
                holdJunction = vertexBusy(v, &c, kVertexZone) ||
                               entryBlocked(c.nextEdge, c.nextDir, 0.0f, &c);
            }
        }
        if (holdJunction)
            target = std::min(target, std::min(std::max(0.0f, (c.s - hold) * 1.2f),
                                               stopCap(c.s - hold)));
        const float dv = target - c.speed;
        const float lim = (dv > 0.0f ? kAccel : kBrake) * dt;
        c.speed += std::max(-lim, std::min(lim, dv));
        if (c.speed < 0.0f) c.speed = 0.0f;

        const float travel = c.speed * dt;
        stats_.distance += travel;
        c.s += travel;
        if (holdJunction && c.s > hold) {
            if (c.s - travel <= hold + 1e-3f) c.s = hold;   // stop at the boundary
            c.speed = 0.0f;
            c.yielding = true;                              // queueing != being stuck
        }
        holdS_[i] = holdJunction ? hold : -1.0f;
        int guard = 0;
        while (c.s > edges_[c.edge].len && guard++ < kMaxTransitions) {
            if (!advanceJunction(c, px, pz, radius)) break;
        }
        if (c.edge >= 0 && c.edge < (int)edges_.size()) {
            const float len = edges_[c.edge].len;
            if (c.s > len) c.s = len;
            if (c.s < 0.0f) c.s = 0.0f;
        }
        c.destTimer += dt;
        // The lateral pushback from contacts relaxes back to the lane - but not
        // while the car is still touching something, otherwise the forced
        // separation and the decay fight and leave a residual overlap.
        if (!c.contact) c.shift -= c.shift * std::min(1.0f, kShiftDecay * dt);
        if (std::fabs(c.shift) < 0.01f) c.shift = 0.0f;
    }

    // --- contacts (car-vs-car, car-vs-player) -------------------------------
    // Before the lane-group clamp: the clamp then has the last word, so a
    // cross-edge shove can never leave two cars of the same lane overlapping.
    resolveContacts();

    // --- hard no-overlap pass (front to back within each edge/dir) ----------
    separateSameLane();

    // --- poses, destinations, streaming, stall recovery --------------------
    const bool haveRadius = radius > 0.0f;
    const float r2 = haveRadius ? radius * radius : 0.0f;
    holdS_.resize(cars_.size(), -1.0f);
    for (size_t i = 0; i < cars_.size(); ++i) {
        Car& c = cars_[i];
        if (c.edge < 0) continue;
        // The contact separation may not shove a car that is waiting for a busy
        // junction into the conflict zone.  A car that already drove into the
        // block keeps its position: pulling it back would be a visible jump (and
        // the junction zone already keeps everybody else out).
        if (holdS_[i] >= 0.0f && c.s > holdS_[i] + kMaxSepStep) {
            c.s = holdS_[i] + kMaxSepStep;
            if (c.speed > 0.0f) c.speed = 0.0f;
        }
        refreshPose(c);

        if (c.destVertex >= 0) {
            const float d = dist2(c.x, c.z, verts_[c.destVertex].x, verts_[c.destVertex].z);
            if (d <= destReach_ * destReach_) {
                ++stats_.arrivals;
                newDestination(c, c.x, c.z);
            }
        }
        if (c.destTimer > kDestTimeout) {                // wandered too long: re-route
            ++stats_.reroutes;
            newDestination(c, c.x, c.z);
        }
        // --- lane adherence / pose continuity measurements ------------------
        ++stats_.frames;
        const Edge& e = edges_[c.edge];
        const float B = std::min(kLaneBlend, 0.5f * e.len);
        const bool inBlend = (c.s < B) || (e.len - c.s < B);
        if (!inBlend) {
            if (laneDistance(c) > offRoadLimit_) ++stats_.offRoadFrames;
        }
        if (haveRadius && dist2(c.x, c.z, px, pz) > r2) { // left the player's bubble
            ++stats_.respawns;
            relocate(c, px, pz, radius);
            prevX_[i] = c.x;
            prevZ_[i] = c.z;
            prevH_[i] = c.h;
            continue;
        }
        {
            const float moved = std::sqrt(dist2(c.x, c.z, prevX_[i], prevZ_[i]));
            const float slack = c.speed * dt + 0.5f;
            if (moved > slack) {
                ++stats_.jumps;
                if (moved > stats_.maxJump) stats_.maxJump = moved;
            }
            // vertical continuity: a car must not fly or sink through the map
            const float dh = std::fabs(c.h - prevH_[i]);
            if (dh > 0.5f) ++stats_.heightJumps;
            if (dh > stats_.maxHeightStep) stats_.maxHeightStep = dh;
            prevH_[i] = c.h;
            prevX_[i] = c.x;
            prevZ_[i] = c.z;
        }
        // A car held by a busy junction or sitting in a queue is not stuck;
        // only a car that cannot move at all (no leader) is, and it will be
        // re-routed / re-spawned. The queue head therefore unsticks the queue.
        bool queued = false;
        const int li2 = leaders_[i];
        if (li2 >= 0 && li2 < (int)cars_.size()) {
            const Car& l = cars_[li2];
            if (l.edge == c.edge && l.dir == c.dir) {
                const float gap = l.s - c.s - kCarLength;
                if (gap < kHeadwayMin + kHeadwayTime * c.speed) queued = true;
            }
        }
        // Queueing is normal, but a queue that never moves is a gridlock: the
        // fuse is only longer there, not absent.
        if (c.speed < kStuckSpeed) c.stuck += dt;
        else c.stuck = 0.0f;
        if (c.stuck > ((c.yielding || queued) ? kGridlockTime : kStuckTime)) {
            c.stuck = 0.0f;
            ++stats_.stalls;
            if (c.recoveries < 2) {                      // first: try another destination
                ++c.recoveries;
                newDestination(c, c.x, c.z);
                planTurn(c);
            } else {                                     // then: re-spawn near the player
                c.recoveries = 0;
                ++stats_.respawns;
                relocate(c, px, pz, radius);
                prevX_[i] = c.x;
                prevZ_[i] = c.z;
                prevH_[i] = c.h;
            }
        }
    }
    // The streaming relocations of this pass may have dropped a car where the
    // lane group had a gap; restore the invariant once more before the frame is
    // drawn.
    separateSameLane();
    stats_.cars = cars_.size();
}

} // namespace ht2

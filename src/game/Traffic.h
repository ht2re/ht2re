#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "Collision.h"
#include "../world/Way.h"

namespace ht2 {

// AI traffic: cars follow the road network (RSEG polylines) of every tile,
// keeping to the right-hand lane, choosing continuations from the named RNOD
// waypoints where possible and heading for a destination node.
//
// The graph is built once from every `.way` of the world:
//   * every RSEG polyline is split into consecutive-point edges (so the graph
//     keeps the real road shape, ~6.2k welded vertices / ~7.6k edges);
//   * endpoints are welded by rounding x/y/z to 0.01 and hashing (they coincide
//     exactly in the shipped data, so this is lossless);
//   * every edge carries the RSEG width / ATTR kind / road name and its own
//     polyline (2 points today, but the storage is a general range);
//   * every named RNOD node is mapped to its nearest graph vertex and used as a
//     destination (`waypoints()` reports how many mapped within
//     `maxWaypointDist()`, 100 m by default).
//
// Cars drive on the right of the centreline (`laneOffset()`, clamped to a third
// of the edge width) and follow the *lane path*: the heading used for both the
// body yaw and the lateral offset is blended across a junction with the heading
// of the edge the car came from (`prevEdge`) and the edge it will take
// (`nextEdge`), so the body sweeps a rounded corner instead of snapping the
// lane offset sideways at the vertex (see docs/traffic_ai.md 3).  Cars keep a
// leader gap on their edge, are held at a junction whose next edge is occupied,
// never U-turn except at a genuine dead end (which is counted separately) and
// are re-routed / re-spawned near the player when they stall or leave the
// radius, never in the player's view.
//
// Car bodies are capsules (radius + length per model, from the model bbox):
// car-vs-car and car-vs-player contacts separate the two bodies, exchange an
// impulse that depends on the closing speed and deal damage to both.
//
// Everything is deterministic: the module owns an LCG (`setSeed`), no rand().
class Traffic {
public:
    struct Car {
        float x = 0, z = 0, h = 0;   // world position (h = height)
        float yaw = 0;               // degrees, same convention as Vehicle
        float wheelSteer = 0;        // front-axle visual steer, degrees
        float speed = 14.0f;         // m/s
        int model = 0;               // index into the model list (setModels)
        std::string dest;            // destination waypoint name ("" = ambient)
        // --- simulation state (read-only for callers) -----------------------
        int edge = -1;               // current edge index
        int dir = 1;                 // +1 = a->b, -1 = b->a
        float s = 0;                 // metres travelled from the entry vertex
        int destVertex = -1;         // graph vertex of `dest`
        float cruise = 14.0f;        // free-flow speed
        float lane = 0;              // signed lane offset on the current edge
        float stuck = 0;             // seconds spent halted (internal)
        bool yielding = false;       // held at a junction whose next edge is busy
        float destTimer = 0;         // seconds since the destination was picked
        int recoveries = 0;          // re-route attempts for this destination
        // --- lane path / turn state -----------------------------------------
        int prevEdge = -1;           // edge the car came from at the last vertex
        int prevDir = 0;             // travel direction on prevEdge
        int nextEdge = -1;           // planned outgoing edge at the far vertex
        int nextDir = 0;             // travel direction on nextEdge
        float turn = 0;              // heading change of that turn (degrees)
        float shift = 0;             // lateral pushback from contacts (m, decays)
        float damage = 0;            // 0..1 body damage from impacts
        bool contact = false;        // in contact this frame (freezes the pushback)
    };
    struct Stats {
        size_t cars = 0;        // cars currently simulated
        size_t uturns = 0;      // driving direction reversals (target: 0)
        size_t deadEndTurns = 0;// turn-arounds at a genuine dead end
        size_t recycles = 0;    // dead end in view -> relocated
        size_t respawns = 0;    // left the radius / stalled -> relocated
        size_t reroutes = 0;    // destination dropped after the trip timeout
        size_t stalls = 0;      // stall / gridlock recovery attempts
        size_t arrivals = 0;    // destinations reached
        double distance = 0;    // metres driven (sum over cars)
        double time = 0;        // car-seconds simulated
        // --- contacts (car-vs-car and car-vs-player) ------------------------
        size_t contacts = 0;        // contact events (a new overlapping pair)
        size_t carContacts = 0;     // of those, car-vs-car
        size_t playerContacts = 0;  // of those, car-vs-player
        size_t junctionHits = 0;    // contacts within kJunctionContact m of a vertex
        size_t overlapFrames = 0;   // frames in which two bodies interpenetrated
        double damageDealt = 0;     // damage dealt to AI cars
        double damageToPlayer = 0;  // damage dealt to the player
        double minGap = 1e9;        // closest surface-to-surface distance seen
        // --- lane adherence --------------------------------------------------
        size_t offRoadFrames = 0;   // frames a car was > offRoadLimit off the lane
        size_t frames = 0;          // car-frames observed
        size_t jumps = 0;           // frames with a pose jump > speed*dt + slack
        double maxJump = 0;         // largest such jump (m)
        size_t heightJumps = 0;     // frames with a height step > 0.5 m
        double maxHeightStep = 0;   // largest such step (m)
        double meanSpeed() const { return time > 0.0 ? distance / time : 0.0; }
    };

    // Road network + named waypoints (RNOD) of all tiles.
    void build(const std::vector<Way::Seg>& segs, const std::map<std::string, Way::Node>& nodes);
    void clear();
    bool empty() const;
    size_t vertices() const;
    size_t edges() const;
    size_t waypoints() const;              // named nodes resolved to a vertex
    size_t waypointNames() const;          // named nodes seen in the .way files
    // The i-th resolved waypoint (name order), for debug overlays / tests.
    int waypointVertex(size_t i) const;
    const std::string& waypointName(size_t i) const;

    // Model list: `Car::model` is an index into it (0..size-1) and the caller
    // maps that index to its own render batches / body sizes (the app uploads an
    // intact and a dented batch per model).
    void setModels(const std::vector<int>& ids);
    void setFreightModel(int model, bool freight) { freightModels_[model] = freight; }
    // Spawn `count` cars on the road network within `radius` of (px,pz).
    void spawn(float px, float pz, float radius, int count);
    // Advance and keep the population around the player.
    void update(float dt, float px, float pz, float radius);
    const std::vector<Car>& cars() const { return cars_; }
    // Lane offset from the centreline (metres, right-hand traffic).
    float laneOffset() const { return laneOffset_; }
    void setLaneOffset(float v) { laneOffset_ = v; }

    // --- extensions ---------------------------------------------------------
    // A named node counts as resolved when its nearest vertex is this close.
    float maxWaypointDist() const { return maxWaypointDist_; }
    void setMaxWaypointDist(float m) { maxWaypointDist_ = m > 0.0f ? m : 100.0f; }
    void setDestinationRadius(float m) { destReach_ = m; }
    float destinationRadius() const { return destReach_; }
    void setSeed(uint32_t s) { rng_ = s ? s : 1u; }
    // Shortest-path next hops (default) or the literal straight-line rule only.
    // The straight-line rule stays the fallback while a route field is missing.
    void setPathfinding(bool on) { pathfinding_ = on; }
    bool pathfinding() const { return pathfinding_; }
    const Stats& stats() const { return stats_; }
    void resetStats() { stats_ = Stats(); }
    // Road metadata of an edge (for debug overlays / HUD).
    float edgeWidth(int e) const;
    float edgeLength(int e) const;
    int edgeAttr(int e) const;
    const std::string& edgeName(int e) const;
    // Distance from a point to the closest point of an edge's polyline.
    float distanceToEdge(int e, float x, float z) const;
    // Nearest point on a right-hand lane line to (x,z): the pose a car would
    // have there, with the graph heading.  Used to park the player in a lane
    // for --traffic-report and by the tests.  False when the graph is empty.
    // `minEdgeLen` skips short edges (a long one gives a queue somewhere to go).
    bool nearestLane(float x, float z, float& outX, float& outZ, float& outH,
                     float& outYawDeg, int& outEdge, float minEdgeLen = 0.0f) const;
    // Distance from a point to the lane polyline of `edge` when travelled in
    // `dir` (centreline offset by the lane, right-hand traffic).
    float distanceToLane(int e, int dir, float x, float z) const;
    // Distance from a car to its own lane polyline, including the incoming and
    // outgoing edges while it is inside the junction blend window.
    float laneDistance(const Car& c) const;
    // Beyond this the car counts as off its lane (default 2.0 m).
    float offRoadLimit() const { return offRoadLimit_; }
    void setOffRoadLimit(float m) { offRoadLimit_ = m > 0.0f ? m : 2.0f; }
    // Straight-line distance from a car to its destination (<0 when unset).
    float destinationDistance(const Car& c) const;

    // --- car bodies / contacts ---------------------------------------------
    // Body of a model id (radius = half width, length = bumper to bumper), the
    // capsule geometry itself lives in src/game/Collision.h.  The defaults are
    // used for a model that was never described.
    using Body = Collision::Body;
    void setBody(int model, float length, float radius, float height = 1.8f);
    Body bodyOf(int model) const;
    float heightOf(int model) const;
    // The player's vehicle, refreshed by the caller before update().  While it is
    // not `valid` no car interacts with the player (headless harnesses).
    struct Player {
        bool valid = false;
        float x = 0, z = 0;                 // position
        float yaw = 0;                      // degrees (Vehicle convention)
        float vx = 0, vz = 0;               // velocity (m/s)
        float radius = 1.45f, length = 9.5f;
        float h = 0, height = 4.0f;         // road contact plane and bbox height
    };
    void setPlayer(const Player& p) { player_ = p; }
    const Player& player() const { return player_; }
    // Feedback of the last update(): the pushback the player has to apply, the
    // damage it took and the strongest closing speed (App plays the crash).
    struct PlayerHit {
        float pushX = 0, pushZ = 0;   // metres to nudge the player
        float damage = 0;             // 0..1 added to the body damage
        float impact = 0;             // closing speed of the worst contact (m/s)
        int contacts = 0;             // contact events with a car
        bool hit = false;
    };
    const PlayerHit& playerHit() const { return playerHit_; }
    // Contacts resolved by the last update() (a new overlapping pair).
    struct Contact {
        int a = -1, b = -1;      // car indices (b = -1 -> the player)
        float x = 0, z = 0;      // contact point
        float rel = 0;           // closing speed along the normal (m/s)
        float depth = 0;         // overlap (m)
        float damage = 0;        // damage this contact dealt
        bool junction = false;   // within kJunctionContact m of a graph vertex
    };
    const std::vector<Contact>& contacts() const { return contacts_; }
    float carDamage(int i) const {
        return (i >= 0 && i < (int)cars_.size()) ? cars_[(size_t)i].damage : 0.0f;
    }

private:
    std::map<int, bool> freightModels_;
    bool canUseEdge(int model, int edge) const;
    struct PolyPt { float x = 0, h = 0, z = 0, d = 0; };   // d = arclength from edge start
    struct Vertex { float x = 0, h = 0, z = 0; std::vector<int> edges; };
    // Reverse-Dijkstra field of one destination vertex: `next[v]` is the edge a
    // car standing on `v` should take (the requirement's straight-line rule is
    // the fallback whenever no field is ready yet).
    struct RouteField {
        int dest = -1;
        bool freight = true;
        uint32_t stamp = 0;
        std::vector<float> dist;
        std::vector<int> next;
    };
    struct Edge {
        int a = 0, b = 0;
        float len = 1.0f;
        float width = 8.0f;
        int attr = 0;              // RSEG ATTR (road kind)
        int nameId = -1;           // into roadNames_ (-1 when RTEN was empty)
        int first = 0, count = 0;  // range into poly_
    };

    // LCG (Numerical Recipes constants) - deterministic, no rand().
    uint32_t nextU32();
    float nextFloat();
    int nextInt(int n);
    int pickResolvedVertex(float fromX, float fromZ, float minDist);
    void newDestination(Car& c, float x, float z);
    void placeOn(Car& c, int edge, int dir, float s);
    bool relocate(Car& c, float px, float pz, float radius);
    // True when another car sits within `minGap` of (edge, dir, s).
    bool occupied(int edge, int dir, float s, float minGap) const;
    // True when a car entering `edge` at `s` would land on top of another car.
    bool entryBlocked(int edge, int dir, float s, const Car* self) const;
    // True when no car (other than `self`) is within `minDist` of (x,z).
    bool worldClear(float x, float z, const Car* self, float minDist) const;
    // True while another car is inside the junction exclusion zone of vertex v.
    bool vertexBusy(int v, const Car* self, float zone) const;
    // Cached route field for `dest` (nullptr while the per-update budget is spent).
    const RouteField* routeField(int dest, bool freight);
    void computeRoute(int dest, RouteField& f, bool freight);
    void sample(int edge, int dir, float s, float& x, float& h, float& z,
                float& fx, float& fz) const;
    // Heading (degrees) of an edge when travelled in `dir`, at arclength `s`.
    float headingAt(int edge, int dir, float s) const;
    // yawDt < 0 snaps the body onto the road tangent (placement).
    // yawDt == 0 keeps the heading and only refreshes the lane position.
    // yawDt > 0 slews the heading toward the tangent at the bicycle yaw rate.
    void refreshPose(Car& c, float yawDt = -1.0f);
    // Rounded lane corner: the heading and the height are blended with the
    // incoming / outgoing edge over the junction window (see docs/traffic_ai.md
    // 3/6).  `wPrev`/`wNext` are the two blend weights (0..0.5).
    void blendWeights(const Car& c, float& wPrev, float& wNext) const;
    float blendedHeading(const Car& c, float rawHeading) const;
    // Choose the outgoing edge at vertex `v` for a car arriving on `c.edge`.
    // Returns -1 when there is no legal continuation (dead end / U-turn only).
    // `avoid` is skipped (used to re-plan when the first pick is occupied).
    int chooseNext(const Car& c, int v, int& outDir, float& outTurn, int avoid = -1);
    // True when (x,z) lies in the player's view (close, or inside the cone).
    bool inView(float x, float z) const;
    // Fill c.nextEdge / c.nextDir / c.turn for the far end of the current edge.
    void planTurn(Car& c);
    // Sort order_ by (edge, dir, s) and fill leaders_ (car directly ahead).
    void buildOrder();
    // Hard separation of the cars in one lane group (same edge + direction):
    // back.s <= front.s - body lengths - the standing gap.
    void separateSameLane();
    // Move a car that overshot its edge onto the following one. False when the
    // car was recycled / relocated instead.
    bool advanceJunction(Car& c, float px, float pz, float radius);
    // Car-vs-car and car-vs-player separation, impulse, damage.  Also updates
    // the contact statistics.  `px/pz` is the player position (for stats).
    void resolveContacts();
    // The player as a stopped / slow leader for `c` (returns a speed cap).
    float playerLeaderCap(const Car& c, float& fx, float& fz) const;
    // Centre of the right-hand lane. `width` is the whole carriageway (the way
    // log's w=8 is both directions), so each flow sits a quarter of the width
    // off the centreline and the two headings do not share one XZ.
    float laneFor(const Edge& e) const {
        float lane = e.width * 0.25f;
        if (!(lane >= 1.0f)) lane = 1.0f;
        const float fit = e.width * 0.5f - 1.3f;
        if (fit > 1.0f && lane > fit) lane = fit;
        return lane;
    }

    std::vector<Vertex> verts_;
    std::vector<Edge> edges_;
    std::vector<PolyPt> poly_;
    std::vector<std::string> roadNames_;
    std::map<std::string, int> nodeVertex_;      // waypoint name -> vertex
    std::map<int, std::string> vertexNames_;     // vertex -> first waypoint name
    std::vector<int> resolvedVerts_;             // vertices used as destinations
    std::vector<std::string> resolvedNames_;     // names of resolvedVerts_
    size_t namesOffered_ = 0;
    size_t resolved_ = 0;
    float maxWaypointDist_ = 100.0f;
    std::vector<Car> cars_;
    std::vector<int> models_;
    std::vector<Body> bodies_;                   // per model id (grows on demand)
    std::vector<float> bodyHeights_;
    Player player_;
    PlayerHit playerHit_;
    std::vector<Contact> contacts_;
    std::vector<int> leaders_;                   // scratch: car ahead on the edge
    std::vector<int> order_;                     // scratch: cars sorted by (edge,dir,s)
    std::vector<int> sweep_;                     // scratch: cars sorted by x (contacts)
    std::vector<float> prevX_, prevZ_, prevH_;   // scratch: pose before this update
    std::vector<float> holdS_;                   // scratch: junction hold per car
    std::vector<float> s0_;                      // scratch: s before the contacts
    std::vector<RouteField> routes_;
    uint32_t routeStamp_ = 0;
    int routeBudget_ = 0;
    float activeRadius_ = 0.0f;   // last streaming radius (0 = unbounded)
    bool pathfinding_ = true;
    float laneOffset_ = 2.0f;
    float destReach_ = 30.0f;
    float offRoadLimit_ = 2.0f;
    uint32_t rng_ = 0x1234567u;
    Stats stats_;
};

} // namespace ht2

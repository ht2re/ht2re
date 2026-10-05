#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "GameData.h"

namespace ht2 {

// ---------------------------------------------------------------------------
// ScriptRules - the engine side of the shipped .sch data.
//
// The original scripts are pure data: they carry no `if`/`cond` forms.  The
// conditions live in the *engine* and are keyed by heads and fields, so they
// were recovered from original/king.exe (addresses in docs/script_gameplay.md):
//
//   `people` gate kinds       0x411f14..0x412077   (1/2/3/4 = UNCONDITIONAL /
//                             DAYONLY / GOODWEATHER / DAYGW), evaluated every
//                             frame at 0x42ac83..0x42ae69 with the jump table
//                             at 0x42b050; visibility = condition &&
//                             rand()/1e6 < prob.
//   day window                0x484040  = 8.0 < hour < 21.0
//                             (constants 0x64b698 = 8.0, 0x64be18 = 21.0)
//   good weather              0x484010  = weather < 0.2  (0x64b840)
//   node event kind/direction 0x413d3a (STATEBORDER = 0x10) and
//                             0x413d89..0x413e84 (ALL/N/S/E/W + compass vector)
//   node event trigger        0x40b100 onNodePass(player, node, direction):
//                             dot(storedDir, travelDir) > 0 and
//                             |now - lastShown| > 8.0 (0x64b698)
//   obstacle scheduler        0x434f2d ObstaclesTask, every 600..900 ticks;
//                             activation at 0x42b454..0x42b4a6 needs the new
//                             obstacle >= 500 u (0x64b860) from the active one
//                             and away from the player (0x42b2d0)
//   ambush band               0x42b550: arms between 250 u (0x64b868) and
//                             600 u (0x64b870); 0x42b4c0 disarms beyond 600 u
//   node role keywords        0x40f303/0x40f323/0x40f344/0x40f364:
//                             unload|1, hidden|2, fuel|4, dummy|8
//   restable tuning table     restable.sch ( PR_X ("name" price) … )
// ---------------------------------------------------------------------------

// The engine constants above, as named rule values.
struct ScriptRules {
    static constexpr double kDayStartHour = 8.0;      // 0x64b698
    static constexpr double kDayEndHour = 21.0;       // 0x64be18
    static constexpr float kGoodWeatherMax = 0.2f;    // 0x64b840
    static constexpr double kNodeEventCooldown = 8.0; // 0x64b698 (re-used)
    static constexpr double kAmbushArmNear = 250.0;   // 0x64b868
    static constexpr double kAmbushArmFar = 600.0;    // 0x64b870
    static constexpr double kObstacleMinSeparation = 500.0;  // 0x64b860
    // fcn.0042b2d0 (called from the obstacle scheduler at 0x42b3c7/0x42b448)
    // returns 1 when the 3-D distance from the player to the obstacle is
    //   > qword [0x64b860] = 500.0
    // i.e. the *same* 500 u global as the obstacle-to-obstacle separation
    // (0x42b33a `fcomp qword [0x64b860]`).  The previous 250 u was a guess.
    static constexpr double kObstaclePlayerAway = 500.0;      // 0x64b860 / 0x42b33a
    static constexpr double kObstacleMinDelay = 600.0;       // 0x434f8a/0x434f92
    static constexpr double kObstacleMaxDelay = 900.0;
    // not in king.exe: how near the player must be to a .way node for its
    // `nodeevent` to count as "passed" **when no road network has been fed**
    // (see ScriptRuntime::setRoadSegments for the engine's own rule).
    static constexpr double kNodePassRadius = 20.0;

    // ---- the weather-badness curve (king.exe 0x5a9d40) --------------------
    // isGoodWeather() at 0x484010 is `fcn.005a9d40(t) < 0.2` (0x48401a calls
    // it).  0x5a9d40 builds the 0..1 badness from three fields of the
    // weatherandtime object and the frame time:
    //   A = 1 - 2 * w[+0x220]                       (0x64c250 = 1.0f)
    //   B = 1 + 0.3 * sin(w[+0x78])                 (0x64d0f0 = 0.3)
    //   b = 0.5 * (1 + A + B * (1 - |A|) * sin(w[+0x68] - pi/2 + t))
    //   clamp(b, 0, 1)                              (0x64b670 = 1.0,
    //                                                0x64da00 = pi/2,
    //                                                0x64b6e0 = 0.5)
    // so `w[+0x220]` is *clearness*: 1.0 -> b = 0 (clear), 0.0 -> b = 1.
    static constexpr double kBadnessAmpCoeff = 0.3;    // 0x64d0f0
    static constexpr double kBadnessPhase = 1.5707963267948966;  // 0x64da00 = pi/2
    static constexpr double kBadnessHalf = 0.5;        // 0x64b6e0
};

// `hour` is the game clock (weatherandtime, king.exe 0x5a748c): 0..24.
bool scriptIsDay(double hour);
// `weather` is 0 = clear .. 1 = storm; good weather is < 0.2.
bool scriptIsGoodWeather(float weather);
// The engine's badness curve (king.exe 0x5a9d40) for a weatherandtime object
// whose clearness is `clearness` (1 = clear sky, 0 = full storm), blend
// amplitude `amp` (w[+0x78]) and phase `phase` (w[+0x68]) at time `t`
// seconds.  Returns 0..1; `scriptIsGoodWeather` is `badness < 0.2`.
double scriptBadness(double clearness, double amp, double phase, double t);
// Same, but taking the port's 0 = clear .. 1 = storm convention.
double scriptBadnessFromWeather(double weather, double amp, double phase, double t);

// Everything the script conditions read.  Filled by the game each frame.
struct ScriptWorldState {
    double time = 0.0;        // monotonic game seconds (the 8 s guards)
    double hour = 12.0;       // clock hour
    float weather = 0.0f;     // 0 clear .. 1 storm
    long money = 0;           // the player's cash (tuning/upgrade gating)
    float damage = 0.0f;      // 0..1 (service/repair gating)
    double px = 0.0, pz = 0.0;// player world position (x, z)
    bool havePosition = true;
    bool paused = false;      // a menu/time-stop freezes the world
};

struct ScriptNodePos { std::string name; double x = 0.0, z = 0.0; };

// One `.way` RSEG road link, as the node-pass rule needs it: the polyline
// (x, z pairs), the RSEG width and the names of the `.way` nodes at its two
// ends.  `nodeA` is the first VDAT point, `nodeB` the last.
struct ScriptRoadSeg {
    std::string nodeA, nodeB;
    double width = 8.0;                 // RSEG WDTH
    std::vector<double> xz;             // x0,z0,x1,z1,...
};

// Per-instance runtime state (the `active / satisfied / fired` counters of the
// `--script-report`).
struct GateState {
    const GameData::Gate* def = nullptr;
    bool satisfied = false;   // the day/weather condition holds
    bool visible = false;     // satisfied && the activation-edge probability roll
    uint64_t rolls = 0;
};

struct NodeEventState {
    const GameData::NodeEvent* def = nullptr;
    bool active = true;       // false once a one-shot fired
    bool satisfied = false;   // condition (pass + direction + guard) holds
    bool fired = false;       // fired *this* frame
    int fires = 0;
    double lastFire = -1.0e9;
};

struct ObstacleState {
    const GameData::Obstacle* def = nullptr;
    bool closed = false;
    int toggles = 0;
    double x = 0, z = 0;
    bool havePos = false;
};

struct AmbushState {
    const GameData::Ambush* def = nullptr;
    bool active = false;      // inside the 250..600 u band
    bool armed = false;       // the player has been outside the band at least once
    bool satisfied = false;
    int fires = 0;
    double x = 0, z = 0;
    bool havePos = false;
};

// Per-family "how much of the shipped data is actually being executed".
struct ScriptCoverage {
    size_t gates = 0, gatesSatisfied = 0, gatesVisible = 0;
    size_t nodeEvents = 0, nodeEventsActive = 0, nodeEventsSatisfied = 0, nodeEventsFired = 0;
    size_t nodeEventsStateBorder = 0, nodeEventsDirectional = 0, nodeEventsAll = 0;
    size_t obstacles = 0, obstaclesClosed = 0, obstaclesToggled = 0;
    size_t ambushes = 0, ambushesActive = 0, ambushesArmed = 0, ambushesFired = 0;
    size_t services = 0, serviceFuel = 0, serviceRepair = 0, upgradeOffers = 0;
    size_t upgradeRows = 0, upgradeLevels = 0;
    size_t towns = 0, townsPetrol = 0, townsFuel = 0, townGoods = 0, townNodes = 0;
    size_t cargo = 0, drivers = 0, parkings = 0, parkedModels = 0, messages = 0;
    int unambiguousDirections = 0;
};

// One tuning-table row the UI can render: a level of PR_X.
struct UpgradeOffer {
    std::string id;              // PR_TIRES
    int level = 0;
    std::string name;            // "Off-road tires"
    long price = 0;
    bool affordable = false;
    const GameData::Service* service = nullptr;   // the shop that offers it
};

class ScriptRuntime {
public:
    explicit ScriptRuntime(const GameData& gd);

    // The named .way nodes of the loaded world (name -> x, z).  Used for the
    // node-pass trigger, the ambush band and the obstacle scheduler.
    void setNodePositions(const std::vector<ScriptNodePos>& nodes);
    const ScriptNodePos* nodePos(const std::string& name) const;
    // The road network (every RSEG of every loaded tile).  When it is fed, the
    // node-pass trigger becomes the engine's rule (see updateNodeEvents): the
    // player "passes" the .way node at the end of the road link they are
    // driving on, exactly like the original's road PositionId (king.exe
    // 0x43e770 compares the new PositionId with the previous one and 0x43e822
    // calls onNodePass).  Without it the 20 u radius stand-in is used.
    void setRoadSegments(const std::vector<ScriptRoadSeg>& segs);
    bool haveRoadSegments() const { return !roadSegs_.empty(); }
    // The .way node the player's road position is currently on ("" = off-road).
    const std::string& currentRoadNode() const { return lastRoadNode_; }
    // how many passes the road rule produced (diagnostics / --script-report)
    size_t roadPasses() const { return roadPasses_; }
    size_t radiusPasses() const { return radiusPasses_; }

    // Feed an explicit node pass (the App already resolves the nearest .way
    // node).  Cheap; can be called in addition to the position-driven pass.
    void passNode(const std::string& wayNode);
    // Seed the travel vector used by the first onNodePass call.  king.exe gets
    // this vector from the road position transition; a freshly spawned player
    // has no previous free-world sample, so the App seeds it from vehicle yaw.
    void setPreviousPosition(double x, double z) {
        prevX_ = x; prevZ_ = z; havePrev_ = true;
    }

    // One frame.  Evaluates every condition family, advances the timers and
    // fills the coverage counters.
    void update(const ScriptWorldState& st);

    // node events whose message should be shown this frame
    const std::vector<const NodeEventState*>& pendingMessages() const { return pending_; }
    // node events that changed the current state/region (STATEBORDER)
    const std::vector<const NodeEventState*>& pendingBorders() const { return borders_; }
    void clearPending();

    const std::vector<GateState>& gates() const { return gates_; }
    const std::vector<NodeEventState>& nodeEvents() const { return events_; }
    const std::vector<ObstacleState>& obstacles() const { return obstacles_; }
    const std::vector<AmbushState>& ambushes() const { return ambushes_; }

    // ---- options ---------------------------------------------------------
    // king.exe re-arms a node event 8 s after it fired; `setNodeEventOneShot`
    // makes it fire at most once per session instead.
    void setNodeEventOneShot(bool v) { oneShot_ = v; }
    bool nodeEventOneShot() const { return oneShot_; }
    void setNodePassRadius(double r) { passRadius_ = r; }
    void setSeed(uint32_t s) { rng_ = s ? s : 1; }
    // The weatherandtime probe `--script-report` prints the engine's own
    // badness curve for (clearness w[+0x220], amp w[+0x78], phase w[+0x68]).
    void setBadnessProbe(double clearness, double amp, double phase) {
        badnessProbe_[0] = clearness; badnessProbe_[1] = amp; badnessProbe_[2] = phase;
    }

    // ---- the tuning / upgrade table (restable.sch) -----------------------
    const std::vector<GameData::UpgradeLevel>* levels(const std::string& pr) const;
    int maxLevel(const std::string& pr) const;                  // 0 when unknown
    long upgradePrice(const std::string& pr, int level) const;  // -1 when unknown
    std::string upgradeName(const std::string& pr, int level) const;
    bool canBuy(const std::string& pr, int level, long money) const;
    // Every level of every upgrade a `service` form offers, priced and marked
    // affordable for `money` - this is what the tuning UI renders.
    std::vector<UpgradeOffer> tuningOffers(long money) const;

    // ---- reporting -------------------------------------------------------
    ScriptCoverage coverage() const;
    std::string reportText() const;          // `--script-report`
    // extra, per-form-family coverage table (form -> fields)
    std::string coverageTable() const;

    const GameData& data() const { return gd_; }

private:
    void updateGates(const ScriptWorldState&);
    void updateNodeEvents(const ScriptWorldState&);
    void updateObstacles(const ScriptWorldState&);
    void updateAmbushes(const ScriptWorldState&);
    bool passDirectionOk(const GameData::NodeEvent& e, double mx, double mz) const;
    double distTo(double x, double z, const ScriptWorldState&) const;
    uint32_t rnd();
    // the engine's node-pass rule; returns the node the player moved onto (or
    // ""), and fills the travel vector the original derives from the two road
    // positions (0x43e7de: new - previous).
    std::string roadPass(const ScriptWorldState& st, double& mx, double& mz);

    const GameData& gd_;
    std::vector<GateState> gates_;
    std::vector<NodeEventState> events_;
    std::vector<ObstacleState> obstacles_;
    std::vector<AmbushState> ambushes_;
    std::vector<const NodeEventState*> pending_, borders_;
    std::vector<std::string> passQueue_;
    std::map<std::string, size_t> nodeIndex_;
    std::vector<ScriptNodePos> nodes_;
    std::map<std::string, std::vector<size_t>> eventsByNode_;
    double prevX_ = 0, prevZ_ = 0;
    bool havePrev_ = false;
    std::string lastNode_;
    bool oneShot_ = false;
    double passRadius_ = ScriptRules::kNodePassRadius;
    std::vector<ScriptRoadSeg> roadSegs_;
    std::string lastRoadNode_;
    double lastRoadArc_ = 0.0;
    size_t roadPasses_ = 0, radiusPasses_ = 0;
    double badnessProbe_[3] = {0.35, 0.0, 0.0};
    double nextObstacleToggle_ = ScriptRules::kObstacleMinDelay;
    uint32_t rng_ = 0x12345678u;
    ScriptCoverage cov_;
};

// Convenience for the App wiring: one line-block with the current coverage.
std::string scriptReport(const GameData& gd, const ScriptRuntime& rt);

} // namespace ht2

#include "ScriptRules.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace ht2 {

bool scriptIsDay(double hour) {
    // king.exe 0x484040: return hour > 8.0 && hour < 21.0
    return hour > ScriptRules::kDayStartHour && hour < ScriptRules::kDayEndHour;
}

bool scriptIsGoodWeather(float weather) {
    // king.exe 0x484010: weatherandtime->badness(0,0) < 0.2
    return weather < ScriptRules::kGoodWeatherMax;
}

// king.exe 0x5a9d40 -- the weather-badness curve that isGoodWeather() reads.
// See the header for the exact instruction trace and the constant addresses.
double scriptBadness(double clearness, double amp, double phase, double t) {
    const double A = 1.0 - 2.0 * clearness;
    const double B = 1.0 + ScriptRules::kBadnessAmpCoeff * std::sin(amp);
    double b = A + B * (1.0 - std::fabs(A)) *
                       std::sin(phase - ScriptRules::kBadnessPhase + t);
    b = (b + 1.0) * ScriptRules::kBadnessHalf;
    if (b > 1.0) b = 1.0;
    if (b < 0.0) b = 0.0;
    return b;
}

double scriptBadnessFromWeather(double weather, double amp, double phase, double t) {
    // the port's `weather` is 0 = clear .. 1 = storm while the engine's
    // w[+0x220] is clearness (1 = clear), see the header
    return scriptBadness(1.0 - weather, amp, phase, t);
}

static const char* gateKindName(int id) {
    switch (id) {
        case 1: return "UNCONDITIONAL";
        case 2: return "DAYONLY";
        case 3: return "GOODWEATHER";
        case 4: return "DAYGW";
        default: return "?";
    }
}

ScriptRuntime::ScriptRuntime(const GameData& gd) : gd_(gd) {
    gates_.reserve(gd.gates.size());
    for (const auto& g : gd.gates) {
        GateState s;
        s.def = &g;
        gates_.push_back(s);
    }
    events_.reserve(gd.nodeEvents.size());
    for (const auto& e : gd.nodeEvents) {
        NodeEventState s;
        s.def = &e;
        events_.push_back(s);
        eventsByNode_[e.wayNode].push_back(events_.size() - 1);
    }
    obstacles_.reserve(gd.obstacles.size());
    for (const auto& o : gd.obstacles) {
        ObstacleState s;
        s.def = &o;
        obstacles_.push_back(s);
    }
    ambushes_.reserve(gd.ambushes.size());
    for (const auto& a : gd.ambushes) {
        AmbushState s;
        s.def = &a;
        ambushes_.push_back(s);
    }
}

void ScriptRuntime::setNodePositions(const std::vector<ScriptNodePos>& nodes) {
    nodes_ = nodes;
    nodeIndex_.clear();
    for (size_t i = 0; i < nodes_.size(); ++i) nodeIndex_[nodes_[i].name] = i;
    for (auto& o : obstacles_) {
        o.havePos = false;
        const std::string& n = !o.def->debrisA.empty() ? o.def->debrisA
                             : (!o.def->workNodes.empty() ? o.def->workNodes[0] : o.def->debrisB);
        if (const ScriptNodePos* p = nodePos(n)) { o.x = p->x; o.z = p->z; o.y = p->y; o.havePos = true; }
        if(!o.havePos)o.closed=false;
    }
    for (auto& a : ambushes_) {
        a.havePos = false;
        if (const ScriptNodePos* p = nodePos(a.def->wayNode)) {
            a.x = p->x; a.z = p->z; a.y = p->y; a.havePos = true;
        }
        if(!a.havePos) { a.active=false;a.armed=false;a.satisfied=false; }
    }
}

const ScriptNodePos* ScriptRuntime::nodePos(const std::string& name) const {
    auto it = nodeIndex_.find(name);
    return it == nodeIndex_.end() ? nullptr : &nodes_[it->second];
}

uint32_t ScriptRuntime::rnd() {
    // deterministic LCG; the engine uses its 55-state generator (0x68b9c8)
    rng_ = rng_ * 1664525u + 1013904223u;
    return rng_ >> 8;
}

double ScriptRuntime::distTo(double x, double z, const ScriptWorldState& st, double y) const {
    double dx = x - st.px, dz = z - st.pz;
    double dy = st.haveHeight ? y-st.py : 0;
    return std::sqrt(dx * dx + dz * dz + dy * dy);
}

void ScriptRuntime::clearPending() {
    pending_.clear();
    borders_.clear();
}

void ScriptRuntime::passNode(const std::string& wayNode) {
    if (wayNode.empty()) return;
    // Deferred: `update` consumes the list.  A tiny queue keeps the API simple.
    passQueue_.push_back(wayNode);
}

// ---------------------------------------------------------------------------
// `people` gates - king.exe 0x42ac83..0x42ae69
// ---------------------------------------------------------------------------
void ScriptRuntime::updateGates(const ScriptWorldState& st) {
    // The port currently loads all tiles once. Uninitialised groups receive
    // that initial load here; frame ticks never reroll loaded modules.
    for(const auto& g:gates_)if(!g.loaded)onModuleLoad(g.def->tile,st);
}

void ScriptRuntime::onModuleLoad(const std::string& module,const ScriptWorldState& st) {
    if(st.paused)return;
    const bool day = scriptIsDay(st.hour);
    const bool good = scriptIsGoodWeather(st.weather);
    for (auto& g : gates_) {
        if(g.def->tile!=module)continue;
        g.loaded=true;
        bool cond = false;
        switch (g.def->kindId) {
            case 1: cond = true; break;                       // UNCONDITIONAL
            case 2: cond = day; break;                        // DAYONLY
            case 3: cond = good; break;                       // GOODWEATHER
            case 4: cond = day && good; break;                // DAYGW (0x42ade8)
            default: cond = false; break;
        }
        g.satisfied = cond;
        if (!cond) { g.visible = false; continue; }
        // 0x42acb7 first filters the module name; this entire switch belongs
        // to onModuleLoadX, not the frame loop or a condition-edge callback.
        ++g.rolls;
        // 0x42adc2: roll = rand()/1e6; visible when prob > roll
        const double roll = (double)(rnd() % 1000000u) / 1000000.0;
        g.visible = roll < (double)g.def->prob;
    }
}

// ---------------------------------------------------------------------------
// `nodeevent` - king.exe onNodePass @0x40b100
// ---------------------------------------------------------------------------
bool ScriptRuntime::passDirectionOk(const GameData::NodeEvent& e, double mx, double mz) const {
    // `ALL` only ever occurs together with STATEBORDER (which never reaches this
    // test), and the engine's ALL branch (0x413e9c) never writes the compass
    // vector.  Treating ALL as "no filter" is the deliberate, documented
    // divergence (see docs/script_gameplay.md 10.3).
    if (e.dirId == 0) return true;
    const double dot = e.dirX * mx + e.dirZ * mz;
    return dot > 0.0;                               // 0x40b292: fcomp 0.0
}

std::vector<ScriptRoadSeg> scriptRoadSegments(const std::vector<Way::Seg>& segs,
                                            const std::vector<ScriptNodePos>& nodes) {
    std::vector<ScriptRoadSeg> out;
    auto nameAt = [&](const Way::Pos& p) {
        double best=.25; // 0.5m authored coordinate tolerance, not a pass radius
        std::vector<std::string> names;
        for(const auto& n:nodes) {
            if(n.name.empty())continue;
            const double dx=n.x-p.x,dz=n.z-p.z,dy=n.y-p.y;
            const double d=dx*dx+dz*dz+dy*dy;
            if(d<best-1e-8) { best=d;names.clear();names.push_back(n.name); }
            else if(!names.empty()&&std::fabs(d-best)<1e-8)names.push_back(n.name);
        }
        return names;
    };
    for(const auto& s:segs) {
        if(s.pts.size()<2)continue;
        size_t start=0;
        auto a=nameAt(s.pts.front());
        for(size_t end=1;end<s.pts.size();++end) {
            const auto b=nameAt(s.pts[end]);
            if(b.empty()&&end+1<s.pts.size())continue;
            ScriptRoadSeg g;g.aliasesA=a;g.aliasesB=b;
            if(!a.empty())g.nodeA=a.front();
            if(!b.empty())g.nodeB=b.front();
            const double endWidth=s.width1>0?s.width1:s.width;
            g.width=s.width+(endWidth-s.width)*start/(s.pts.size()-1);
            g.widthEnd=s.width+(endWidth-s.width)*end/(s.pts.size()-1);
            for(size_t i=start;i<=end;++i) {
                g.xz.push_back(s.pts[i].x);g.xz.push_back(s.pts[i].z);
                g.heights.push_back(s.pts[i].y);
            }
            out.push_back(std::move(g));start=end;a=b;
        }
    }
    return out;
}

void ScriptRuntime::setRoadSegments(const std::vector<ScriptRoadSeg>& segs) {
    roadSegs_ = segs;
    lastRoadNode_.clear();
    roadAliases_.clear();
    for(const auto& s:segs) {
        if(!s.nodeA.empty())roadAliases_[s.nodeA]=s.aliasesA;
        if(!s.nodeB.empty())roadAliases_[s.nodeB]=s.aliasesB;
    }
}

// The engine's node pass.  king.exe's setPlayerPosition (0x43e610) resolves the
// player's road PositionId, compares it with the previous one (0x43e770), and
// only when it changed computes `newPos - oldPos` (0x43e7de..0x43e7f1 - the
// travel vector) and calls onNodePass (0x43e822).  The road position is a link
// of the road graph, so the port reproduces it directly from the .way RSEG
// polylines: the link the player is within (its own WDTH/2) decides which
// node they are on, and the node changes exactly when they reach the far end
// of that link.  No free-floating radius is involved.
std::string ScriptRuntime::roadPass(const ScriptWorldState& st, double& mx, double& mz) {
    mx = mz = 0.0;
    if (roadSegs_.empty() || !st.havePosition) return std::string();
    const double px = st.px, pz = st.pz;
    size_t bestSeg = roadSegs_.size();
    double bestD = 1e30, bestS = 0.0, bestTotal = 0.0;
    for (size_t s = 0; s < roadSegs_.size(); ++s) {
        const ScriptRoadSeg& g = roadSegs_[s];
        const size_t n = g.xz.size() / 2;
        if (n < 2) continue;
        double arc = 0.0;                        // arclength at the segment start
        for (size_t i = 0; i + 1 < n; ++i) {
            const double ax = g.xz[2 * i], az = g.xz[2 * i + 1];
            const double bx = g.xz[2 * i + 2], bz = g.xz[2 * i + 3];
            const double dx = bx - ax, dz = bz - az;
            const double len = std::sqrt(dx * dx + dz * dz);
            if (len < 1e-6) continue;
            double t = ((px - ax) * dx + (pz - az) * dz) / (len * len);
            t = std::max(0.0, std::min(1.0, t));
            const double cx = ax + t * dx, cz = az + t * dz;
            const double d = std::sqrt((px - cx) * (px - cx) + (pz - cz) * (pz - cz));
            if(st.haveHeight&&g.heights.size()==n) {
                const double h=g.heights[i]+t*(g.heights[i+1]-g.heights[i]);
                // Port projection guard, not a recovered original constant.
                // Prevent crossing an unrelated bridge on the same x/z.
                if(std::fabs(h-st.py)>4.0) { arc+=len;continue; }
            }
            if (d < bestD) {
                bestD = d;
                bestSeg = s;
                bestS = arc + t * len;
            }
            arc += len;
        }
        if (bestSeg == s) bestTotal = arc;
    }
    if (bestSeg >= roadSegs_.size()) { lastRoadNode_.clear();return std::string(); }
    const ScriptRoadSeg& g = roadSegs_[bestSeg];
    // off-road: the engine has no road position at all, so nothing is passed
    const double width=g.width+(g.widthEnd>0?g.widthEnd-g.width:0)*
        (bestTotal>0?bestS/bestTotal:0);
    const double halfWidth = std::max(2.0, 0.5 * width);
    if (bestD > halfWidth) {
        lastRoadNode_.clear();
        return std::string();
    }
    // "at a node" = within one road width of either end of the link
    const double zone = std::max(width, 5.0);
    std::string node;
    const bool atA=bestS<=zone&&!g.nodeA.empty();
    const bool atB=bestTotal-bestS<=zone&&!g.nodeB.empty();
    if(atA&&(!atB||bestS<=bestTotal-bestS))node=g.nodeA;
    else if(atB)node=g.nodeB;
    if (node.empty() || node == lastRoadNode_) {
        lastRoadNode_ = node;
        return std::string();
    }
    // Preserve the complete position difference, not a signed tangent:
    // BASE01LEFT/BASE02RIGHT/BASE07RIGHT explicitly require a transverse
    // component at their authored road position. The port does not yet
    // implement the original PositionId -> position resolver (0x435bf0).
    lastRoadNode_ = node;
    mx = havePrev_?st.px-prevX_:0;
    mz = havePrev_?st.pz-prevZ_:0;
    ++roadPasses_;
    return node;
}

void ScriptRuntime::updateNodeEvents(const ScriptWorldState& st) {
    // 1. the pass set for this frame: the explicit queue plus either the
    //    engine's road-position rule (when the .way network was fed) or the
    //    nearest-node-within-passRadius stand-in.
    const double travelX=st.havePosition&&havePrev_?st.px-prevX_:0;
    const double travelZ=st.havePosition&&havePrev_?st.pz-prevZ_:0;
    std::map<std::string,std::pair<double,double>> passed;
    for(const auto& n:passQueue_)passed[n]={travelX,travelZ};
    passQueue_.clear();
    double mx = 0, mz = 0;
    if (!roadSegs_.empty()) {
        std::string n = roadPass(st, mx, mz);
        if (!n.empty()) {
            passed.emplace(n,std::make_pair(mx,mz));
            // Multiple script markers can name the very same authored road
            // position; do not silently discard all but the first RNOD.
            for(const auto& alias:roadAliases_[n])passed.emplace(alias,std::make_pair(mx,mz));
        }
    } else if (st.havePosition && !nodes_.empty()) {
        const ScriptNodePos* best = nullptr;
        double bd = passRadius_;
        for (const auto& n : nodes_) {
            double d = distTo(n.x, n.z, st, n.y);
            if (d <= bd) { bd = d; best = &n; }
        }
        if (best && best->name != lastNode_) { passed.emplace(best->name,std::make_pair(travelX,travelZ)); ++radiusPasses_; }
        lastNode_ = best ? best->name : std::string();   // left the node again
    }
    // Update previous position once, after every pass captured its direction.
    if(st.havePosition) { prevX_=st.px;prevZ_=st.pz;havePrev_=true; }
    else { havePrev_=false;lastNode_.clear();lastRoadNode_.clear(); }

    for (auto& s : events_) {
        s.fired = false;
        s.satisfied = false;
        if (!s.active) continue;
        const GameData::NodeEvent& e = *s.def;
        const auto hit=passed.find(e.wayNode);
        if(hit==passed.end())continue;
        if (e.kindFlag & 0x10) {
            // STATEBORDER: handled *before* the direction/cooldown test
            // (0x40b1a4..0x40b1b7) - it updates the player's region.
            s.satisfied = true;
            s.fired = true;
            ++s.fires;
            s.lastFire = st.time;
            borders_.push_back(&s);
            continue;
        }
        if (!passDirectionOk(e, hit->second.first, hit->second.second)) continue;
        if (!(st.time - s.lastFire > ScriptRules::kNodeEventCooldown)) continue;  // 0x40b2a9
        s.satisfied = true;
        s.fired = true;
        ++s.fires;
        s.lastFire = st.time;
        pending_.push_back(&s);
        if (oneShot_) s.active = false;
    }
}

// ---------------------------------------------------------------------------
// `obstacle` - king.exe ObstaclesTask @0x434f2d and 0x42b3a0
// ---------------------------------------------------------------------------
void ScriptRuntime::updateObstacles(const ScriptWorldState& st) {
    if (obstacles_.empty() || !st.havePosition) return;
    const auto delay=[&]() {
        return ScriptRules::kObstacleMinDelay+double(rnd()%uint32_t(
            ScriptRules::kObstacleMaxDelay-ScriptRules::kObstacleMinDelay));
    };
    if(nextObstacleToggle_<0)nextObstacleToggle_=delay();
    if (st.time < nextObstacleToggle_) return;
    // pick a candidate: at least 500 u from the closed one (0x64b860) and away
    // from the player (0x42b2d0); the engine tries up to 5 times.
    size_t current = obstacles_.size();
    for (size_t i = 0; i < obstacles_.size(); ++i)
        if (obstacles_[i].closed) { current = i; break; }
    for (int attempt = 0; attempt < 5; ++attempt) {
        const size_t pick = rnd() % obstacles_.size();
        ObstacleState& c = obstacles_[pick];
        if(!c.havePos)continue;
        if (c.havePos) {
            if (distTo(c.x, c.z, st, c.y) <= ScriptRules::kObstaclePlayerAway) continue;
            if (current < obstacles_.size() && obstacles_[current].havePos) {
                double dx = c.x - obstacles_[current].x, dz = c.z - obstacles_[current].z;
                const double dy=c.y-obstacles_[current].y;
                if (std::sqrt(dx * dx + dz * dz + dy * dy) <= ScriptRules::kObstacleMinSeparation) continue;
            }
        }
        for (auto& o : obstacles_) if (&o != &c) { if (o.closed) { ++o.toggles;o.changed=true; } o.closed = false; }
        c.closed = true;
        c.changed=true;
        ++c.toggles;
        break;
    }
    nextObstacleToggle_ = st.time + delay();
}

// ---------------------------------------------------------------------------
// `ambush` - king.exe 0x42b550 (arm), 0x42b4c0 (disarm), updateAmbushes @0x42c390
// ---------------------------------------------------------------------------
void ScriptRuntime::updateAmbushes(const ScriptWorldState& st) {
    for (auto& a : ambushes_) {
        if (!a.havePos || !st.havePosition) continue;
        const double d = distTo(a.x, a.z, st, a.y);
        if (d > ScriptRules::kAmbushArmFar) { a.armed = true; a.active = false; a.satisfied = false; continue; }
        if (d < ScriptRules::kAmbushArmNear) { a.active = false; a.satisfied = false; continue; }
        a.satisfied = a.armed && !a.active;
        if (a.satisfied) {
            ++a.fires;
            a.active = true;
            a.armed = false;        // another pass requires leaving beyond 600 u
        }
    }
}

void ScriptRuntime::update(const ScriptWorldState& st) {
    clearPending();
    for(auto& e:events_) { e.fired=false;e.satisfied=false; }
    for(auto& o:obstacles_)o.changed=false;
    for(auto& a:ambushes_)a.satisfied=false;
    if (st.paused) return;
    updateGates(st);
    updateNodeEvents(st);
    updateObstacles(st);
    updateAmbushes(st);
}

// ---------------------------------------------------------------------------
// the tuning table
// ---------------------------------------------------------------------------
const std::vector<GameData::UpgradeLevel>* ScriptRuntime::levels(const std::string& pr) const {
    auto it = gd_.upgradeLevels.find(pr);
    return it == gd_.upgradeLevels.end() ? nullptr : &it->second;
}

int ScriptRuntime::maxLevel(const std::string& pr) const {
    const auto* l = levels(pr);
    return l ? (int)l->size() - 1 : 0;
}

long ScriptRuntime::upgradePrice(const std::string& pr, int level) const {
    const auto* l = levels(pr);
    if (!l || level < 0 || (size_t)level >= l->size()) return -1;
    return (*l)[(size_t)level].price;
}

std::string ScriptRuntime::upgradeName(const std::string& pr, int level) const {
    const auto* l = levels(pr);
    if (!l || level < 0 || (size_t)level >= l->size()) return std::string();
    return (*l)[(size_t)level].name;
}

bool ScriptRuntime::canBuy(const std::string& pr, int level, long money) const {
    const long p = upgradePrice(pr, level);
    if (p < 0) return false;
    if (level == 0) return false;               // level 0 is the factory part
    return money >= p;                          // NOT_ENOUGH_MONEY
}

std::vector<UpgradeOffer> ScriptRuntime::tuningOffers(long money) const {
    std::vector<UpgradeOffer> out;
    for (const auto& sv : gd_.services) {
        for (const auto& up : sv.upgrades) {
            const int n = up.second;
            for (int lv = 0; lv <= n; ++lv) {
                UpgradeOffer o;
                o.id = up.first;
                o.level = lv;
                o.name = upgradeName(up.first, lv);
                o.price = upgradePrice(up.first, lv);
                o.affordable = o.price == 0 || (o.price > 0 && money >= o.price);
                o.service = &sv;
                out.push_back(o);
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// reporting
// ---------------------------------------------------------------------------
ScriptCoverage ScriptRuntime::coverage() const {
    ScriptCoverage c;
    c.gates = gates_.size();
    for (const auto& g : gates_) { if (g.satisfied) ++c.gatesSatisfied; if (g.visible) ++c.gatesVisible; }
    c.nodeEvents = events_.size();
    for (const auto& e : events_) {
        if (e.active) ++c.nodeEventsActive;
        if (e.satisfied) ++c.nodeEventsSatisfied;
        if (e.fired) ++c.nodeEventsFired;
        if (e.def->kindFlag & 0x10) ++c.nodeEventsStateBorder; else ++c.nodeEventsDirectional;
    }
    c.nodeEventsAll = 0;
    for (const auto& e : events_) if (e.def->dirId == 0) ++c.nodeEventsAll;
    c.obstacles = obstacles_.size();
    for (const auto& o : obstacles_) { if (o.closed) ++c.obstaclesClosed; c.obstaclesToggled += (size_t)o.toggles; }
    c.ambushes = ambushes_.size();
    for (const auto& a : ambushes_) {
        if (a.active) ++c.ambushesActive;
        if (a.armed) ++c.ambushesArmed;
        c.ambushesFired += (size_t)a.fires;
    }
    c.services = gd_.services.size();
    for (const auto& s : gd_.services) {
        if (s.kind == 10 || s.id.rfind("BENZO", 0) == 0) ++c.serviceFuel; else ++c.serviceRepair;
        c.upgradeOffers += s.upgrades.size();
    }
    c.upgradeRows = gd_.upgradeLevels.size();
    for (const auto& kv : gd_.upgradeLevels) c.upgradeLevels += kv.second.size();
    c.towns = gd_.towns.size();
    for (const auto& t : gd_.towns) {
        if (t.petrol) ++c.townsPetrol;
        if (t.fuelDepot) ++c.townsFuel;
        c.townGoods += t.goods.size();
        c.townNodes += t.nodes.size();
    }
    c.cargo = gd_.cargo.size();
    c.drivers = gd_.people.size();
    c.parkings = gd_.parkings.size();
    c.parkedModels = gd_.parked.size();
    c.messages = gd_.messages.size();
    c.unambiguousDirections = (int)c.nodeEventsDirectional;
    return c;
}

static void addReportLine(std::string& out, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    out += "script-report: ";
    out += buf;
    out += "\n";
}

std::string ScriptRuntime::reportText() const {
    ScriptCoverage c = coverage();
    std::string out;
#define line(...) addReportLine(out, __VA_ARGS__)

    // how many of each gate kind the shipped data uses
    std::map<std::string, size_t> kinds;
    for (const auto& g : gd_.gates) kinds[gateKindName(g.kindId)]++;
    std::string kindList;
    for (const auto& kv : kinds) {
        if (!kindList.empty()) kindList += ",";
        kindList += kv.first + "=" + std::to_string(kv.second);
    }
    line("family=gate        total=%zu active=%zu satisfied=%zu visible=%zu kinds=%s",
         c.gates, c.gatesVisible, c.gatesSatisfied, c.gatesVisible, kindList.c_str());
    line("family=nodeevent   total=%zu active=%zu satisfied=%zu fired=%zu stateborder=%zu "
         "directional=%zu all=%zu cooldown=%.1fs",
         c.nodeEvents, c.nodeEventsActive, c.nodeEventsSatisfied, c.nodeEventsFired,
         c.nodeEventsStateBorder, c.nodeEventsDirectional, c.nodeEventsAll,
         ScriptRules::kNodeEventCooldown);
    line("family=obstacle    total=%zu closed=%zu toggles=%zu separation=%.0f playerAway=%.0f "
         "delay=[%.0f,%.0f]",
         c.obstacles, c.obstaclesClosed, c.obstaclesToggled,
         ScriptRules::kObstacleMinSeparation, ScriptRules::kObstaclePlayerAway,
         ScriptRules::kObstacleMinDelay, ScriptRules::kObstacleMaxDelay);
    line("family=ambush      total=%zu active=%zu armed=%zu fired=%zu band=[%.0f,%.0f]",
         c.ambushes, c.ambushesActive, c.ambushesArmed, c.ambushesFired,
         ScriptRules::kAmbushArmNear, ScriptRules::kAmbushArmFar);
    line("family=service     total=%zu fuel=%zu repair=%zu upgradeOffers=%zu",
         c.services, c.serviceFuel, c.serviceRepair, c.upgradeOffers);
    line("family=town        total=%zu petrol=%zu fuelDepot=%zu nodes=%zu goods=%zu",
         c.towns, c.townsPetrol, c.townsFuel, c.townNodes, c.townGoods);
    // services may ask for an upgrade level restable.sch has no row for - that
    // is a real gap in the shipped data and is reported here.
    size_t missing = 0, rows = 0;
    long maxPrice = 0;
    for (const auto& sv : gd_.services)
        for (const auto& up : sv.upgrades) {
            const auto* lv = levels(up.first);
            for (int l = 0; l <= up.second; ++l) {
                ++rows;                          // one UI row per (PR_X, level)
                if (!lv || (size_t)l >= lv->size()) { ++missing; continue; }
                maxPrice = std::max(maxPrice, (*lv)[(size_t)l].price);
            }
        }
    line("family=tuning      upgradeRows=%zu levels=%zu offers=%zu rows=%zu missingLevels=%zu "
         "maxPrice=%ld",
         c.upgradeRows, c.upgradeLevels, c.upgradeOffers, rows, missing, maxPrice);
    line("family=world       cargo=%zu drivers=%zu parkings=%zu parkedModels=%zu messages=%zu",
         c.cargo, c.drivers, c.parkings, c.parkedModels, c.messages);
    line("conditions day=%.1f<hour<%.1f goodWeatherBadness<%.2f oneShot=%d",
         ScriptRules::kDayStartHour, ScriptRules::kDayEndHour, ScriptRules::kGoodWeatherMax,
         oneShot_ ? 1 : 0);
    // the engine's own badness curve (king.exe 0x5a9d40) sampled so the report
    // prints the numbers the engine would use, not a stand-in
    line("badness    clear=%.3f amp=%.3f phase=%.3f t=0 -> %.4f (good=%d)",
         badnessProbe_[0], badnessProbe_[1], badnessProbe_[2],
         scriptBadness(badnessProbe_[0], badnessProbe_[1], badnessProbe_[2], 0.0),
         scriptIsGoodWeather((float)scriptBadness(badnessProbe_[0], badnessProbe_[1],
                                                  badnessProbe_[2], 0.0)) ? 1 : 0);
    line("nodePass   rule=%s passes=%zu radiusPasses=%zu radius=%.1f node=%s",
         roadSegs_.empty() ? "nearest-node-radius" : "road-position-transition",
         roadPasses_, radiusPasses_, passRadius_,
         lastRoadNode_.empty() ? "-" : lastRoadNode_.c_str());
    line("%s", gd_.coverageSummary().c_str());
#undef line
    return out;
}

std::string ScriptRuntime::coverageTable() const {
    return gd_.coverageText();
}

std::string scriptReport(const GameData& gd, const ScriptRuntime& rt) {
    (void)gd;
    return rt.reportText();
}

} // namespace ht2

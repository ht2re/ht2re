#include "Economy.h"
#include "../util/Log.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace ht2 {

namespace {

std::string lowerOf(const std::string& s) {
    std::string o = s;
    for (auto& c : o) c = (char)tolower((unsigned char)c);
    return o;
}

// "ENV/mod_aa.sch" (lowercased) -> true when the basename is `name`.
bool baseIs(const std::string& lowerPath, const char* name) {
    size_t a = lowerPath.find_last_of("/\\");
    std::string base = (a == std::string::npos) ? lowerPath : lowerPath.substr(a + 1);
    return base == name;
}

// The world this engine loads lives in <data>/ENV; MENV and MTR are the menu
// worlds and must not override its counters.
bool inEnvDir(const std::string& lowerPath) {
    return lowerPath.find("/env/") != std::string::npos || lowerPath.rfind("env/", 0) == 0;
}

// Every list form with the given head, up to `depth` levels deep.  The shipped
// files wrap their records in one outer list, but staying depth-limited keeps a
// nested `vehicle` list (semitrailer model lists) from being mistaken for a
// record.
void collectHead(const Sexp& e, const char* head, int depth, std::vector<const Sexp*>& out) {
    if (!e.isList() || depth > 4) return;
    if (e.head() == head && e.size() >= 3) out.push_back(&e);
    for (const auto& k : e.kids) collectHead(k, head, depth + 1, out);
}

bool isIdentAtom(const Sexp& e) {
    return e.isAtom() && !e.text.empty() && (isalpha((unsigned char)e.text[0]) || e.text[0] == '_');
}

// A record form: a list whose head is an identifier (the cargo items of
// merchandise.sch are one, and this also rejects the wrapping outer list whose
// first element is a list).
bool hasIdentHead(const Sexp& e) {
    return e.isList() && !e.kids.empty() && isIdentAtom(e.kids[0]);
}

// merchandise.sch spells one number `1#e+006` (C_WAGES' sell price, i.e. the
// 1,000,000 wage transfer).  The game's own number reader accepts it, strtod
// stops at '#'; use the shared full-token game-data accessor.
double numLoose(const Sexp* e, size_t i) {
    return e ? e->gameNum(i) : 0.0;
}

bool isNumberLoose(const Sexp* e, size_t i) {
    return e && e->isGameNumber(i);
}

} // namespace

// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------

void Economy::setWayNodes(const std::map<std::string, Way::Node>& nodes) {
    nodes_ = nodes;
    if (initialized_) resolvePositions();     // late table: fill in the positions
}

const Way::Node* Economy::wayNode(const std::string& name) const {
    if (name.empty()) return nullptr;
    auto it = nodes_.find(name);
    return it == nodes_.end() ? nullptr : &it->second;
}

float Economy::distance2D(float ax, float az, float bx, float bz) const {
    float dx = bx - ax, dz = bz - az;
    return sqrtf(dx * dx + dz * dz);
}

const Economy::CargoDef* Economy::findCargo(const std::string& id) const {
    for (const auto& c : cargo_)
        if (c.id == id) return &c;
    // the shipped town lists use one case-mismatched id (c_TABLEWARE vs
    // C_TABLEWARE), so fall back to a case-insensitive match
    std::string lo = lowerOf(id);
    for (const auto& c : cargo_)
        if (lowerOf(c.id) == lo) return &c;
    return nullptr;
}

void Economy::init(const GameData& gd) {
    ++loadStateRevision_;
    stats_ = Stats();
    cargo_.clear();
    towns_.clear();
    dealers_.clear();
    offers_.clear();
    specs_.clear();
    jobs_.clear();
    counters_.clear();
    unresolvedCatalog_.clear();
    dealerBaysMissing_ = 0;
    startX_ = gd.startPos[0];
    startZ_ = gd.startPos[1];
    initialized_ = true;

    buildCatalog(gd);
    buildDealers(gd);
    buildJobs(gd);
    resolvePositions();

    LOG("economy: %s", report().c_str());
    for (const auto& u : stats_.unresolved) LOG("economy: unresolved '%s'", u.c_str());
}

// evehicles.sch / merchandise.sch / vd.sch and the ENV counters.  Nothing here
// is hard-coded: the dealer stock, the cargo table and the start truck all come
// from the shipped scripts.
void Economy::buildCatalog(const GameData& gd) {
    std::map<std::string, std::string> vdNames;      // vehicle id -> display
    std::string playerModel;                         // GAME.CNF (live 1 "ZilR" 1)
    int playerVariant = 1;

    for (const auto& d : gd.documents()) {
        std::string lp = lowerOf(d.path);
        const bool isVehicles = lp.find("evehicles") != std::string::npos;
        const bool isCargo = lp.find("merchandise") != std::string::npos;
        const bool isVd = baseIs(lp, "vd.sch");
        const bool isGameCnf = baseIs(lp, "game.cnf");
        if (!isVehicles && !isCargo && !isVd && !isGameCnf) continue;

        if (isVehicles) {
            std::vector<const Sexp*> forms;
            for (const auto& f : d.forms) collectHead(f, "vehicle", 0, forms);
            for (const Sexp* e : forms) {
                Offer o;
                o.id = e->str(1);
                o.model = e->str(2);
                o.variant = (int)e->num(3);
                o.klass = e->str(4);
                // ( vehicle NAME "MODEL" VARIANT CLASS AI PRICE POWER (tanks) ):
                // the first number after the AI field is the price, the second
                // the per-model power/mass figure (docs/economy.md §2).
                std::vector<long> nums;
                for (size_t i = 6; i < e->size(); ++i)
                    if (e->isNumber(i)) nums.push_back((long)e->num(i));
                if (nums.size() > 0) o.price = nums[0];
                if (nums.size() > 1) o.power = nums[1];
                if (o.id.empty() || o.model.empty()) continue;
                ++stats_.vehicleDefs;
                if (o.price <= 0) { ++stats_.offersSkipped; continue; }
                offers_.push_back(o);
            }
        } else if (isCargo) {
            std::vector<const Sexp*> items;
            for (const auto& f : d.forms) {
                if (hasIdentHead(f) && f.size() >= 5 && f.at(4) && f.at(4)->isList())
                    items.push_back(&f);
                for (const auto& k : f.kids)
                    if (hasIdentHead(k) && k.size() >= 5 && k.at(4) && k.at(4)->isList())
                        items.push_back(&k);
            }
            for (const Sexp* e : items) {
                // ( ITEM UNIT WEIGHT VOLUME ( BUY SELL FACTOR (TAGS) ) F2 IDX "Display" )
                const CargoDef* dup = findCargo(e->str(0));
                if (dup) continue;
                const Sexp* pb = e->at(4);
                if (!pb || !isNumberLoose(pb, 0) || !isNumberLoose(pb, 1)) continue;
                CargoDef c;
                c.id = e->str(0);
                c.display = e->str(e->size() - 1);
                if (c.display.empty() || c.display == c.id) c.display = c.id;
                c.weight = (long)numLoose(e, 2);
                c.volume = (float)numLoose(e, 3);
                c.buy = (long)numLoose(pb, 0);
                c.sell = (long)numLoose(pb, 1);
                for (size_t i = 3; i < pb->size(); ++i) {
                    const Sexp* t = pb->at(i);
                    if (!t || !t->isList()) continue;
                    for (size_t k = 0; k < t->size(); ++k) {
                        std::string tag = lowerOf(t->str(k));
                        if (tag.empty()) continue;
                        if (!c.tags.empty()) c.tags += ",";
                        c.tags += tag;
                        if (tag == "trucks" || tag == "fuel") c.truckOk = true;
                        if (tag == "cars") c.carOk = true;
                        if (tag == "illegal") c.illegal = true;
                        if (tag == "formafia") c.mafia = true;
                    }
                }
                ++stats_.cargoDefs;
                cargo_.push_back(c);
            }
        } else if (isVd) {
            for (const auto& f : d.forms) {
                std::vector<const Sexp*> pairs;
                if (f.isList() && f.size() >= 3 && f.at(1) && f.at(1)->text == ".") pairs.push_back(&f);
                for (const auto& k : f.kids)
                    if (k.isList() && k.size() >= 3 && k.at(1) && k.at(1)->text == ".") pairs.push_back(&k);
                for (const Sexp* p : pairs)
                    if (!p->str(0).empty()) vdNames[p->str(0)] = p->str(2);
            }
        } else if (isGameCnf) {
            const bool env = inEnvDir(lp);
            if (!env && !counters_.empty()) continue;   // ENV is the loaded world
            std::vector<const Sexp*> forms;
            for (const auto& f : d.forms) {
                forms.push_back(&f);
                for (const auto& k : f.kids) forms.push_back(&k);
            }
            for (const Sexp* e : forms) {
                if (!e->isList() || e->head().empty()) continue;
                std::string h = e->head();
                if (h == "live" && e->size() >= 3 && !e->str(2).empty()) {
                    playerModel = e->str(2);
                    if (e->isNumber(3)) playerVariant = (int)e->num(3);
                } else if (e->isNumber(1)) {
                    counters_[h] = (long)e->num(1);
                }
            }
        }
    }

    // The parent (GameData) parses merchandise.sch too; when this module cannot
    // see the documents (older data dir) fall back to its table.
    if (cargo_.empty()) {
        for (const auto& g : gd.cargo) {
            CargoDef c;
            c.id = g.id;
            c.display = g.display.empty() ? g.id : g.display;
            c.buy = g.price;
            c.sell = g.sellPrice;
            c.weight = g.weight;
            c.volume = (float)g.volume;
            c.tags = g.tags;
            c.truckOk = g.tags.find("trucks") != std::string::npos ||
                        g.tags.find("fuel") != std::string::npos;
            c.carOk = g.tags.find("cars") != std::string::npos;
            c.illegal = g.tags.find("illegal") != std::string::npos;
            c.mafia = g.tags.find("formafia") != std::string::npos;
            ++stats_.cargoDefs;
            cargo_.push_back(c);
        }
        stats_.cargoFromGameData = cargo_.size();
    }

    for (auto& o : offers_) {
        auto it = vdNames.find(o.id);
        if (it != vdNames.end()) o.display = it->second;
    }
    std::sort(offers_.begin(), offers_.end(), [](const Offer& a, const Offer& b) {
        if (a.price != b.price) return a.price < b.price;
        return a.id < b.id;
    });
    stats_.offers = offers_.size();

    // The starting truck named by ENV/GAME.CNF ((live 1 "ZilR" 1)), else the
    // table GameData filled in.
    if (playerModel.empty()) playerModel = gd.playerVehicle;
    playerModel_ = playerModel;
    playerVariant_ = playerVariant;
    playerId_.clear();
    for (auto& o : offers_) {
        if (o.model == playerModel_ && o.variant == playerVariant_) {
            o.owned = true;
            playerId_ = o.id;
            break;
        }
    }
    if (playerId_.empty())
        for (auto& o : offers_)
            if (o.model == playerModel_) { o.owned = true; playerId_ = o.id; playerVariant_ = o.variant; break; }
}

// The car dealers of the manual sit on the bazaar parking lots; the trade stall
// is the bay with role 4 (it also names the town).  Positions come from .way.
// Prefer GameData::Parking::tile, retaining the document-path fallback for
// data assembled by older callers.
void Economy::buildDealers(const GameData& gd) {
    std::map<std::string, std::string> tiles;      // parking id -> ENV tile
    for (const auto& d : gd.documents()) {
        std::string lp = lowerOf(d.path);
        size_t a = lp.find("mod_");
        if (a == std::string::npos || a + 6 > lp.size()) continue;
        std::string tile = lp.substr(a + 4, 2);
        std::vector<const Sexp*> forms;
        for (const auto& f : d.forms) collectHead(f, "parking", 0, forms);
        for (const Sexp* e : forms)
            if (!e->str(1).empty()) tiles[e->str(1)] = tile;
    }

    for (const auto& pk : gd.parkings) {
        stats_.bazaarBays += pk.bays.size();
        if (pk.bazaar < 0) continue;                    // hidden/off-road group
        const GameData::Bay* stall = nullptr;
        for (const auto& b : pk.bays) {
            if (b.role == 4) { stall = &b; break; }
            if (!stall) stall = &b;                     // fall back to the first bay
        }
        if (!stall) { ++dealerBaysMissing_; continue; }
        Dealer dl;
        dl.id = pk.id;
        dl.display = stall->town.empty() ? pk.display : stall->town;
        auto it = tiles.find(pk.id);
        dl.tile = pk.tile.empty() && it != tiles.end() ? it->second : pk.tile;
        dl.node = stall->name;
        dealers_.push_back(dl);
    }
    stats_.dealers = dealers_.size();
}

// `town` records: the store/warehouse node (role `unload`) is both the pickup
// dock and the drop-off, and the goods list says what the town trades.
void Economy::buildJobs(const GameData& gd) {
    for (const auto& d : gd.documents()) {
        std::string lp = lowerOf(d.path);
        if (lp.find("mod_") == std::string::npos) continue;
        std::vector<const Sexp*> forms;
        for (const auto& f : d.forms) collectHead(f, "town", 0, forms);
        for (const Sexp* e : forms) {
            Town t;
            t.ident = e->str(1);
            t.display = e->str(2).empty() ? e->str(1) : e->str(2);
            t.id = (int)e->num(3);
            // nodes: ( node "WAY" [role] [town] ) inside the node list
            for (const auto& grp : e->kids) {
                if (!grp.isList() || grp.head() == "node") continue;
                for (const auto& n : grp.kids) {
                    if (!n.isList() || n.head() != "node") continue;
                    std::string role = n.str(2);
                    if (role == "unload" && t.unloadNode.empty()) t.unloadNode = n.str(1);
                }
            }
            // goods: ( ( ITEM STOCK ) ... ) — 2-element lists, fuel depots too
            for (const auto& grp : e->kids) {
                if (!grp.isList() || grp.size() < 2) continue;
                bool pairs = true;
                std::vector<std::string> ids;
                for (const auto& p : grp.kids) {
                    if (!p.isList() || p.size() != 2 || !p.isNumber(1) || p.str(0).empty()) { pairs = false; break; }
                    ids.push_back(p.str(0));
                }
                if (!pairs || ids.empty()) continue;
                std::set<std::string> seen;
                for (const auto& id : ids) {
                    ++stats_.goodsRefs;
                    if (!seen.insert(lowerOf(id)).second) continue;   // town lists repeat items
                    if (findCargo(id)) { t.goods.push_back(id); ++stats_.goodsResolved; }
                    else {
                        ++stats_.goodsUnresolved;
                        unresolvedCatalog_.push_back(id + " (" + t.ident + ")");
                    }
                }
            }
            // A market: it trades goods and owns a resolvable warehouse node.
            // Petrol stations (empty goods) and the FUEL depots (no `unload`
            // node) are not cargo destinations and are counted as skipped.
            if (t.ident.empty() || t.unloadNode.empty() || t.goods.empty()) {
                ++stats_.townsSkipped;
                continue;
            }
            if (!wayNode(t.unloadNode)) {
                ++stats_.townsSkipped;
                unresolvedCatalog_.push_back(t.unloadNode + " (" + t.ident + ")");
                continue;
            }
            towns_.push_back(t);
        }
    }
    stats_.towns = towns_.size();

    // Every market that trades an item can ship it to every other market; the
    // reward then grows with the distance (docs/economy.md §3).
    for (size_t a = 0; a < towns_.size(); ++a) {
        const Town& src = towns_[a];
        for (const auto& id : src.goods) {
            const CargoDef* c = findCargo(id);
            if (!c) continue;
            for (size_t b = 0; b < towns_.size(); ++b) {
                if (b == a) continue;
                JobSpec s;
                s.cargoIndex = (size_t)(c - &cargo_[0]);
                s.fromTown = a;
                s.toTown = b;
                s.job.cargoId = c->id;
                s.job.cargoName = c->display.empty() ? c->id : c->display;
                s.job.fromTown = src.display;
                s.job.toTown = towns_[b].display;
                s.job.fromNode = src.unloadNode;
                s.job.toNode = towns_[b].unloadNode;
                s.job.weight = c->weight;
                s.job.volume = c->volume;
                s.job.illegal = c->illegal;
                s.job.mafia = c->mafia;
                specs_.push_back(s);
            }
        }
    }
    stats_.jobs = specs_.size();
}

// Job and dealer positions, out of the .way table.  Jobs whose nodes are not in
// the table are dropped (never guessed) and counted.
void Economy::resolvePositions() {
    // re-resolving (a late node table) must not lose the player's progress
    std::set<std::string> done, active;
    for (const auto& j : jobs_) {
        std::string k = j.cargoId + "|" + j.fromNode + "|" + j.toNode;
        if (j.done) done.insert(k);
        if (j.active) active.insert(k);
    }
    jobs_.clear();
    size_t unresolved = 0;
    std::vector<std::string> unresolvedNames = unresolvedCatalog_;
    for (const auto& s : specs_) {
        const Way::Node* fn = wayNode(s.job.fromNode);
        const Way::Node* tn = wayNode(s.job.toNode);
        if (!fn || !tn) {
            ++unresolved;
            unresolvedNames.push_back(s.job.fromNode + " -> " + s.job.toNode);
            continue;
        }
        Job j = s.job;
        j.fromX = (float)fn->pos.x;
        j.fromZ = (float)fn->pos.z;
        j.toX = (float)tn->pos.x;
        j.toZ = (float)tn->pos.z;
        j.distance = distance2D(j.fromX, j.fromZ, j.toX, j.toZ);
        const CargoDef& c = cargo_[s.cargoIndex];
        long value = std::max(c.buy, c.sell);
        value = std::max(value, kCargoValueFloor);
        double km = j.distance / 1000.0;
        double reward = kRewardValueFrac * (double)value * (1.0 + km);
        long r = (long)llround(reward);
        if (r < kRewardMin) r = kRewardMin;
        if (r > kRewardCap) r = kRewardCap;
        j.reward = r;
        // Contract clock (the HUD's «ОСТ.ВРЕМЯ»): a fixed loading window plus
        // the run at an average lorry speed, so a fresh job reads ~25:00 exactly
        // like the reference frames (25:06 / 25:12).  The scripts carry no
        // deadline, so this is our own model - documented in docs/economy.md.
        j.timeLimit = kDetainTime + j.distance / kCruiseMps;
        j.timeLeft = j.timeLimit;
        std::string key = j.cargoId + "|" + j.fromNode + "|" + j.toNode;
        j.done = done.count(key) > 0;
        j.active = active.count(key) > 0;
        jobs_.push_back(j);
    }
    stats_.jobsResolved = jobs_.size();
    stats_.jobsUnresolved = unresolved;
    if(!active.empty()&&!activeJob())++loadStateRevision_;

    // dealer positions + the dealer the player starts next to
    float best = 0;
    int bestIdx = -1;
    size_t dealerUnresolved = dealerBaysMissing_;
    for (size_t i = 0; i < dealers_.size(); ++i) {
        const Way::Node* n = wayNode(dealers_[i].node);
        if (!n) {
            dealerUnresolved++;
            unresolvedNames.push_back(dealers_[i].node + " (" + dealers_[i].id + ")");
            dealers_[i].resolved = false;
            continue;
        }
        dealers_[i].resolved = true;
        dealers_[i].x = (float)n->pos.x;
        dealers_[i].z = (float)n->pos.z;
        float d = distance2D(startX_, startZ_, dealers_[i].x, dealers_[i].z);
        if (bestIdx < 0 || d < best) { best = d; bestIdx = (int)i; }
    }
    dealer_ = bestIdx;
    stats_.dealersUnresolved = dealerUnresolved;

    if (unresolvedNames.size() > 12) unresolvedNames.resize(12);
    stats_.unresolved = unresolvedNames;
}

// ---------------------------------------------------------------------------
// money / market
// ---------------------------------------------------------------------------

long Economy::ownedValue() const {
    for (const auto& o : offers_)
        if (o.id == playerId_) return o.price;
    for (const auto& o : offers_)
        if (o.model == playerModel_ && o.variant == playerVariant_) return o.price;
    return 0;
}

bool Economy::buy(size_t index, long tradeInCredit) {
    if (index >= offers_.size()) return false;
    Offer& o = offers_[index];
    if (o.owned || (o.model == playerModel_ && o.variant == playerVariant_)) return false;
    if (o.price <= 0) return false;
    if (tradeInCredit < 0) tradeInCredit = 0;
    if (tradeInCredit > o.price) tradeInCredit = o.price;
    if (money_ + tradeInCredit < o.price) return false;
    money_ -= o.price - tradeInCredit;
    o.owned = true;
    playerModel_ = o.model;
    playerId_ = o.id;
    playerVariant_ = o.variant;
    return true;
}

bool Economy::restorePlayerModel(const std::string& model) {
    if (model.empty()) return false;
    for (auto& o : offers_) {
        if (o.model != model) continue;
        o.owned = true;
        playerModel_ = o.model;
        playerId_ = o.id;
        playerVariant_ = o.variant;
        return true;
    }
    return false;
}

void Economy::restoreProgress(size_t delivered, int activeJobIndex) {
    ++loadStateRevision_;
    delivered_ = delivered;
    for (Job& j : jobs_) {
        j.active = false;
        j.done = false;
        j.timeLeft = j.timeLimit;
    }
    if (activeJobIndex >= 0 && (size_t)activeJobIndex < jobs_.size())
        jobs_[(size_t)activeJobIndex].active = true;
}

int Economy::dealerNear(float x, float z, float radius) const {
    int best = -1;
    float bestD = radius;
    for (size_t i = 0; i < dealers_.size(); ++i) {
        if (!dealers_[i].resolved) continue;
        float d = distance2D(x, z, dealers_[i].x, dealers_[i].z);
        if (d <= bestD) { bestD = d; best = (int)i; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// jobs
// ---------------------------------------------------------------------------

std::vector<size_t> Economy::jobsNear(float x, float z, float radius) const {
    std::vector<std::pair<float, size_t>> hits;
    for (size_t i = 0; i < jobs_.size(); ++i) {
        const Job& j = jobs_[i];
        if (j.done || j.fromNode.empty()) continue;
        float d = distance2D(x, z, j.fromX, j.fromZ);
        if (d <= radius) hits.push_back({d, i});
    }
    std::sort(hits.begin(), hits.end());
    std::vector<size_t> out;
    out.reserve(hits.size());
    for (const auto& h : hits) out.push_back(h.second);
    return out;
}

bool Economy::accept(size_t jobIndex) {
    if (jobIndex >= jobs_.size()) return false;
    if (activeJob()) return false;                    // only one job at a time
    Job& j = jobs_[jobIndex];
    if (j.done || j.fromNode.empty() || j.toNode.empty()) return false;
    j.active = true;
    ++loadStateRevision_;
    return true;
}

bool Economy::deliver(float x, float z, float radius, long* rewardOut) {
    for (auto& j : jobs_) {
        if (!j.active) continue;
        if (distance2D(x, z, j.toX, j.toZ) > radius) return false;
        // Late?  The contract still pays, but only a fraction (the HUD's
        // ОСТ.ВРЕМЯ cell has run out).
        long paid = j.reward;
        if (j.timeLeft <= 0.0f) {
            paid = (long)((double)j.reward * kLatePayFraction);
            ++late_;
        }
        j.active = false;
        j.done = true;
        ++loadStateRevision_;
        money_ += paid;
        ++delivered_;
        if (rewardOut) *rewardOut = paid;
        return true;
    }
    return false;
}

// The contract clock.  Only the active job runs; the parent calls this once per
// simulated frame with the world's dt (so a paused ESC screen does not tick it).
void Economy::tick(float dt) {
    if (dt <= 0.0f) return;
    for (auto& j : jobs_)
        if (j.active && j.timeLeft > 0.0f) {
            j.timeLeft -= dt;
            if (j.timeLeft < 0.0f) j.timeLeft = 0.0f;
        }
}

float Economy::jobTimeLeft() const {
    const Job* j = activeJob();
    return j ? j->timeLeft : 0.0f;
}

bool Economy::jobOverdue() const {
    const Job* j = activeJob();
    return j && j->timeLeft <= 0.0f;
}

std::string Economy::jobTimeText() const {
    const Job* j = activeJob();
    if (!j) return std::string();
    int total = (int)(j->timeLeft + 0.5f);
    if (total < 0) total = 0;
    // The original's cell is MM:SS and keeps counting past an hour as minutes.
    char buf[32];
    snprintf(buf, sizeof buf, "%02d:%02d", total / 60, total % 60);
    return std::string(buf);
}

const Economy::Job* Economy::activeJob() const {
    for (const auto& j : jobs_)
        if (j.active) return &j;
    return nullptr;
}

// ---------------------------------------------------------------------------
// reporting
// ---------------------------------------------------------------------------

std::string Economy::report() const {
    char buf[512];
    long forsale = 0, forhire = 0, missions = 0, fuel = 0;
    auto it = counters_.find("forsale");   if (it != counters_.end()) forsale = it->second;
    it = counters_.find("forhire");        if (it != counters_.end()) forhire = it->second;
    it = counters_.find("missions");       if (it != counters_.end()) missions = it->second;
    it = counters_.find("fuel");           if (it != counters_.end()) fuel = it->second;
    snprintf(buf, sizeof buf,
             "money=%ld player=%s/%s offers=%zu (skipped=%zu, %zu vehicle defs) "
             "cargo=%zu%s towns=%zu(markets,%zu skipped) dealers=%zu jobs=%zu "
             "(resolved=%zu unresolved=%zu) goods=%zu/%zu "
             "counters(forsale=%ld forhire=%ld missions=%ld fuel=%ld) "
             "late=%zu jobtime=%s",
             money_, playerModel_.c_str(), playerId_.c_str(),
             stats_.offers, stats_.offersSkipped, stats_.vehicleDefs,
             cargo_.size(), stats_.cargoFromGameData ? "(GameData fallback)" : "",
             stats_.towns, stats_.townsSkipped, stats_.dealers,
             stats_.jobs, stats_.jobsResolved, stats_.jobsUnresolved,
             stats_.goodsResolved, stats_.goodsRefs, forsale, forhire, missions, fuel,
             late_, jobTimeText().empty() ? "-" : jobTimeText().c_str());
    return buf;
}

} // namespace ht2

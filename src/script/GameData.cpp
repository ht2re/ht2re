#include "GameData.h"
#include "Sdte.h"
#include "../util/Log.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>

namespace ht2 {

// ---------------------------------------------------------------------------
// The (form, field) -> consumer table.  This is the single source of truth for
// docs/script_gameplay.md's coverage table and for `--script-report`; an empty
// string means no code reads the field ("DROPPED").
// ---------------------------------------------------------------------------
namespace {
struct FieldRef { const char* head; int index; const char* consumer; };

const FieldRef kConsumers[] = {
    {"vehicle", 1, "GameData::VehicleDef.name"}, {"vehicle", 2, "VehicleDef.model"},
    {"vehicle", 3, "VehicleDef.variant"}, {"vehicle", 4, "VehicleDef.klass"},
    {"vehicle", 5, "VehicleDef.aiKind"}, {"vehicle", 6, "VehicleDef.price"},
    {"vehicle", 7, "VehicleDef.mass (raw F2)"}, {"vehicle", 8, "trailers (unused)"},
    {"vehicle", 9, "tank flag (unused)"},
    {"node", 1, "Bay.name -> .way RNOD"},
    {"node", 2, "Bay.role / Bay.flags (unload|1 hidden|2 fuel|4 dummy|8)"},
    {"node", 3, "Bay.flags keyword or Bay.town (trade stall)"},
    {"town", 1, "Town.id"}, {"town", 2, "Town.display"}, {"town", 3, "Town.townId"},
    {"town", 4, "Town.kind (2 = petrol station)"}, {"town", 5, "Town.nodes"},
    {"town", 6, ""}, {"town", 7, "Town.goods (item, stock)"},
    {"town", 8, ""},
    {"parking", 1, "Parking.id"}, {"parking", 2, "Parking.display"},
    {"parking", 3, "Parking.bazaar"}, {"parking", 4, ""},
    {"parking", 5, "Parking.bays"}, {"parking", 6, "Parking.size"},
    {"service", 1, "Service.id"}, {"service", 2, "Service.display"},
    {"service", 3, "Service.type / Service.kind"}, {"service", 4, "Service.wayNode"},
    {"service", 5, "Service.upgrades (PR_X LEVELS)"}, {"service", 6, "Service.fuelId"},
    {"nodeevent", 1, "NodeEvent.id"}, {"nodeevent", 2, "NodeEvent.kindFlag (0/0x10)"},
    {"nodeevent", 3, "NodeEvent.wayNode"}, {"nodeevent", 4, "NodeEvent.dirId + dir vector"},
    {"nodeevent", 5, "NodeEvent.text"},
    {"ambush", 1, "Ambush.id"}, {"ambush", 2, "Ambush.klass"}, {"ambush", 3, "Ambush.tileTag"},
    {"ambush", 4, "Ambush.f4s"}, {"ambush", 5, "Ambush.wayNode"}, {"ambush", 6, "Ambush.dir"},
    {"ambush", 7, "Ambush.slots"}, {"ambush", 8, "Ambush.next"}, {"ambush", 9, "Ambush.radius"},
    {"obstacle", 1, "Obstacle.id"}, {"obstacle", 2, "Obstacle.kind"}, {"obstacle", 3, "Obstacle.tile"},
    {"obstacle", 4, "Obstacle.key (b3d subtag 21)"}, {"obstacle", 5, "Obstacle.debrisA"},
    {"obstacle", 6, "Obstacle.debrisB"}, {"obstacle", 7, "Obstacle.msgClose"},
    {"obstacle", 8, "Obstacle.msgOpen"}, {"obstacle", 9, "Obstacle.workNodes"},
    {"people", 1, "Gate.tile (tile tag)"}, {"people", 2, "the gate list"},
    {"DAYGW", 1, "Gate.key"}, {"DAYGW", 2, "Gate.prob"},
    {"DAYONLY", 1, "Gate.key"}, {"DAYONLY", 2, "Gate.prob"},
    {"GOODWEATHER", 1, "Gate.key"}, {"GOODWEATHER", 2, "Gate.prob"},
    {"UNCONDITIONAL", 1, "Gate.key"}, {"UNCONDITIONAL", 2, "Gate.prob"},
    {"playertypes", 1, "the counter list"}, {"playertypes", 2, "the counter list"},
    {"startposition", 1, "GameData::startPos[0]"}, {"startposition", 2, "startPos[1]"},
    {"startposition", 3, "startPos[2]"},
    {"scenario", 1, "GameData::worldName"}, {"world", 1, "GameData::worldName"},
    {"live", 1, "flag"}, {"live", 2, "GameData::playerVehicle"}, {"live", 3, "variant"},
    {"forsale", 1, "GameData::counters"}, {"forsale_used", 1, "GameData::counters"},
    {"forsale_stolen", 1, "GameData::counters"}, {"forhire", 1, "GameData::counters"},
    {"missions", 1, "GameData::counters"}, {"fuel", 1, "GameData::counters"},
    {"semitrailers", 1, "GameData::counters"}, {"containers", 1, "GameData::counters"},
    {"parkings", 1, "GameData::counters"}, {"ghost", 1, "GameData::ghost"},
    {"police", 1, "GameData::police"}, {"mafia", 1, "GameData::mafia"},
    {"aitruck", 1, "GameData::aiTrucks"}, {"dumb", 1, "GameData::dumb"},
    {"PR_FORCED_ENGINE", 1, "upgradeLevels level 0"}, {"PR_FORCED_ENGINE", 2, "upgradeLevels level 1"},
    {"PR_TIRES", 1, "upgradeLevels level 0"}, {"PR_TIRES", 2, "upgradeLevels level 1"},
    {"PR_TIRES", 3, "upgradeLevels level 2"},
};

const FieldRef* findRef(const std::string& head, size_t index) {
    for (const auto& r : kConsumers)
        if (head == r.head && (size_t)r.index == index) return &r;
    return nullptr;
}
} // namespace

bool isScriptFormHead(const std::string& h) {
    static const char* kHeads[] = {
        "vehicle", "node", "town", "parking", "service", "nodeevent", "ambush", "obstacle",
        "people", "DAYGW", "DAYONLY", "GOODWEATHER", "UNCONDITIONAL", "playertypes",
        "startposition", "scenario", "world", "live", "forsale", "forsale_used",
        "forsale_stolen", "forhire", "missions", "fuel", "semitrailers", "containers",
        "parkings", "ghost", "police", "mafia", "aitruck", "dumb",
    };
    for (const char* h2 : kHeads) if (h == h2) return true;
    return h.rfind("PR_", 0) == 0;
}

std::string scriptFieldConsumer(const std::string& head, size_t index) {
    if (index == 0) return "the form head";
    if (const FieldRef* r = findRef(head, index)) return r->consumer;
    if (head.rfind("PR_", 0) == 0 && index <= 3) {
        char buf[64];
        snprintf(buf, sizeof buf, "upgradeLevels level %zu", index - 1);
        return buf;
    }
    if (head == "people" || head == "playertypes") return "the repeated child list";
    return std::string();
}

// `Sexp::isNumber(i)` indexes a *list*'s children, so it is always false for an
// atom.  The loader needs a test for an arbitrary node.
static bool atomIsNumber(const Sexp& e) {
    if (e.kind != Sexp::Atom || e.quoted || e.text.empty()) return false;
    char* end = nullptr;
    strtod(e.text.c_str(), &end);
    return end && *end == '\0';
}

static bool endsWithCI(const std::string& s, const char* suf) {
    size_t n = strlen(suf);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (tolower((unsigned char)s[s.size() - n + i]) != tolower((unsigned char)suf[i])) return false;
    return true;
}

bool GameData::loadFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { errorMsgs.push_back("cannot open " + path); ++errors; return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> raw((size_t)(n > 0 ? n : 0));
    size_t got = raw.empty() ? 0 : fread(raw.data(), 1, raw.size(), f);
    fclose(f);
    if (got != raw.size()) { errorMsgs.push_back("short read " + path); ++errors; return false; }

    std::vector<uint8_t> plain = Sdte::decrypt(raw.data(), raw.size());
    Doc d;
    d.path = path;
    std::string err;
    std::string text((const char*)plain.data(), plain.size());
    if (!parseSexp(text, d.forms, &err)) {
        errorMsgs.push_back(path + ": " + err);
        ++errors;
        return false;
    }
    ingest(d);
    docs.push_back(std::move(d));
    return true;
}

// ---- coverage bookkeeping --------------------------------------------------
void GameData::noteField(const std::string& head, size_t index, const Sexp& kid) {
    FormCoverage& fc = coverage[head];
    fc.head = head;
    if (index == 0) { ++fc.count; fc.consumed = true; return; }
    std::string type = kid.isList() ? "list"
                     : (kid.quoted ? "string"
                                   : (atomIsNumber(kid) ? "number" : "symbol"));
    for (auto& f : fc.fields) {
        if ((size_t)f.index != index) continue;
        ++f.count;
        ++f.types[type];
        return;
    }
    FieldShape fs;
    fs.index = (int)index;
    fs.count = 1;
    fs.types[type] = 1;
    fc.fields.push_back(fs);
}

const GameData::FormCoverage* GameData::coverageFor(const std::string& head) const {
    auto it = coverage.find(head);
    return it == coverage.end() ? nullptr : &it->second;
}

size_t GameData::coverageFields() const {
    size_t n = 0;
    for (const auto& kv : coverage) n += kv.second.fields.size();
    return n;
}

size_t GameData::coverageDroppedFields() const {
    size_t n = 0;
    for (const auto& kv : coverage)
        for (const auto& f : kv.second.fields)
            if (scriptFieldConsumer(kv.first, (size_t)f.index).empty()) ++n;
    return n;
}

std::string GameData::coverageText() const {
    std::string out = "| form | field | shapes (count) | consumed by |\n"
                      "|------|-------|----------------|-------------|\n";
    // stable order: form by descending instance count, then field index
    std::vector<const FormCoverage*> forms;
    for (const auto& kv : coverage) forms.push_back(&kv.second);
    std::sort(forms.begin(), forms.end(), [](const FormCoverage* a, const FormCoverage* b) {
        if (a->count != b->count) return a->count > b->count;
        return a->head < b->head;
    });
    char buf[512];
    for (const FormCoverage* fc : forms) {
        for (const auto& f : fc->fields) {
            std::string shape;
            for (const auto& t : f.types) {
                if (!shape.empty()) shape += ",";
                snprintf(buf, sizeof buf, "%sx%zu", t.first.c_str(), t.second);
                shape += buf;
            }
            std::string consumer = scriptFieldConsumer(fc->head, (size_t)f.index);
            if (consumer.empty()) consumer = "**DROPPED** (nothing reads it)";
            snprintf(buf, sizeof buf, "| `%s` | %d | %s | %s |\n", fc->head.c_str(), f.index,
                     shape.c_str(), consumer.c_str());
            out += buf;
        }
    }
    return out;
}

std::string GameData::coverageSummary() const {
    char buf[256];
    snprintf(buf, sizeof buf, "coverage: heads=%zu form_heads=%zu fields=%zu dropped_fields=%zu",
             formCounts.size(), coverage.size(), coverageFields(), coverageDroppedFields());
    return buf;
}

// Walks the whole tree: every shipped file wraps its contents in one outer
// list, e.g. `( (playertypes ...) (startposition ...) )`.
void GameData::ingest(const Doc& d) {
    std::string lower = d.path;
    for (auto& c : lower) c = (char)tolower((unsigned char)c);
    const bool isGameCnf = endsWithCI(d.path, "game.cnf");
    const bool isEnvCnf = lower.find("/env/") != std::string::npos;
    const bool isEvehicles = lower.find("evehicles") != std::string::npos;
    const bool isRestable = lower.find("restable") != std::string::npos;
    const bool isTrashboxes = lower.find("etrashboxes") != std::string::npos;
    const bool isMod = lower.find("/mod_") != std::string::npos;
    const bool isCargo = lower.find("merchandise") != std::string::npos;
    const bool isNames = lower.find("names.sch") != std::string::npos;
    const bool isMs = lower.find("/ms.sch") != std::string::npos;
    std::string tile;
    if (isMod) {
        size_t a = lower.rfind("mod_");
        tile = lower.substr(a + 4, 2);
    }

    struct Walker {
        GameData* self;
        bool isGameCnf, isEnvCnf, isEvehicles, isRestable, isTrashboxes, isMod, isCargo,
             isNames, isMs;
        std::string tile;

        // `Sexp::isNumber(i)` indexes a *list*'s children, so it is always
        // false for an atom.  These two helpers test an arbitrary node.
        static long lastNumber(const Sexp& e) {
            long v = 0;
            bool got = false;
            for (const auto& k : e.kids) {
                if (k.isAtom() && !k.quoted && !k.text.empty()) {
                    char* end = nullptr;
                    double d = strtod(k.text.c_str(), &end);
                    if (end && *end == '\0') { v = (long)d; got = true; }
                } else if (k.isList()) {
                    long inner = lastNumber(k);
                    if (inner != 0) { v = inner; got = true; }
                }
            }
            return got ? v : 0;
        }

        // node keyword -> the engine's flag bit (king.exe 0x40f303..0x40f36c)
        static int roleFlag(const std::string& s) {
            if (s == "unload") return GameData::kNodeUnload;
            if (s == "hidden") return GameData::kNodeHidden;
            if (s == "fuel") return GameData::kNodeFuel;
            if (s == "dummy") return GameData::kNodeDummy;
            return 0;
        }

        // `node` fields 2 (and 3): either a keyword bit or a numeric kind.
        static void readNodeRole(const Sexp& n, GameData::Bay& b) {
            for (size_t i = 2; i <= 3; ++i) {
                const Sexp* f = n.at(i);
                if (!f || f->kind != Sexp::Atom) continue;
                if (f->quoted) { if (i == 3) b.town = f->text; continue; }
                int bit = roleFlag(f->text);
                if (bit) { b.flags |= bit; continue; }
                if (n.isNumber(i)) { b.role = (int)n.num(i); b.kind = b.role; }
            }
        }

        // `( town ID "display" townId kind (nodes) () (goods) (map) )` - the
        // kind slot shifts for petrol stations, so the blocks are recognised by
        // shape rather than by position.
        static bool looksLikeNodeList(const Sexp& l) {
            for (const auto& k : l.kids)
                if (k.isList() && k.head() == "node") return true;
            return false;
        }
        static bool looksLikeGoods(const Sexp& l) {
            if (l.kids.empty()) return false;
            for (const auto& k : l.kids) {
                if (!k.isList() || k.size() != 2) return false;
                if (k.head().empty() || !k.isNumber(1)) return false;
            }
            return true;
        }

        void walk(const Sexp& e, int depth) {
            if (depth > 10 || !e.isList()) return;
            std::string h = e.head();
            if (!h.empty()) self->formCounts[h]++;
            if (!h.empty() && isScriptFormHead(h)) {
                self->noteField(h, 0, e);
                for (size_t i = 1; i < e.size(); ++i) {
                    const Sexp* kid = e.at(i);
                    if (kid) self->noteField(h, i, *kid);
                }
            }

            if (isGameCnf && h == "playertypes") {
                // ENV is the world this engine loads, so its counters win over
                // the menu worlds (MENV / MTR) that sort after it.
                const bool env = isEnvCnf;
                for (const auto& pt : e.kids) {
                    if (!pt.isList()) continue;
                    std::string k = pt.head();
                    if (k.empty()) continue;
                    if (env || !self->counters.count(k)) self->counters[k] = (long)pt.num(1);
                    const bool take = env || !self->countersSeen_.count(k);
                    if (k == "live") {
                        std::string model = pt.str(2);
                        if (!model.empty() && (env || self->playerVehicle.empty()))
                            self->playerVehicle = model;
                    } else if (k == "aitruck") { if (take) self->aiTrucks = (int)pt.num(1); }
                    else if (k == "police") { if (take) self->police = (int)pt.num(1); }
                    else if (k == "mafia") { if (take) self->mafia = (int)pt.num(1); }
                    else if (k == "dumb") { if (take) self->dumb = (int)pt.num(1); }
                    else if (k == "ghost") { if (take) self->ghost = (int)pt.num(1); }
                    if (take) self->countersSeen_[k] = true;
                }
            } else if (isGameCnf && h == "startposition") {
                float sx = (float)e.num(1), sz = (float)e.num(2), sh = (float)e.num(3);
                if (isEnvCnf || !self->haveStart) {
                    self->startPos[0] = sx; self->startPos[1] = sz; self->startPos[2] = sh;
                    self->haveStart = true;
                }
            } else if (isGameCnf && h == "world") {
                if (isEnvCnf || self->worldName.empty()) self->worldName = e.str(1);
            } else if (isGameCnf && h != "playertypes" && h != "startposition" &&
                       h != "world" && !h.empty()) {
                // (forsale 25) (forhire 20) (missions 10) (fuel 10) ... - the
                // world counters, read by src/game/Economy.cpp
                if (e.size() >= 2 && e.isNumber(1) && (isEnvCnf || !self->counters.count(h)))
                    self->counters[h] = (long)e.num(1);
            } else if (isEvehicles && h == "vehicle") {
                VehicleDef v;
                v.name = e.str(1);
                v.model = e.str(2);
                v.variant = (int)e.num(3);
                v.klass = e.str(4);
                const Sexp* ai = e.at(5);
                if (ai) v.aiKind = ai->isAtom() ? ai->text : ai->head();
                std::vector<long> nums;
                for (size_t i = 6; i < e.size(); ++i)
                    if (e.isNumber(i)) nums.push_back((long)e.num(i));
                // evehicles.sch: field 6 is the price, field 7 the per-model
                // power/weight figure (the manual lists "model, power, capacity,
                // price"); the older code had them the other way round.
                if (nums.size() > 0) v.price = nums[0];
                if (nums.size() > 1) v.mass = nums[1];
                if (!v.name.empty()) self->vehicles.push_back(v);
            } else if (isTrashboxes && h == "vehicle") {
                ParkedDef v;
                v.model = e.str(2);
                v.variant = (int)e.num(3);
                v.klass = e.str(4);
                if (!v.model.empty()) self->parked.push_back(v);
            } else if (isMod && h == "parking") {
                Parking pk;
                pk.id = e.str(1);
                pk.display = e.str(2);
                pk.bazaar = (int)e.num(3);
                pk.tile = tile;
                const Sexp* tail = e.at(6);
                if (tail && tail->isAtom() && e.isNumber(6)) pk.size = (int)e.num(6);
                // the bays live in a nested list: ( (node "..") (node "..") )
                auto addBay = [&](const Sexp& n) {
                    if (!n.isList() || n.head() != "node") return;
                    Bay b;
                    b.name = n.str(1);
                    b.role = (int)n.num(2);
                    b.town = n.str(3);
                    readNodeRole(n, b);
                    if (!b.name.empty()) pk.bays.push_back(b);
                };
                for (const auto& grp : e.kids) {
                    if (!grp.isList()) continue;
                    if (grp.head() == "node") { addBay(grp); continue; }
                    for (const auto& n : grp.kids) addBay(n);
                }
                if (!pk.bays.empty()) self->parkings.push_back(pk);
            } else if (isMod && h == "town") {
                Town t;
                t.id = e.str(1);
                t.display = e.str(2);
                t.townId = (int)e.num(3);
                const Sexp* k4 = e.at(4);
                if (k4 && k4->kind == Sexp::Atom) t.kind = k4->text;
                t.tile = tile;
                for (size_t i = 4; i < e.size(); ++i) {
                    const Sexp* b = e.at(i);
                    if (!b || !b->isList()) continue;
                    if (looksLikeNodeList(*b)) {
                        for (const auto& n : b->kids) {
                            if (!n.isList() || n.head() != "node") continue;
                            Bay bay;
                            bay.name = n.str(1);
                            bay.role = (int)n.num(2);
                            bay.town = n.str(3);
                            readNodeRole(n, bay);
                            if (bay.name.empty()) continue;
                            if (bay.unload()) t.hasUnload = true;
                            t.nodes.push_back(bay);
                        }
                    } else if (looksLikeGoods(*b)) {
                        for (const auto& g : b->kids)
                            t.goods.emplace_back(g.head(), (long)g.num(1));
                    } else if (!b->kids.empty()) {
                        // field 8: the map icon descriptor (list of name groups)
                        for (const auto& g : b->kids)
                            for (const auto& nm : g.kids)
                                if (nm.kind == Sexp::Atom) t.mapIcons.push_back(nm.text);
                        if (t.mapIcons.empty())
                            for (const auto& nm : b->kids)
                                if (nm.kind == Sexp::Atom) t.mapIcons.push_back(nm.text);
                    }
                }
                t.petrol = (t.kind == "2");
                t.fuelDepot = !t.petrol && t.display.empty() &&
                              t.id.find("_FUEL") != std::string::npos;
                if (!t.id.empty()) self->towns.push_back(t);
            } else if (isCargo && depth == 1 && e.size() >= 8 &&
                       e.at(4)->isList() && e.at(4)->isGameNumber(0) &&
                       e.at(4)->isGameNumber(1)) {
                // (ID UNIT WEIGHT VOLUME (BUY SELL FACTOR (TAGS)) F2 IDX "Display")
                const Sexp& prices = *e.at(4);
                Cargo cg;
                cg.id = e.str(0);
                cg.unit = e.str(1);
                cg.weight = (long)e.gameNum(2);
                cg.volume = e.gameNum(3);
                cg.price = (long)prices.gameNum(0);
                cg.sellPrice = (long)prices.gameNum(1);
                cg.priceFactor = prices.gameNum(2);
                cg.secondaryFactor = e.gameNum(5);
                cg.index = (int)e.gameNum(6);
                cg.display = e.str(7);
                for (size_t i = 3; i < prices.size(); ++i) {
                    const Sexp* t = prices.at(i);
                    if (!t || !t->isList()) continue;
                    for (size_t k = 0; k < t->size(); ++k) {
                        const std::string tag = t->str(k);
                        if (tag.empty()) continue;
                        if (!cg.tags.empty()) cg.tags += ",";
                        cg.tags += tag;
                    }
                }
                if (!cg.id.empty() && cg.id[0] != '(') self->cargo.push_back(cg);
            } else if (isNames && depth == 1 && e.size() >= 3 && e.at(0) && e.at(0)->quoted) {
                Person ps;
                ps.name = e.str(0);
                ps.gender = e.str(1);
                ps.role = e.str(2);
                if (!ps.name.empty()) self->people.push_back(ps);
            } else if (isMs && depth == 1 && e.size() >= 3 && e.at(1) && e.at(1)->text == ".") {
                // ( KEY . "value" )
                std::string k = e.str(0), v = e.str(2);
                if (!k.empty()) self->messages[k] = v;
            } else if (isMod && h == "service") {
                Service sv;
                sv.id = e.str(1);
                sv.display = e.str(2);
                sv.type = e.str(3);
                sv.kind = (int)e.num(3);
                sv.wayNode = e.str(4);
                sv.tile = tile;
                for (const auto& grp : e.kids) {
                    if (!grp.isList()) continue;
                    // field 6 is the fuel id list: ( 4031 )
                    if (!grp.kids.empty() && grp.kids[0].isAtom() && grp.isNumber(0) &&
                        grp.head() != "PR_" && grp.head().rfind("PR_", 0) != 0)
                        if (sv.fuelId == 0 && grp.size() == 1) sv.fuelId = (int)grp.num(0);
                    for (const auto& up : grp.kids)
                        if (up.isList() && !up.head().empty() && up.isNumber(1))
                            sv.upgrades.emplace_back(up.head(), (int)up.num(1));
                }
                if (!sv.id.empty()) self->services.push_back(sv);
            } else if (isMod && h == "ambush") {
                Ambush am;
                am.id = e.str(1);
                am.klass = e.str(2);
                const Sexp* f3 = e.at(3);
                if (f3 && f3->kind == Sexp::Atom && f3->quoted) am.tileTag = f3->text;
                const Sexp* f4 = e.at(4);
                if (f4 && f4->kind == Sexp::Atom) am.f4s = f4->text;
                am.f3 = (int)e.num(3);
                am.f4 = (int)e.num(4);
                am.wayNode = e.str(5);
                am.dir = e.str(6);
                am.tile = tile;
                // field 7: the global ambush-slot ids (integers only); field 8:
                // the coupled ambush; field 9: the optional extra radius.
                const Sexp* sl = e.at(7);
                if (sl && sl->isList())
                    for (const auto& s : sl->kids)
                        if (atomIsNumber(s)) am.slots.push_back(s.text);
                const Sexp* nx = e.at(8);
                if (nx && nx->isList())
                    for (const auto& s : nx->kids)
                        if (s.isAtom() && !s.text.empty()) am.next.push_back(s.text);
                if (e.isNumber(9)) am.radius = (int)e.num(9);
                if (!am.id.empty()) self->ambushes.push_back(am);
            } else if (isMod && h == "people") {
                const std::string tileTag = e.str(1);
                for (const auto& grp : e.kids) {
                    if (!grp.isList()) continue;
                    Gate g;
                    g.kind = grp.head();
                    g.key = grp.str(1);
                    g.prob = (float)grp.num(2, 1.0);
                    g.tile = tileTag;
                    // king.exe 0x411f14..0x412077: exactly four condition heads
                    if (g.kind == "UNCONDITIONAL") g.kindId = 1;
                    else if (g.kind == "DAYONLY") g.kindId = 2;
                    else if (g.kind == "GOODWEATHER") g.kindId = 3;
                    else if (g.kind == "DAYGW") g.kindId = 4;
                    if (g.kindId && !g.key.empty()) self->gates.push_back(g);
                }
            } else if (isMod && h == "obstacle") {
                Obstacle ob;
                ob.id = e.str(1);
                ob.kind = e.str(2);
                ob.tile = e.str(3);
                ob.key = e.str(4);
                ob.debrisA = e.str(5);          // .way node of the first debris
                ob.debrisB = e.str(6);          // .way node of the second debris
                ob.msgClose = e.str(7);         // shown while the road is closed
                ob.msgOpen = e.str(8);          // shown once it is reopened
                const Sexp* w = e.at(9);
                if (w && w->isList())
                    for (const auto& n : w->kids)
                        if (n.isAtom() && !n.text.empty()) ob.workNodes.push_back(n.text);
                if (!ob.key.empty()) self->obstacles.push_back(ob);
            } else if (isMod && h == "nodeevent") {
                NodeEvent ne;
                ne.id = e.str(1);
                ne.kind = e.str(2);
                ne.wayNode = e.str(3);
                ne.dir = e.str(4);
                ne.text = e.isNumber(5) ? std::string() : e.str(5);
                ne.tile = tile;
                // king.exe 0x413d3a: STATEBORDER is bit 0x10, UNCONDITIONAL is 0
                ne.kindFlag = (ne.kind == "STATEBORDER") ? 0x10 : 0;
                // king.exe 0x413d89..0x413e84: direction enum + compass vector.
                // The vector is (x, second-world-axis): NORTH = +z, EAST = +x.
                if (ne.dir == "NORTH")      { ne.dirId = 1; ne.dirX = 0; ne.dirZ = 1; }
                else if (ne.dir == "SOUTH") { ne.dirId = 2; ne.dirX = 0; ne.dirZ = -1; }
                else if (ne.dir == "EAST")  { ne.dirId = 3; ne.dirX = 1; ne.dirZ = 0; }
                else if (ne.dir == "WEST")  { ne.dirId = 4; ne.dirX = -1; ne.dirZ = 0; }
                else                        { ne.dirId = 0; }
                if (!ne.id.empty() && !ne.wayNode.empty()) self->nodeEvents.push_back(ne);
            } else if (isRestable && h.rfind("PR_", 0) == 0) {
                // ( PR_X ("off" 0) ("on" 12500) ) -> the full level table
                std::vector<UpgradeLevel> levels;
                for (const auto& lv : e.kids) {
                    if (!lv.isList() || lv.size() < 2) continue;
                    UpgradeLevel l;
                    l.name = lv.str(0);
                    l.price = (long)lv.num(1);
                    levels.push_back(l);
                }
                long price = lastNumber(e);
                self->upgrades[h] = price;
                if (!levels.empty()) self->upgradeLevels[h] = levels;
            }
            for (const auto& k : e.kids) walk(k, depth + 1);
        }
    } w{this, isGameCnf, isEnvCnf, isEvehicles, isRestable, isTrashboxes, isMod, isCargo,
        isNames, isMs, tile};
    for (const auto& f : d.forms) w.walk(f, 0);
}

bool GameData::load(const std::string& dataDir) {
    const char* dirs[] = {"", "/ENV", "/MENV", "/MTR", "/COMMON"};
    std::vector<std::string> files;
    for (const char* sub : dirs) {
        std::string dir = dataDir + sub;
        DIR* dp = opendir(dir.c_str());
        if (!dp) continue;
        while (struct dirent* e = readdir(dp)) {
            std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            if (endsWithCI(n, ".sch") || endsWithCI(n, ".cnf"))
                files.push_back(dir + "/" + n);
        }
        closedir(dp);
    }
    std::sort(files.begin(), files.end());
    for (const auto& p : files) loadFile(p);
    size_t levelEntries = 0;
    for (const auto& kv : upgradeLevels) levelEntries += kv.second.size();
    LOG("scripts: %zu files decrypted+parsed, %zu errors, %zu vehicles, %zu upgrades "
        "(%zu levels), %zu parked models, %zu parking groups, %zu towns",
        docs.size(), errors, vehicles.size(), upgrades.size(), levelEntries,
        parked.size(), parkings.size(), towns.size());
    return errors == 0;
}

const Sexp* GameData::findForm(const std::string& head) const {
    for (const auto& d : docs)
        for (const auto& f : d.forms)
            if (f.isList() && f.head() == head) return &f;
    return nullptr;
}

std::string GameData::summary() const {
    char buf[640];
    snprintf(buf, sizeof buf,
             "files=%zu errors=%zu | player=%s start=(%.0f,%.0f,%.0f) world=%s "
             "ai=%d police=%d mafia=%d dumb=%d ghost=%d | vehicles=%zu upgrades=%zu parked=%zu parkings=%zu "
             "cargo=%zu drivers=%zu nodeevents=%zu messages=%zu gates=%zu obstacles=%zu "
             "services=%zu ambushes=%zu towns=%zu | %s",
             docs.size(), errors, playerVehicle.c_str(),
             startPos[0], startPos[1], startPos[2], worldName.empty() ? "-" : worldName.c_str(),
             aiTrucks, police, mafia, dumb, ghost, vehicles.size(), upgrades.size(),
             parked.size(), parkings.size(), cargo.size(), people.size(),
             nodeEvents.size(), messages.size(), gates.size(), obstacles.size(),
             services.size(), ambushes.size(), towns.size(), coverageSummary().c_str());
    return buf;
}

} // namespace ht2

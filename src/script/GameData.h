#pragma once
#include <string>
#include <vector>
#include <map>
#include "Sexp.h"

namespace ht2 {

// Typed view over the original game scripts (M3 groundwork).
//
// Every shipped .sch / .CNF is decrypted in memory (SDTE) and parsed as
// s-expressions; the engine then reads the tables it needs from them instead of
// hard-coding values.  Nothing is written to disk.
//
// The original scripts carry no evaluation forms at all: every condition is
// encoded in a *head keyword* (UNCONDITIONAL / DAYONLY / GOODWEATHER / DAYGW)
// or in an enumerated field whose rule lives in the engine.  Those rules were
// recovered from original/king.exe and are implemented in ScriptRules.{h,cpp};
// see docs/script_gameplay.md for the addresses and the coverage table.
class GameData {
public:
    struct VehicleDef {
        std::string name;      // "FIAT_1"
        std::string model;     // "Fiat" (the COMMON/TRUCKS.B3D container)
        int variant = 0;
        std::string klass;     // car / truck / tractor / lorry ...
        std::string aiKind;    // dumb / trucker / police / mafia ...
        long mass = 0;
        long price = 0;
    };
    struct Doc {
        std::string path;
        std::vector<Sexp> forms;
    };

    // Loads every *.sch / *.CNF under `dataDir` (top level, ENV/, MENV/, MTR/).
    bool load(const std::string& dataDir);
    // Loads one file (decrypt + parse).
    bool loadFile(const std::string& path);

    size_t files() const { return docs.size(); }
    size_t parseErrors() const { return errors; }
    const std::vector<std::string>& errorList() const { return errorMsgs; }
    const std::vector<Doc>& documents() const { return docs; }

    // ---- tables the engine consumes -------------------------------------
    // ENV/GAME.CNF (playertypes ...) -> the player's truck model name
    std::string playerVehicle = "ZilR";
    int aiTrucks = 0, police = 0, mafia = 0, dumb = 0;
    // the remaining (playertypes) counters: ghost, and the world caps
    // (forsale*, forhire, missions, fuel, semitrailers, containers, parkings)
    int ghost = 0;
    std::map<std::string, long> counters;
    // (startposition x z [h]) - x, z in world units, h = road height
    bool haveStart = false;
    float startPos[3] = {-3020.0f, -4625.0f, 20.0f};
    // evehicles.sch -> the whole vehicle catalogue
    std::vector<VehicleDef> vehicles;
    // restable.sch -> upgrade prices (PR_* -> price of the *last* level, kept
    // for the existing callers; `upgradeLevels` below has the full table)
    std::map<std::string, long> upgrades;
    // restable.sch -> the complete tuning table: PR_X -> (name, price) per level.
    // The level index is what `service`'s `( PR_X LEVELS )` refers to; level 0 is
    // always the "regular" factory part with price 0.
    struct UpgradeLevel { std::string name; long price = 0; };
    std::map<std::string, std::vector<UpgradeLevel>> upgradeLevels;
    // etrashboxes.sch: the parked/prop catalogue (class == "parked" is a car)
    struct ParkedDef { std::string model; int variant = 0; std::string klass; };
    std::vector<ParkedDef> parked;
    // mod_*.sch: a parking group and its bays (bay.name is a .way node name)
    //  `role`  = the numeric token of the form (4 trade stall, 8 hidden car,
    //            32 old car, 0 otherwise) - what the shipped data uses
    //  `flags` = the keyword bitmask (king.exe 0x40f303..0x40f36c):
    //            unload|1, hidden|2, fuel|4, dummy|8
    struct Bay {
        std::string name; int role = 0; std::string town;
        int flags = 0, kind = 0;
        bool unload() const { return (flags & 1) != 0; }
        bool hidden() const { return (flags & 2) != 0; }
        bool fuel() const { return (flags & 4) != 0; }
        bool dummy() const { return (flags & 8) != 0; }
    };
    static constexpr int kNodeUnload = 1, kNodeHidden = 2, kNodeFuel = 4, kNodeDummy = 8;
    struct Parking {
        std::string id, display; int bazaar = 0; std::string tile;
        std::vector<Bay> bays;
        int size = 0;            // optional trailing integer (5..22, bazaars only)
    };
    std::vector<Parking> parkings;
    // mod_*.sch `town`: the market / petrol station / fuel depot records.  The
    // economy reads its own copy; this one exists so the script runtime and the
    // coverage report see the same data.
    struct Town {
        std::string id, display, tile;
        int townId = 0;
        std::string kind;                 // "2" = petrol station, "" = town/depot
        std::vector<Bay> nodes;
        std::vector<std::pair<std::string, long>> goods;   // (item, initial stock)
        std::vector<std::string> mapIcons;                 // field 8, unread by us
        bool petrol = false, fuelDepot = false, hasUnload = false;
    };
    std::vector<Town> towns;
    // merchandise.sch: cargo items (id, display name, price, carrier-class tags)
    struct Cargo { std::string id, display; long price = 0; std::string tags; };
    std::vector<Cargo> cargo;
    // names.sch: the AI drivers (name, gender, role)
    struct Person { std::string name, gender, role; };
    std::vector<Person> people;
    // mod_*.sch: ( nodeevent ID KIND "waynode" DIR "text" ) - fired when the
    // player's road position becomes that node while travelling in DIR.
    struct NodeEvent {
        std::string id, kind, wayNode, dir, text, tile;
        int kindFlag = 0;                 // 0 = UNCONDITIONAL, 0x10 = STATEBORDER
        int dirId = 0;                    // 0 = ALL, 1 = NORTH, 2 = SOUTH, 3 = EAST, 4 = WEST
        double dirX = 0, dirZ = 0;        // king.exe 0x413d9f..0x413e84
        bool oneShot() const { return true; }   // 8 s re-trigger guard, 0x40b2a9
    };
    std::vector<NodeEvent> nodeEvents;
    // Ms.sch: the in-game message strings (dotted pairs KEY . "value")
    std::map<std::string, std::string> messages;
    // `people` day/night gates: ( people "tile" ( KIND "key" prob ) ... )
    // The key is a `subtag 21` record name in that tile's .b3d; the group is
    // shown or hidden depending on the time of day (and weather).
    struct Gate {
        std::string tile;      // "ad"
        std::string kind;      // DAYGW / DAYONLY / GOODWEATHER / UNCONDITIONAL
        std::string key;       // b3d key node name
        float prob = 1.0f;     // density/probability 0.3..1.0
        int kindId = 0;        // 1 UNCONDITIONAL, 2 DAYONLY, 3 GOODWEATHER, 4 DAYGW
    };
    std::vector<Gate> gates;
    // `obstacle`: ( obstacle ID KIND "tile" "b3dKey" "debrisA" "debrisB"
    //               ["closed text"] ["open text"] [("pos_work" …)] )
    struct Obstacle {
        std::string id, kind, tile, key;
        std::string debrisA, debrisB;      // .way nodes of the debris geometry
        std::string msgClose, msgOpen;     // fields 7 and 8
        std::vector<std::string> workNodes;// field 9, the repair-site .way nodes
    };
    std::vector<Obstacle> obstacles;
    // `service`: ( service ID "display" TYPE "waynode" ( (PR_X LEVELS) … ) [ID] )
    struct Service {
        std::string id, display, type, wayNode, tile;
        int kind = 0;                      // field 3 as an integer
        std::vector<std::pair<std::string, int>> upgrades;
        int fuelId = 0;                    // field 6 = 4000 + town id (fuel only)
    };
    std::vector<Service> services;
    // `ambush`: ( ambush ID CLASS F3 F4 "waynode" DIR ( slot… ) [( next )] [r] )
    struct Ambush {
        std::string id, klass, wayNode, dir, tile;
        std::string tileTag, f4s;          // fields 3 and 4 (tile tags in the north)
        int f3 = 0, f4 = 0;
        std::vector<std::string> slots;    // the numeric slot ids of field 7
        std::vector<std::string> next;     // field 8: the coupled ambush ids
        int radius = 0;                    // field 9 (65 / 85, 8 forms)
    };
    std::vector<Ambush> ambushes;
    // counts of the world-description forms, for reporting
    std::map<std::string, size_t> formCounts;
    // the game's own world list from GAME.CNF ((world MENV) / (world MTR))
    std::string worldName;

    const Sexp* findForm(const std::string& head) const;
    std::string summary() const;

    // ---- coverage: every form head and every field position ---------------
    // Filled while loading, so the report and the tests see the same numbers as
    // the parser.  `formCoverage` holds the grammar forms; `formCounts` holds
    // every head (including merchandise/vd/ms data heads).
    struct FieldShape {
        int index = 0;
        size_t count = 0;
        std::map<std::string, size_t> types;     // "string" / "number" / "symbol" / "list"
    };
    struct FormCoverage {
        std::string head;
        size_t count = 0;
        bool consumed = false;                   // the engine acts on this form
        std::string consumer;                    // where
        std::vector<FieldShape> fields;
    };
    const FormCoverage* coverageFor(const std::string& head) const;
    size_t coverageFields() const;
    size_t coverageDroppedFields() const;
    // markdown rows: form | field | shapes | consumed by (the doc's table)
    std::string coverageText() const;
    // one stable line for the regression tests
    std::string coverageSummary() const;

private:
    void ingest(const Doc& d);
    void noteField(const std::string& head, size_t index, const Sexp& kid);
    std::vector<Doc> docs;
    std::vector<std::string> errorMsgs;
    size_t errors = 0;
    std::map<std::string, FormCoverage> coverage;
    std::map<std::string, bool> countersSeen_;   // ENV-priority bookkeeping
};

// The (form, field) -> consumer table, shared by the report and the doc.
// An empty consumer means "nothing reads this field".
std::string scriptFieldConsumer(const std::string& head, size_t index);
// Form heads that the loader/engine treat as grammar.
bool isScriptFormHead(const std::string& head);

} // namespace ht2

#pragma once
#include <map>
#include <string>
#include <vector>
#include "../script/GameData.h"
#include "../world/Way.h"

namespace ht2 {

// Player economy: money, the vehicle market (buying a truck/car at a bazaar) and
// cargo transport jobs built from the shipped scripts (evehicles.sch prices,
// merchandise.sch cargo, the `town`/`parking` nodes of mod_*.sch).
//
// Everything positional comes from the .way node table that the world owner has
// already indexed (App::indexWayNodes -> App::wayNodes_): the scripts only name
// nodes, the coordinates live in ENV/<tile>.way.  The intended call order is
//
//     economy.setWayNodes(wayNodes_);
//     economy.init(gameData_);
//
// init() may be called again (e.g. once the node table exists) and keeps the
// player's money, the delivered counter and which job is active.  Calling
// setWayNodes() *after* init() re-resolves the jobs already built (a node that
// appears now becomes usable, one that disappears is dropped and counted); new
// towns appear after another init().
//
// See docs/economy.md for the reverse-engineered field layout, the formulas and
// the numbers this produces on the shipped data.
class Economy {
public:
    // The manual: "you have a mid-class truck and some funds".  The shipped
    // dealer catalogue (147 offers, 14000..118000) has a median price of 42000,
    // so 45000 buys about half of it - every standard car (32000) and every
    // lorry up to the Kamaz, none of the top rigs (see docs/economy.md §5).
    static constexpr long kStartMoney = 45000;

    // Reward: value = max(buy, sell, kCargoValueFloor); km = distance in km;
    // reward = clamp(round(kRewardValueFrac * value * (1 + km)), min, cap).
    static constexpr long kCargoValueFloor = 500;
    static constexpr double kRewardValueFrac = 0.20;
    static constexpr long kRewardMin = 200;
    static constexpr long kRewardCap = 250000;

    struct Offer {                 // a vehicle for sale
        std::string id, model;     // "FIAT_1", "Fiat"
        std::string display;       // vd.sch name ("Фиат"), empty when unknown
        std::string klass;         // car / lorry / tractor / semitrailer
        int variant = 0;
        long price = 0;            // evehicles.sch field 6 (see docs/economy.md)
        long power = 0;            // evehicles.sch field 7 (hp-like, per-model)
        bool owned = false;
    };
    struct Job {                   // a cargo delivery
        std::string cargoId, cargoName;
        std::string fromTown, toTown;
        std::string fromNode, toNode;             // the .way nodes used
        float fromX = 0, fromZ = 0, toX = 0, toZ = 0;
        long reward = 0;
        float distance = 0;                       // world metres
        long weight = 0;
        float volume = 0;
        // The contract clock the original shows as «ОСТ.ВРЕМЯ» in the HUD strip.
        // The scripts carry no deadline, so it is derived the way the original's
        // job list reads: a generous allowance for the distance at an average
        // lorry speed, plus a fixed loading window (25:00 at the reference's
        // 25:06 / 25:12 readings for a fresh job).
        float timeLimit = 0;                      // seconds allowed
        float timeLeft = 0;                       // seconds remaining
        // merchandise tags. trucks/fuel → truckOk, cars → carOk. The two
        // class tags are exclusive in the shipped table; illegal/mafia are not
        // a carrier class.
        bool truckOk = false, carOk = false;
        bool illegal = false, mafia = false;
        bool active = false, done = false;
    };
    struct Dealer {                // a bazaar trade stall (parking bay role 4)
        std::string id, display, tile, node;
        float x = 0, z = 0;
        bool resolved = false;     // the bay node was found in the .way table
    };
    struct Stats {                 // what resolved, what was skipped
        size_t vehicleDefs = 0, offers = 0, offersSkipped = 0;
        size_t cargoDefs = 0, cargoFromGameData = 0;
        size_t towns = 0, townsSkipped = 0;
        size_t goodsRefs = 0, goodsResolved = 0, goodsUnresolved = 0;
        size_t jobs = 0, jobsResolved = 0, jobsUnresolved = 0;
        size_t bazaarBays = 0, dealers = 0, dealersUnresolved = 0;
        std::vector<std::string> unresolved;      // first few names, for the log
    };

    // ---- setup -----------------------------------------------------------
    // The complete named-node table of the loaded world (name -> Way::Node).
    void setWayNodes(const std::map<std::string, Way::Node>& nodes);
    void init(const GameData& gd);

    // ---- money -----------------------------------------------------------
    long money() const { return money_; }
    void addMoney(long d) { money_ += d; }
    void setMoney(long m) { money_ = m; }
    size_t delivered() const { return delivered_; }
    size_t loadStateRevision() const { return loadStateRevision_; }
    // GAME.CNF counters as shipped: forsale, forsale_stolen, forsale_used,
    // forhire, missions, fuel, semitrailers, containers, parkings.
    const std::map<std::string, long>& counters() const { return counters_; }

    // ---- vehicle market --------------------------------------------------
    const std::vector<Offer>& offers() const { return offers_; }
    // Deducts the price (minus an optional trade-in credit) and re-points the
    // player at that model so the parent can respawn the truck.  False when the
    // index is bad, the vehicle is already owned, it has no price, or the money
    // (plus credit) does not cover it.
    bool buy(size_t index, long tradeInCredit = 0);
    // Restore a saved vehicle without charging for it.  Selects the first
    // catalogue variant with this model and marks it as owned.
    bool restorePlayerModel(const std::string& model);
    const std::string& playerModel() const { return playerModel_; }
    const std::string& playerVehicleId() const { return playerId_; }
    int playerVariant() const { return playerVariant_; }
    // Catalogue price of the vehicle the player drives now (0 when unknown);
    // the manual's "pay the difference" trade-in for buy().
    long ownedValue() const;

    // Bazaar trade stalls (out-of-town car dealers of the manual, "продавцы
    // машин" on the parking lots).  Positions come from the .way table.
    const std::vector<Dealer>& dealers() const { return dealers_; }
    bool haveDealer() const { return dealer_ >= 0 && (size_t)dealer_ < dealers_.size(); }
    const Dealer* dealer() const { return haveDealer() ? &dealers_[(size_t)dealer_] : nullptr; }
    int dealerNear(float x, float z, float radius) const;   // -1 when none in range

    // ---- cargo jobs ------------------------------------------------------
    const std::vector<Job>& jobs() const { return jobs_; }
    // Jobs whose pickup point is within `radius` of (x, z), nearest first.
    std::vector<size_t> jobsNear(float x, float z, float radius) const;
    // Same order as jobsNear, keeping only cargo this vehicle's class can
    // haul (lorry/tractor → trucks/fuel, car → cars). accept() itself stays
    // unfiltered.
    std::vector<size_t> jobsForPlayer(float x, float z, float radius) const;
    // A town terminal's windows, from the `town` node list (king.exe TownNode,
    // stride 0x10). Index 0 is the `unload` node: delivery only. formOrders
    // (0x41b720) skips it because flags & 9 (unload|1 or dummy|8). Windows 1+
    // each hold one offer. formOrders is scheduled every 10 s and does nothing
    // until |now| >= 20 s (0x64b6f0); a filled offer then lives
    // 200 + (rng % 300) seconds (0x41b562, 0x41b570) and is replaced when it
    // expires. The binary keeps two class slots per window (types 16 and 8);
    // the right-hand board shows the one this vehicle can haul.
    static constexpr float kWindowRadius = 10.0f;       // bays sit ~15 m apart
    static constexpr double kFormOrdersPeriod = 10.0;   // task +0x1c of formOrders
    static constexpr double kFormOrdersWarmup = 20.0;   // qword 0x64b6f0
    static constexpr double kOrderLifeBase = 200.0;     // 0xc8
    static constexpr int kOrderLifeSpan = 300;          // 0x12c
    struct TerminalWindow {
        int town = -1;
        int index = 0;                 // 0 = unload / delivery
        std::string name;
        float x = 0, z = 0;
        bool unload = false;
        int job = -1;                  // jobs() index, -1 when the window is empty
        double expires = 0;
    };
    const std::vector<TerminalWindow>& windows() const { return windows_; }
    // The window the truck is standing at, or a default (valid == false).
    TerminalWindow terminalAt(float x, float z) const;
    // Offers currently hanging on pickup windows (the Справка list).
    std::vector<int> postedJobs() const;
    bool canCarry(const Job& job) const;
    // "car" / "lorry" / "tractor" / "semitrailer", or empty when the driven
    // model is not in the catalogue.
    std::string playerKlass() const;
    bool accept(size_t jobIndex);                 // one active job at a time
    // Replace the small progress subset stored by HT2RE-SAVE.
    void restoreProgress(size_t delivered, int activeJobIndex);
    // Called with the current position: completes an active job at its drop-off.
    bool deliver(float x, float z, float radius, long* rewardOut);
    const Job* activeJob() const;
    float distance2D(float ax, float az, float bx, float bz) const;

    // ---- contract clock («ОСТ.ВРЕМЯ», additively added for the HUD) ---------
    // The deadline model above.  tick() is called once per simulated frame with
    // the dt the world ran; it only advances the *active* job.  A job that runs
    // out of time is delivered but only pays a fraction of the reward (the
    // original shows the ОСТ.ВРЕМЯ cell in red past zero).
    static constexpr float kDetainTime = 1500.0f;    // 25:00 fixed handling time
    static constexpr float kCruiseMps = 16.0f;       // ~58 km/h average lorry
    static constexpr float kLatePayFraction = 0.5f;  // paid when overdue

    // Arrival race recovered from loadOrder / finishPlayer (king.exe).
    // [0x68d164] is the 0-based place counter: 0 means nobody has finished yet.
    // A human who unloads while it is still 0 (0x444408) is first and is granted
    // hiring licences (0x4774e0, "ЛИЦЕНЗИЯ НА НАЙМ"). The count is
    // min(3, (int)sqrt(wins+1)) at 0x444474; a later place resets the streak
    // (0x4444b6). Each licence is an expiry timestamp (8-byte double); the
    // duration fild is 360 + (rng % 360) at 0x44432f. Expiry pops from the end
    // of the sorted vector while now > last expiry (0x47746f).
    // The original finisher is an AI driver (COMPETITOR_ARRIVED, 0x4443dc).
    // That driver is not traced here: the rival is scheduled to arrive once the
    // contract's own cruise time (distance / kCruiseMps) has elapsed.
    static constexpr int kLicenceDurationBase = 360;   // 0x168
    static constexpr int kLicenceDurationSpan = 360;
    static constexpr int kLicenceCountCap = 3;
    struct Arrival {
        bool valid = false;
        int place = 0;                 // 0 = YOU_CAME_0, first
        int licencesGranted = 0;
        long paid = 0;                 // the contract reward only
    };
    int licences() const { return (int)licences_.size(); }
    int winStreak() const { return wins_; }
    const Arrival& lastArrival() const { return lastArrival_; }
    // One-shot notices produced by tick()/deliver(). 0 when nothing happened.
    bool takeRivalNotice();
    int takeExpiredLicences();
    void tick(float dt);
    // Remaining seconds of the active job (0 when none / overdue).
    float jobTimeLeft() const;
    bool jobOverdue() const;
    // seconds left as "MM:SS" ("--:--" when no job is active)
    std::string jobTimeText() const;
    size_t lateDeliveries() const { return late_; }

    // ---- reporting -------------------------------------------------------
    const Stats& stats() const { return stats_; }
    const Way::Node* wayNode(const std::string& name) const;
    std::string report() const;                   // one line, for the log

private:
    struct CargoDef {
        std::string id, display, tags;
        long buy = 0, sell = 0, weight = 0;
        float volume = 0;
        bool truckOk = false, carOk = false, illegal = false, mafia = false;
    };
    struct Town {
        std::string ident, display, unloadNode;
        int id = -1;
        std::vector<std::string> goods;           // deduped cargo ids
        struct Bay { std::string name; bool unload = false; bool dummy = false; };
        std::vector<Bay> bays;
    };
    // A job before its .way positions are known.
    struct JobSpec {
        Job job;
        size_t cargoIndex = 0, fromTown = 0, toTown = 0;
    };

    void buildCatalog(const GameData& gd);
    void buildDealers(const GameData& gd);
    void buildJobs(const GameData& gd);
    void resolvePositions();
    const CargoDef* findCargo(const std::string& id) const;

    std::map<std::string, Way::Node> nodes_;
    std::vector<Offer> offers_;
    std::vector<Job> jobs_;
    std::vector<JobSpec> specs_;
    std::vector<CargoDef> cargo_;
    std::vector<Town> towns_;
    std::vector<Dealer> dealers_;
    std::vector<TerminalWindow> windows_;
    std::vector<std::vector<int>> townJobs_;   // jobs() indices leaving each town
    double formClock_ = 0;                     // seconds since the last formOrders pass
    unsigned rng_ = 1;
    std::map<std::string, long> counters_;
    std::vector<std::string> unresolvedCatalog_;   // names skipped while reading
    Stats stats_;
    int dealer_ = -1;
    size_t dealerBaysMissing_ = 0;
    float startX_ = 0, startZ_ = 0;
    long money_ = kStartMoney;
    size_t delivered_ = 0;
    size_t loadStateRevision_ = 0;
    size_t late_ = 0;
    double simTime_ = 0;               // seconds tick() has accumulated
    double acceptTime_ = 0;            // simTime_ when the active job was taken
    int place_ = 0;                    // [0x68d164], reset when a job is taken
    int wins_ = 0;                     // player+0x218, first-place streak
    bool rivalArrived_ = false;
    bool rivalNotice_ = false;
    int expiredPulse_ = 0;
    std::vector<double> licences_;     // expiry timestamps, soonest at the back
    Arrival lastArrival_;
    void noteRival();
    void grantLicences(int count, double duration);
    int expireLicences();
    void rotateOffers();
    unsigned rnd();
    std::string playerModel_, playerId_;
    int playerVariant_ = 0;
    bool initialized_ = false;
};

} // namespace ht2

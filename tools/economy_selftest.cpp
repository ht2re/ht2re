// economy_selftest.cpp -- standalone harness for src/game/Economy.{h,cpp}.
//
// It loads the shipped scripts (GameData) plus every ENV/<tile>.way node table,
// runs Economy::init(), and prints the numbers the module produced: dealer
// stock and price range, the dealers, the cargo jobs and their rewards, and a
// scripted buy/accept/deliver round trip.  Not part of the game build:
//
//   cmake --build build -j                       # the game itself must still compile
//   tools/build_economy_check.sh                 # builds + runs this harness
//
// See docs/economy.md.
#include "game/Economy.h"
#include "script/GameData.h"
#include "world/Way.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <dirent.h>
#include <map>
#include <string>
#include <vector>

using namespace ht2;

static int failures = 0;
static void check(bool ok, const char* what, const std::string& detail = std::string()) {
    printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", what,
           detail.empty() ? "" : "  | ", detail.c_str());
    if (!ok) ++failures;
}

int main(int argc, char** argv) {
    std::string data = argc > 1 ? argv[1] : "original";

    GameData gd;
    gd.load(data);
    printf("scripts: %s\n", gd.summary().c_str());

    std::map<std::string, Way::Node> nodes;
    DIR* dp = opendir((data + "/ENV").c_str());
    std::vector<std::string> ways;
    if (dp) {
        while (struct dirent* e = readdir(dp)) {
            std::string n = e->d_name;
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".way") == 0)
                ways.push_back(data + "/ENV/" + n);
        }
        closedir(dp);
    }
    std::sort(ways.begin(), ways.end());
    for (const auto& p : ways) {
        Way w;
        if (!w.load(p)) continue;
        for (const auto& room : w.rooms)
            for (const auto& n : room.nodes)
                if (!n.name.empty()) nodes[n.name] = n;
    }
    printf("way: %zu tiles, %zu named nodes\n", ways.size(), nodes.size());

    Economy ec;
    ec.setWayNodes(nodes);
    ec.init(gd);

    printf("report: %s\n", ec.report().c_str());
    // Remove only the cargo document: all other economic inputs stay identical.
    // The typed-table fallback must preserve every generated job field.
    GameData typed;
    bool fallbackLoaded = true;
    for (const auto& d : gd.documents())
        if (d.path.find("merchandise") == std::string::npos)
            fallbackLoaded = typed.loadFile(d.path) && fallbackLoaded;
    typed.cargo = gd.cargo;
    Economy fallback;
    fallback.setWayNodes(nodes);
    fallback.init(typed);
    bool same = fallback.jobs().size() == ec.jobs().size();
    for (size_t i = 0; same && i < ec.jobs().size(); ++i) {
        const auto& a = ec.jobs()[i];
        const auto& b = fallback.jobs()[i];
        same = a.cargoId == b.cargoId && a.cargoName == b.cargoName &&
               a.fromNode == b.fromNode && a.toNode == b.toNode &&
               a.weight == b.weight && a.volume == b.volume && a.reward == b.reward &&
               a.mafia == b.mafia && a.illegal == b.illegal;
    }
    check(fallbackLoaded && same && fallback.stats().cargoFromGameData == 107,
          "all cargo jobs match document and GameData fallback paths");
    size_t wageJobs = 0;
    bool wagePrices = true;
    for (const auto& j : ec.jobs()) if (j.cargoId == "C_WAGES") {
        ++wageJobs;
        long expected = (long)std::llround(Economy::kRewardValueFrac * 1000000 *
                                         (1.0 + j.distance / 1000.0));
        expected = std::max(Economy::kRewardMin, std::min(Economy::kRewardCap, expected));
        wagePrices = wagePrices && j.reward == expected && j.weight == 50 && j.mafia;
    }
    check(wageJobs > 0 && wagePrices, "C_WAGES jobs use million-unit sell price (port reward formula)");
    check(ec.stats().jobsResolved > 0, "jobs resolved to real .way nodes",
          std::to_string(ec.stats().jobsResolved) + " resolved, " +
              std::to_string(ec.stats().jobsUnresolved) + " unresolved");
    check(ec.stats().jobsUnresolved == 0, "no job silently unresolved");
    check(ec.stats().goodsUnresolved <= 1, "cargo ids in town lists resolve",
          std::to_string(ec.stats().goodsUnresolved) + " unresolved of " +
              std::to_string(ec.stats().goodsRefs));
    check(ec.offers().size() > 100, "dealer catalogue loaded",
          std::to_string(ec.offers().size()) + " offers");
    check(ec.haveDealer(), "a car dealer was placed from the .way table");

    // ---- offers ----------------------------------------------------------
    long lo = 0, hi = 0, sum = 0;
    for (const auto& o : ec.offers()) {
        if (!lo || o.price < lo) lo = o.price;
        if (o.price > hi) hi = o.price;
        sum += o.price;
    }
    long median = ec.offers().empty() ? 0 : ec.offers()[ec.offers().size() / 2].price;
    printf("offers: %zu  price %ld..%ld  median %ld  mean %ld  start money %ld\n",
           ec.offers().size(), lo, hi, median, ec.offers().empty() ? 0 : sum / (long)ec.offers().size(),
           Economy::kStartMoney);
    printf("offers: cheapest %s %s %ld | dearest %s %s %ld\n",
           ec.offers().front().id.c_str(), ec.offers().front().display.c_str(), ec.offers().front().price,
           ec.offers().back().id.c_str(), ec.offers().back().display.c_str(), ec.offers().back().price);

    // ---- dealers ---------------------------------------------------------
    for (const auto& d : ec.dealers())

        printf("dealer: %-12s %-12s tile=%s node=%-16s (%.0f, %.0f)%s\n",
               d.id.c_str(), d.display.c_str(), d.tile.c_str(), d.node.c_str(), d.x, d.z,
               d.resolved ? "" : " UNRESOLVED");
    if (const Economy::Dealer* d = ec.dealer())
        printf("dealer nearest the start: %s node=%s (%.0f, %.0f)\n",
               d->id.c_str(), d->node.c_str(), d->x, d->z);

    // ---- jobs ------------------------------------------------------------
    long rlo = 0, rhi = 0;
    for (const auto& j : ec.jobs()) {
        if (!rlo || j.reward < rlo) rlo = j.reward;
        if (j.reward > rhi) rhi = j.reward;
    }
    printf("jobs: %zu  all rewards %ld..%ld\n", ec.jobs().size(), rlo, rhi);

    std::vector<size_t> near = ec.jobsNear(gd.startPos[0], gd.startPos[1], 250.0f);
    printf("jobsNear(start): %zu\n", near.size());
    for (size_t i = 0; i < near.size() && i < 8; ++i) {
        const Economy::Job& j = ec.jobs()[near[i]];
        printf("  job %-16s %-12s -> %-12s %7.0f m  reward %-8ld node %s -> %s\n",
               j.cargoId.c_str(), j.fromTown.c_str(), j.toTown.c_str(), j.distance, j.reward,
               j.fromNode.c_str(), j.toNode.c_str());
    }

    // ---- money / purchase ------------------------------------------------
    check(ec.money() == Economy::kStartMoney, "start money", std::to_string(ec.money()));
    check(ec.ownedValue() > 0, "the start truck is in the dealer catalogue");
    printf("player: model=%s id=%s variant=%d start truck worth %ld\n",
           ec.playerModel().c_str(), ec.playerVehicleId().c_str(), ec.playerVariant(),
           ec.ownedValue());
    {
        const size_t cheap = 0;
        const long price = ec.offers()[cheap].price;
        const long before = ec.money();
        check(ec.buy(cheap), ("buy the cheapest offer: " + ec.offers()[cheap].id).c_str());
        check(ec.money() == before - price, "buy deducts the price",
              std::to_string(before) + " - " + std::to_string(price) + " = " + std::to_string(ec.money()));
        check(ec.playerVehicleId() == ec.offers()[cheap].id, "player re-pointed at the bought model");
        check(ec.buy(cheap) == false, "cannot buy a vehicle that is already owned");
        check(ec.buy(ec.offers().size() + 5) == false, "bad index refused");
        ec.setMoney(1);
        check(ec.buy(ec.offers().size() - 1) == false, "refused when the money does not cover it");
        ec.setMoney(before);
    }

    // ---- accept / deliver ------------------------------------------------
    if (!near.empty()) {
        const size_t idx = near[0];
        const Economy::Job j = ec.jobs()[idx];
        check(ec.accept(idx), "accept a nearby job");
        check(ec.activeJob() != nullptr, "one active job");
        size_t other = near.size() > 1 ? near[1] : idx;
        check(!ec.accept(other), "a second job is refused while one is active");
        long paid = 0;
        check(!ec.deliver(gd.startPos[0], gd.startPos[1], 10.0f, &paid), "far from drop-off pays nothing");
        const long before = ec.money();
        check(ec.deliver(j.toX, j.toZ, 10.0f, &paid), "deliver at the drop-off node");
        check(ec.delivered() == 1 && paid == j.reward && ec.money() == before + j.reward,
              "reward paid once and counted",
              "paid " + std::to_string(paid) + " delivered " + std::to_string(ec.delivered()));
        check(ec.activeJob() == nullptr, "no active job after delivery");
    } else {
        check(false, "jobsNear found at least one job at the start position");
    }

    ec.init(gd);   // re-init must keep money/delivered
    check(ec.delivered() == 1 && ec.money() > 0, "re-init keeps progress");

    // ---- robustness: missing names must be skipped, never guessed ---------
    {
        Economy bare;                     // no .way table at all
        bare.init(gd);
        const Economy::Stats& s = bare.stats();
        check(s.jobs == 0 && s.jobsResolved == 0,
              "empty .way table: no job is invented",
              "towns skipped " + std::to_string(s.townsSkipped) + ", dealers " +
                  std::to_string(s.dealersUnresolved) + " unresolved");
        check(s.townsSkipped > 0 && s.dealersUnresolved > 0 && !s.unresolved.empty(),
              "empty .way table: everything skipped is reported");
        check(!bare.haveDealer() && bare.dealerNear(0, 0, 1e9f) < 0, "empty .way table: no dealer");
        check(bare.jobsNear(0, 0, 1e9f).empty() && !bare.accept(0) && !bare.deliver(0, 0, 1e9f, nullptr),
              "empty .way table: jobsNear/accept/deliver stay empty/false");
        check(bare.money() == Economy::kStartMoney && bare.offers().size() > 100,
              "empty .way table: the catalogue still loads");
    }
    {
        // A node table that is missing names: with a *partial* table from the
        // start the affected towns never become markets, and a table that loses
        // nodes later (setWayNodes after init) drops those jobs and reports them.
        std::map<std::string, Way::Node> partial = nodes;
        partial.erase("node_store_0300");               // Южный's warehouse
        partial.erase("node_store_0800");               // Бухта's warehouse
        partial.erase("node_store_1000");               // Алмазный's warehouse
        partial.erase("node_car_0608");                 // MARKET_G's stall
        Economy hack;
        hack.setWayNodes(partial);
        hack.init(gd);
        const Economy::Stats first = hack.stats();
        check(first.towns == 8 && first.townsSkipped == 21 && first.jobsUnresolved == 0,
              "partial .way table: the three towns without a warehouse are dropped",
              std::to_string(first.towns) + " markets, " + std::to_string(first.townsSkipped) +
                  " skipped, " + std::to_string(first.jobsUnresolved) + " unresolved jobs");
        check(first.dealers == 15 && first.dealersUnresolved == 1 && hack.dealerNear(-2935, -4672, 50) < 0,
              "partial .way table: the missing dealer stall is reported and skipped");
        check(hack.haveDealer(), "partial .way table: another dealer takes over");

        hack.setWayNodes(nodes);                        // the full table comes back
        check(hack.stats().jobsResolved == 560,
              "a later setWayNodes only re-resolves the jobs already built",
              std::to_string(hack.stats().jobsResolved) + " jobs (8 markets)");
        hack.init(gd);                                  // re-init picks the towns up
        const Economy::Stats again = hack.stats();
        check(again.towns == 11 && again.jobsResolved == 1110 && again.jobsUnresolved == 0,
              "re-init with the full .way table restores every job",
              std::to_string(again.jobsResolved) + "/" + std::to_string(again.jobs) + " resolved");
        check(hack.money() == Economy::kStartMoney, "re-init keeps the money");

        Economy late;
        late.setWayNodes(nodes);
        late.init(gd);
        late.setWayNodes(partial);                      // nodes vanish after init
        const Economy::Stats lost = late.stats();
        check(lost.jobsResolved > 0 && lost.jobsUnresolved > 0 &&
                  lost.jobs == lost.jobsResolved + lost.jobsUnresolved,
              "jobs whose nodes vanish are dropped and counted",
              std::to_string(lost.jobsResolved) + " + " + std::to_string(lost.jobsUnresolved) +
                  " = " + std::to_string(lost.jobs));
        size_t bad = 0;
        for (const auto& j : late.jobs())
            if (j.fromNode.empty() || j.toNode.empty()) ++bad;
        check(bad == 0, "every job that survived has both nodes",
              std::to_string(bad) + " without a node");
    }

    printf("%s\n", failures ? "FAILURES" : "all economy self-tests passed");
    return failures ? 1 : 0;
}

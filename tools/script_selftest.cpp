// script_selftest.cpp -- standalone harness for src/script/ScriptRules.{h,cpp}.
//
// It is the `--script-report` producer: it loads the shipped scripts, builds the
// script runtime, prints the per-family coverage (`active / satisfied / fired`),
// the (form, field) coverage table and drives the recovered conditions with
// synthetic world states (a clock, a weather value, a money amount and a player
// path) so the rules can be checked without the renderer.
//
//   cmake --build build -j                # the game itself must still compile
//   tools/build_script_check.sh           # builds + runs this harness
//   python3 tools/shot_test.py            # incl. the checks below
//
// See docs/script_gameplay.md sections 9 and 10.
#include "script/GameData.h"
#include "script/ScriptRules.h"
#include "world/Way.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <map>
#include <string>
#include <vector>

using namespace ht2;

static int failures = 0;
static int checks = 0;
static void check(bool ok, const char* what, const std::string& detail = std::string()) {
    ++checks;
    printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", what,
           detail.empty() ? "" : "  | ", detail.c_str());
    if (!ok) ++failures;
}

static std::vector<ScriptNodePos> loadWayNodes(const std::string& data, size_t& tiles) {
    std::vector<ScriptNodePos> out;
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
    std::map<std::string, bool> seen;
    for (const auto& p : ways) {
        Way w;
        if (!w.load(p)) continue;
        ++tiles;
        for (const auto& room : w.rooms)
            for (const auto& n : room.nodes)
                if (!n.name.empty() && !seen.count(n.name)) {
                    seen[n.name] = true;
                    ScriptNodePos sp;
                    sp.name = n.name;
                    sp.x = n.pos.x;   // POSN = (x, z, height)
                    sp.z = n.pos.z;
                    out.push_back(sp);
                }
    }
    return out;
}

int main(int argc, char** argv) {
    std::string data = argc > 1 ? argv[1] : "original";
    for (int i = 1; i < argc; ++i)
        if (argv[i][0] == '-' && argv[i] != data.c_str()) { /* --report: print only */ }

    GameData gd;
    bool ok = gd.load(data);
    printf("scripts: %s\n", gd.summary().c_str());
    check(ok, "all 46 scripts decrypt and parse", gd.summary().c_str());

    if (argc > 3 && !strcmp(argv[2], "--state")) {
        // `--state HOUR,WEATHER[,MONEY]`: one frame of the recovered conditions,
        // printed as the same per-family lines `--script-report` produces.
        double hour = 12.0, money = 45000.0;
        float weather = 0.0f;
        sscanf(argv[3], "%lf,%f,%lf", &hour, &weather, &money);
        ScriptRuntime sr(gd);
        ScriptWorldState st;
        st.hour = hour;
        st.weather = weather;
        st.money = (long)money;
        st.time = 1000.0;
        st.havePosition = false;
        sr.update(st);
        printf("state hour=%.1f weather=%.2f money=%ld\n", hour, weather, (long)money);
        fputs(sr.reportText().c_str(), stdout);
        return 0;
    }
    if (argc > 2 && !strcmp(argv[2], "--coverage")) {
        // the engine's own (form, field) table - compared against
        // `python3 tools/script_coverage.py --md` by tools/shot_test.py
        fputs(gd.coverageText().c_str(), stdout);
        return 0;
    }

    size_t tiles = 0;
    std::vector<ScriptNodePos> nodes = loadWayNodes(data, tiles);
    printf("way: %zu tiles, %zu named nodes\n", tiles, nodes.size());

    ScriptRuntime rt(gd);
    rt.setNodePositions(nodes);

    // ---- the `--script-report` payload ----------------------------------
    printf("\n%s", rt.reportText().c_str());

    ScriptCoverage c = rt.coverage();
    check(c.gates == 66, "66 `people` gates are evaluated", std::to_string(c.gates));
    check(c.nodeEvents == 31 && c.nodeEventsActive == 31,
          "31 node events are live", std::to_string(c.nodeEvents));
    check(c.nodeEventsStateBorder == 12 && c.nodeEventsDirectional == 19,
          "12 STATEBORDER + 19 directional node events",
          std::to_string(c.nodeEventsStateBorder) + "/" + std::to_string(c.nodeEventsDirectional));
    check(c.obstacles == 17 && c.ambushes == 87 && c.services == 37 && c.towns == 29,
          "obstacle/ambush/service/town forms all loaded",
          std::to_string(c.obstacles) + "/" + std::to_string(c.ambushes) + "/" +
          std::to_string(c.services) + "/" + std::to_string(c.towns));

    // ---- the coverage table is stable -----------------------------------
    char cov[160];
    snprintf(cov, sizeof cov, "%s", gd.coverageSummary().c_str());
    check(strstr(cov, "heads=722") && strstr(cov, "dropped_fields=3"),
          "coverage table is stable (722 heads, 3 dropped fields)", cov);
    check(c.upgradeRows == 19 && c.upgradeLevels == 39,
          "restable.sch gives 19 upgrades / 39 levels",
          std::to_string(c.upgradeRows) + "/" + std::to_string(c.upgradeLevels));
    check(c.nodeEventsDirectional == 19 && c.nodeEventsAll == c.nodeEventsStateBorder,
          "direction enum recovered (ALL is only ever used with STATEBORDER)");

    // ---- time / weather windows -----------------------------------------
    auto runState = [&](double hour, float weather, double time) {
        ScriptWorldState st;
        st.time = time;
        st.hour = hour;
        st.weather = weather;
        st.havePosition = false;
        rt.update(st);
        return rt.coverage();
    };
    ScriptCoverage noon = runState(12.0, 0.0f, 100.0);
    ScriptCoverage night = runState(23.0, 0.0f, 100.0);
    ScriptCoverage bad = runState(12.0, 0.5f, 100.0);
    check(noon.gatesSatisfied == 66, "every gate is satisfied at noon + clear weather",
          std::to_string(noon.gatesSatisfied));
    check(night.gatesSatisfied == 5,
          "at 23:00 only the 5 UNCONDITIONAL gates hold",
          std::to_string(night.gatesSatisfied));
    check(bad.gatesSatisfied == 37,
          "bad weather drops the 29 DAYGW gates (5 + 32)",
          std::to_string(bad.gatesSatisfied));
    check(noon.gatesSatisfied != night.gatesSatisfied &&
          noon.gatesSatisfied != bad.gatesSatisfied,
          "day window 8..21 and weather < 0.2 are both honoured");

    // ---- tuning table / money -------------------------------------------
    const auto* tires = rt.levels("PR_TIRES");
    check(tires && tires->size() == 3, "PR_TIRES has 3 levels",
          tires ? std::to_string(tires->size()) : "missing");
    check(rt.upgradeName("PR_TIRES", 2) == "Off-road tires" &&
          rt.upgradePrice("PR_TIRES", 2) == 1500,
          "tuning table row (PR_TIRES, level 2)",
          rt.upgradeName("PR_TIRES", 2) + " " + std::to_string(rt.upgradePrice("PR_TIRES", 2)));
    check(rt.canBuy("PR_TIRES", 2, 1500) && !rt.canBuy("PR_TIRES", 2, 1000),
          "the money condition gates the upgrade",
          std::string("1500=") + (rt.canBuy("PR_TIRES", 2, 1500) ? "yes" : "no") +
          " 1000=" + (rt.canBuy("PR_TIRES", 2, 1000) ? "yes" : "no"));
    std::vector<UpgradeOffer> offers = rt.tuningOffers(45000);
    size_t affordable = 0, consistent = 0;
    for (const auto& o : offers) {
        if (o.affordable) ++affordable;
        // affordable <=> the row exists and the price is within the wallet
        const bool want = o.price == 0 || (o.price > 0 && 45000 >= o.price);
        if (want == o.affordable) ++consistent;
    }
    check(offers.size() == 559 && affordable == 549 && consistent == offers.size(),
          "the tuning UI table is exposed for the UI",
          std::to_string(offers.size()) + " offers, " + std::to_string(affordable) +
          " affordable, " + std::to_string(consistent) + " consistent");

    // ---- a directional node event fires only when travelling its way -----
    const GameData::NodeEvent* north = nullptr;
    for (const auto& e : gd.nodeEvents)
        if (e.dirId == 1) { north = &e; break; }
    check(north != nullptr, "a NORTH node event exists in the shipped data");
    if (north) {
        ScriptRuntime r2(gd);
        ScriptWorldState st;
        st.havePosition = true;
        st.time = 50.0;
        st.px = 0; st.pz = 0;
        r2.update(st);                       // establishes the previous position
        st.time = 50.1;
        st.px = 0; st.pz = 10;               // travelling +z = the NORTH vector
        r2.passNode(north->wayNode);
        r2.update(st);
        int firedNorth = 0, firedSouth = 0;
        for (const auto& e : r2.nodeEvents()) if (e.def == north) firedNorth = e.fires;
        // the same node passed while travelling the other way must not fire
        ScriptRuntime r3(gd);
        st.time = 50.0; st.px = 0; st.pz = 0;
        r3.update(st);
        st.time = 50.1; st.px = 0; st.pz = -10;
        r3.passNode(north->wayNode);
        r3.update(st);
        for (const auto& e : r3.nodeEvents()) if (e.def == north) firedSouth = e.fires;
        check(firedNorth == 1 && firedSouth == 0,
              "direction filter: +z passes, -z does not",
              north->id + " N=" + std::to_string(firedNorth) + " S=" + std::to_string(firedSouth));

        // ---- the 8 s guard and the one-shot policy ----------------------
        ScriptRuntime rc(gd);
        rc.setNodeEventOneShot(false);
        auto firesOf = [&](const ScriptRuntime& r) {
            int n = 0;
            for (const auto& e : r.nodeEvents()) if (e.def == north) n = e.fires;
            return n;
        };
        ScriptWorldState s2;
        s2.havePosition = true;
        s2.time = 100.0; s2.px = 0; s2.pz = 0; rc.update(s2);
        const int base = firesOf(rc);
        s2.time = 100.1; s2.pz = 10; rc.passNode(north->wayNode); rc.update(s2);
        const int afterFirst = firesOf(rc) - base;
        s2.time = 103.0; s2.pz = 20; rc.passNode(north->wayNode); rc.update(s2);
        const int withinWindow = firesOf(rc) - base;
        s2.time = 110.0; s2.pz = 30; rc.passNode(north->wayNode); rc.update(s2);
        const int afterCooldown = firesOf(rc) - base;
        check(afterFirst == 1 && withinWindow == 1 && afterCooldown == 2,
              "the 8 s guard suppresses a second pass inside the window",
              std::to_string(afterFirst) + "/" + std::to_string(withinWindow) + "/" +
              std::to_string(afterCooldown));

        ScriptRuntime r4(gd);
        r4.setNodeEventOneShot(true);
        ScriptWorldState s4;
        s4.havePosition = true;
        s4.time = 100.0; s4.px = 0; s4.pz = 0; r4.update(s4);
        s4.time = 100.1; s4.pz = 10; r4.passNode(north->wayNode); r4.update(s4);
        s4.time = 110.0; s4.pz = 30; r4.passNode(north->wayNode); r4.update(s4);
        int oneShotFires = 0;
        bool inactive = false;
        for (const auto& e : r4.nodeEvents())
            if (e.def == north) { oneShotFires = e.fires; inactive = !e.active; }
        check(oneShotFires == 1 && inactive, "a one-shot node event does not re-fire",
              std::to_string(oneShotFires));
    }

    // ---- ambush band 250..600 -------------------------------------------
    size_t ambIdx = rt.ambushes().size();
    for (size_t i = 0; i < rt.ambushes().size(); ++i)
        if (rt.ambushes()[i].havePos) { ambIdx = i; break; }
    check(ambIdx < rt.ambushes().size(), "an ambush resolved to its .way node");
    if (ambIdx < rt.ambushes().size()) {
        const ScriptNodePos* p = rt.nodePos(rt.ambushes()[ambIdx].def->wayNode);
        auto state = [&](double dz, double t) {
            ScriptWorldState st;
            st.havePosition = true;
            st.time = t;
            st.px = p->x; st.pz = p->z + dz;
            rt.update(st);
            return rt.ambushes()[ambIdx];
        };
        AmbushState far = state(900.0, 200.0);
        AmbushState band = state(300.0, 200.1);
        AmbushState near = state(100.0, 200.2);
        AmbushState back = state(900.0, 200.3);
        check(!far.active && far.armed && band.active && !near.active && !back.active && back.armed,
              "ambush activates only inside 250..600 u",
              std::string("far=") + (far.active ? "on" : "off") +
              " band=" + (band.active ? "on" : "off") +
              " near=" + (near.active ? "on" : "off"));
    }

    // ---- obstacle scheduler ---------------------------------------------
    {
        ScriptWorldState st;
        st.havePosition = true;
        st.time = 10.0;
        st.px = 1e6; st.pz = 1e6;                 // far from every obstacle
        rt.update(st);
        size_t before = rt.coverage().obstaclesClosed;
        st.time = 700.0; rt.update(st);
        ScriptCoverage c1 = rt.coverage();
        st.time = 2000.0; rt.update(st);
        ScriptCoverage c2 = rt.coverage();
        check(before == 0 && c1.obstaclesClosed == 1 && c2.obstaclesClosed == 1 &&
              c2.obstaclesToggled >= 2,
              "exactly one obstacle is closed, and it moves",
              std::to_string(before) + "/" + std::to_string(c1.obstaclesClosed) + "/" +
              std::to_string(c2.obstaclesToggled));
    }

    // ---- node role flags (king.exe 0x40f303..0x40f36c) ------------------
    {
        size_t unload = 0, fuel = 0, hidden = 0, dummy = 0, kind8 = 0, kind32 = 0, kind4 = 0;
        auto scan = [&](const GameData::Bay& b) {
            if (b.unload()) ++unload;
            if (b.fuel()) ++fuel;
            if (b.hidden()) ++hidden;
            if (b.dummy()) ++dummy;
            if (b.kind == 4) ++kind4;
            if (b.kind == 8) ++kind8;
            if (b.kind == 32) ++kind32;
        };
        for (const auto& pk : gd.parkings) for (const auto& b : pk.bays) scan(b);
        for (const auto& t : gd.towns) for (const auto& b : t.nodes) scan(b);
        check(unload == 25 && fuel == 18,
              "node keyword bits are decoded (unload|1, fuel|4)",
              std::to_string(unload) + "/" + std::to_string(fuel));
        check(hidden == 0 && dummy == 0,
              "`hidden`/`dummy` are never used as keywords on a named bay "
              "(the shipped data uses the numeric 8/32 instead)",
              std::to_string(hidden) + "/" + std::to_string(dummy));
        check(kind4 > 0 && kind8 > 0 && kind32 > 0,
              "node numeric roles are kept (4 trade stall, 8 hidden, 32 old)",
              std::to_string(kind4) + "/" + std::to_string(kind8) + "/" +
              std::to_string(kind32));
    }

    // ---- obstacle messages are not the debris names ---------------------
    {
        size_t withMsgs = 0, debrisSwapped = 0;
        for (const auto& o : gd.obstacles) {
            if (!o.msgClose.empty() && !o.msgOpen.empty()) ++withMsgs;
            if (o.msgClose.rfind("debris", 0) == 0 || o.msgOpen.rfind("debris", 0) == 0)
                ++debrisSwapped;
        }
        check(withMsgs == 7 && debrisSwapped == 0,
              "obstacle field 7/8 are the messages, 5/6 the debris",
              std::to_string(withMsgs) + " message pairs");
    }

    // ---- the second-pass constants (docs/script_gameplay.md 14) ----------
    {
        check(ScriptRules::kObstaclePlayerAway == 500.0 &&
              ScriptRules::kObstaclePlayerAway == ScriptRules::kObstacleMinSeparation,
              "the obstacle player-distance constant is the engine's 500 u "
              "(0x42b33a `fcomp qword [0x64b860]`)",
              std::to_string((int)ScriptRules::kObstaclePlayerAway) + " u");
        // king.exe 0x5a9d40: clearness 1 -> badness 0, clearness 0 -> badness 1
        const double clear = scriptBadness(1.0, 0.0, 0.0, 0.0);
        const double storm = scriptBadness(0.0, 0.0, 0.0, 0.0);
        const double mid = scriptBadness(0.5, 0.0, 0.0, 0.0);
        const double wrap = scriptBadness(0.5, 0.0, 0.0, 3.14159265358979);
        check(clear == 0.0 && storm == 1.0 && mid == 0.0 && wrap == 1.0,
              "the weather-badness curve 0x5a9d40 is reproduced "
              "(b = 0.5*(1+A+B*(1-|A|)*sin(phase-pi/2+t)))",
              std::to_string(clear) + "/" + std::to_string(storm) + "/" +
              std::to_string(mid) + "/" + std::to_string(wrap));
        // the curve must cross the 0.2 "good weather" threshold exactly twice
        // per period, i.e. the engine's good/bad window is a sinusoid
        int good = 0, bad = 0;
        for (int i = 0; i < 64; ++i) {
            double b = scriptBadness(0.8, 0.0, 0.0, i * 0.1);
            if (b < ScriptRules::kGoodWeatherMax) ++good; else ++bad;
        }
        check(good > 0 && bad > 0, "the curve crosses the 0.2 good-weather "
              "threshold over a period", std::to_string(good) + "/" +
              std::to_string(bad));
    }

    // ---- the engine's node pass is a road-position transition ------------
    {
        // a synthetic .way link: node A at (0,0), node B at (0,100), width 10
        ScriptRoadSeg seg;
        seg.nodeA = "A"; seg.nodeB = "B"; seg.width = 10.0;
        seg.xz = {0.0, 0.0, 0.0, 50.0, 0.0, 100.0};
        ScriptRuntime rt(gd);
        rt.setRoadSegments({seg});
        ScriptWorldState st;
        st.time = 0.0;
        st.hour = 12.0;
        st.weather = 0.0f;
        int fires = 0;
        // roll from 0 to 100 m along the link, one frame per metre
        for (int i = 0; i <= 100; ++i) {
            st.px = 0.0; st.pz = (double)i;
            st.time = i * 0.1;
            rt.update(st);
            fires += (int)rt.coverage().nodeEventsFired;
        }
        check(rt.haveRoadSegments() && rt.roadPasses() >= 1,
              "the road-position rule fires the node event at the link end",
              std::to_string(rt.roadPasses()) + " passes, " +
              std::to_string(fires) + " events");
        check(rt.currentRoadNode() == "B",
              "the player is on node B at the far end of the link",
              rt.currentRoadNode());
        // 60 m off the link is off-road: no road position, no pass
        ScriptRuntime rt2(gd);
        rt2.setRoadSegments({seg});
        for (int i = 0; i <= 20; ++i) {
            st.px = 60.0; st.pz = (double)i * 5.0; st.time = i * 0.1;
            rt2.update(st);
        }
        check(rt2.roadPasses() == 0 && rt2.currentRoadNode().empty(),
              "60 m aside is off-road and passes nothing",
              std::to_string(rt2.roadPasses()));
        // driving the link backwards passes B first and ends on A
        ScriptRuntime rt3(gd);
        rt3.setRoadSegments({seg});
        std::string firstPass;
        for (int i = 100; i >= 0; --i) {
            st.px = 0.0; st.pz = (double)i; st.time = (100 - i) * 0.1;
            size_t before = rt3.roadPasses();
            rt3.update(st);
            if (firstPass.empty() && rt3.roadPasses() > before) firstPass = rt3.currentRoadNode();
        }
        check(firstPass == "B" && rt3.currentRoadNode() == "A",
              "the rule is direction-agnostic in geometry (B first when driving "
              "the link backwards, A at the end)",
              firstPass + " -> " + rt3.currentRoadNode());
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

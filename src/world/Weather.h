#pragma once
#include <string>
#include <vector>

namespace ht2 {

// The original's weather, reverse engineered from the shipped data.
//
// `original/WEATHER.INI` is the whole parameter set: eight region presets
// (`[WEATHER1]`..`[WEATHER8]`), each holding, for four times of day
// (`night`/`srise`/`day`/`sset`) crossed with two weather qualities
// (`good`/`bad`):
//
//   fogn_/fogf_     fog near / far distance in world units = the DRAW DISTANCE
//                   (250 -> 900 in fair weather, 1 -> 150 in the tunnels)
//   fog_            fog (= horizon) colour, RGB 0..255
//   sunsky_         sky colour, suncld_ cloud colour, moon_ moon colour
//   skyamb_/envamb_/sunenv_   ambient and sun colours
//   frpl_           rain/snow particle density 0..1
//   sun=off rain=off thund=off    per-preset switches (the tunnels)
//
// king.exe loads it in one loop (0x5a7b50) that formats "WEATHER%d", calls
// GetPrivateProfileStringA for every key and parses the RGB triples at
// 0x5a88f0; the runtime interpolates between two presets in 0x5a8e90 with the
// blend factor at 0x6fb3e0 (the region manager changes presets as the player
// drives between them, and the weather crosses over rather than snapping).
// The `GOODWEATHER` people-gate key is the engine's good/bad boolean.
//
// Nothing in the shipped scripts selects a preset: the .sch/.CNF files contain
// no weather form at all, so the app exposes `--weather` / `--weather-set` and
// documents the region table as the part that is conceded (see
// docs/weather.md).
class Weather {
public:
    // Order used by the .INI and by Params().
    enum Phase { PH_NIGHT = 0, PH_RISE, PH_DAY, PH_SET, PH_COUNT };

    struct Params {
        float fogNear = 250.0f;
        float fogFar = 900.0f;
        float fog[3] = {1.0f, 1.0f, 0.9f};
        float sunsky[3] = {0.6f, 1.0f, 1.0f};
        float suncld[3] = {1.0f, 1.0f, 1.0f};
        float skyamb[3] = {0.8f, 0.8f, 0.8f};
        float envamb[3] = {0.3f, 0.3f, 0.3f};
        float sunenv[3] = {1.0f, 1.0f, 0.9f};
        float moon[3] = {0.0f, 0.0f, 0.0f};
        float frpl = 1.0f;          // precipitation density 0..1
        bool sun = true, rain = false, thund = false;
    };

    // Section = [WEATHER<1..setCount()>], quality = false for "good", true for
    // "bad", phase = one of Phase.  Returns false when the set/phase is absent.
    bool params(int setIndex, bool bad, int phase, Params& out) const;

    bool load(const std::string& path);
    bool loaded() const { return !sets_.empty(); }
    int setCount() const { return (int)sets_.size(); }
    const std::string& path() const { return path_; }
    // One-line summary for the startup log.
    std::string describe() const;

    // --- helpers shared by the app -----------------------------------------
    // Time of day -> Phase.  The original blends the four phases; we snap to
    // the nearest one (documented as a simplification).
    static int phaseForHour(float hour);
    static const char* phaseName(int phase);
    // Parse a `--weather` argument: "clear"/"good", "rain", "snow", "bad",
    // "fog", or a preset number 1..8.  Returns false for an unknown name.
    struct Selection { bool bad = false; int set = 1; bool rain = false, snow = false; };
    static bool parseSelection(const std::string& name, int setCount, Selection& out);

private:
    struct Entry {
        bool present = false;
        Params p;
    };
    // sets_[set][bad][phase]
    std::vector<Entry> sets_;       // index = (set-1)*8 + bad*4 + phase
    std::string path_;
};

} // namespace ht2

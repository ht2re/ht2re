#include "Weather.h"
#include "../util/Log.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cctype>
#include <algorithm>

namespace ht2 {

static const char* kPhase[Weather::PH_COUNT] = {"night", "srise", "day", "sset"};
static const char* kQual[2] = {"good", "bad"};

const char* Weather::phaseName(int phase) {
    return (phase >= 0 && phase < PH_COUNT) ? kPhase[phase] : "?";
}

int Weather::phaseForHour(float hour) {
    // The four keys of WEATHER.INI with the hours they stand for.  The original
    // cross-fades between them; the boundaries here are the midpoints of a
    // 24 h day laid out as night / sunrise / day / sunset.
    while (hour < 0.0f) hour += 24.0f;
    while (hour >= 24.0f) hour -= 24.0f;
    if (hour < 5.0f) return PH_NIGHT;
    if (hour < 8.0f) return PH_RISE;
    if (hour < 18.0f) return PH_DAY;
    if (hour < 21.5f) return PH_SET;
    return PH_NIGHT;
}

// --- a minimal GetPrivateProfileStringA equivalent --------------------------
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static bool parseTriple(const std::string& v, float out[3]) {
    // "255 217 127" (the .INI also carries ';' comments after the values)
    int r = 0, g = 0, b = 0;
    if (sscanf(v.c_str(), "%d %d %d", &r, &g, &b) != 3) return false;
    out[0] = std::min(255, std::max(0, r)) / 255.0f;
    out[1] = std::min(255, std::max(0, g)) / 255.0f;
    out[2] = std::min(255, std::max(0, b)) / 255.0f;
    return true;
}

bool Weather::load(const std::string& path) {
    path_ = path;
    sets_.clear();
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { LOG("weather: cannot open %s", path.c_str()); return false; }
    std::vector<std::pair<std::string, std::string>> kv;
    std::string section;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        std::string s(line);
        size_t semi = s.find(';');
        if (semi != std::string::npos) s = s.substr(0, semi);
        s = trim(s);
        if (s.empty()) continue;
        if (s[0] == '[') {
            size_t e = s.find(']');
            section = trim(s.substr(1, e == std::string::npos ? std::string::npos : e - 1));
            continue;
        }
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(s.substr(0, eq));
        std::string val = trim(s.substr(eq + 1));
        if (section.empty() || key.empty()) continue;
        kv.emplace_back(section + "." + key, val);
    }
    fclose(f);
    if (kv.empty()) { LOG("weather: %s has no [WEATHERn] keys", path.c_str()); return false; }

    // Index the sections: "WEATHER7" -> 7 (case-insensitive, tolerant of text
    // before/after the digits like the exe's sscanf("%d")).
    auto sectionIndex = [](const std::string& sec) {
        const char* p = sec.c_str();
        while (*p && !isdigit((unsigned char)*p)) ++p;
        return *p ? atoi(p) : -1;
    };
    int maxSet = 0;
    for (auto& e : kv) {
        std::string sec = e.first.substr(0, e.first.find('.'));
        maxSet = std::max(maxSet, sectionIndex(sec));
    }
    if (maxSet <= 0) { LOG("weather: no numbered [WEATHERn] section in %s", path.c_str()); return false; }
    sets_.assign((size_t)maxSet * 2 * PH_COUNT, Entry());
    // Defaults: the first present phase of the set/quality is copied into the
    // phases the file leaves out (WEATHER5/8 only define a few keys).
    int applied = 0;
    for (auto& e : kv) {
        size_t dot = e.first.find('.');
        std::string sec = e.first.substr(0, dot), key = e.first.substr(dot + 1);
        int set = sectionIndex(sec);
        if (set <= 0) continue;
        int phase = PH_DAY, qual = 0;             // qual 0 = good, 1 = bad
        bool havePhase = false;
        for (int p = 0; p < PH_COUNT; ++p)
            if (key.size() > strlen(kPhase[p]) &&
                key.compare(key.size() - strlen(kPhase[p]), strlen(kPhase[p]), kPhase[p]) == 0) {
                phase = p; havePhase = true; break;
            }
        for (int q = 0; q < 2; ++q)
            if (key.find(kQual[q]) != std::string::npos) { qual = q; break; }
        Entry& ent = sets_[((size_t)set - 1) * 2 * PH_COUNT + (size_t)qual * PH_COUNT + phase];
        ent.present = true;
        Params& P = ent.p;
        // <colour>_<quality><phase>, e.g. "fogf_goodday" / "fogn_badnight";
        // the tunnels only carry "<colour><phase>" (WEATHER5/8 have no quality).
        std::string stem = key;
        if (havePhase) stem.resize(stem.size() - strlen(kPhase[phase]));
        for (int q = 0; q < 2; ++q) {
            size_t ql = strlen(kQual[q]);
            if (stem.size() >= ql && stem.compare(stem.size() - ql, ql, kQual[q]) == 0) {
                stem.resize(stem.size() - ql);
                break;
            }
        }
        std::string base = stem;
        while (!base.empty() && base.back() == '_') base.pop_back();
        if (base == "fogn") { P.fogNear = (float)atof(e.second.c_str()); ++applied; }
        else if (base == "fogf") { P.fogFar = (float)atof(e.second.c_str()); ++applied; }
        else if (base == "frpl") { P.frpl = (float)atof(e.second.c_str()); ++applied; }
        else if (base == "fog") { parseTriple(e.second, P.fog); ++applied; }
        else if (base == "sunsky") { parseTriple(e.second, P.sunsky); ++applied; }
        else if (base == "suncld") { parseTriple(e.second, P.suncld); ++applied; }
        else if (base == "skyamb") { parseTriple(e.second, P.skyamb); ++applied; }
        else if (base == "envamb") { parseTriple(e.second, P.envamb); ++applied; }
        else if (base == "sunenv") { parseTriple(e.second, P.sunenv); ++applied; }
        else if (base == "moon") { parseTriple(e.second, P.moon); ++applied; }
        else if (key == "sun") { P.sun = (e.second != "off" && e.second != "0"); ++applied; }
        else if (key == "rain") { P.rain = (e.second != "off" && e.second != "0"); ++applied; }
        else if (key == "thund") { P.thund = (e.second != "off" && e.second != "0"); ++applied; }
    }
    LOG("weather: %s: %d presets, %d keys applied", path.c_str(), maxSet, applied);
    return true;
}

bool Weather::params(int setIndex, bool bad, int phase, Params& out) const {
    if (sets_.empty()) return false;
    if (setIndex < 1) setIndex = 1;
    int count = setCount();
    if (setIndex > count) setIndex = count;
    if (phase < 0 || phase >= PH_COUNT) phase = PH_DAY;
    size_t base = ((size_t)setIndex - 1) * 2 * PH_COUNT + (size_t)(bad ? 1 : 0) * PH_COUNT;
    // Walk outward for the nearest defined phase (and fall back to the other
    // quality) so a sparse preset like [WEATHER5] still yields a full set.
    for (int d = 0; d < PH_COUNT; ++d) {
        for (int sgn = 0; sgn < 2; ++sgn) {
            int p = phase + (sgn ? -d : d);
            if (p < 0 || p >= PH_COUNT) continue;
            if (sets_[base + (size_t)p].present) { out = sets_[base + (size_t)p].p; return true; }
        }
    }
    size_t other = ((size_t)setIndex - 1) * 2 * PH_COUNT + (size_t)(bad ? 0 : 1) * PH_COUNT;
    for (int p = 0; p < PH_COUNT; ++p)
        if (sets_[other + (size_t)p].present) { out = sets_[other + (size_t)p].p; return true; }
    return false;
}

std::string Weather::describe() const {
    char buf[192];
    snprintf(buf, sizeof buf, "weather: %s, %d presets (%s)", path_.c_str(), setCount(),
             sets_.empty() ? "not loaded" : "WEATHER1..N from the original WEATHER.INI");
    return std::string(buf);
}

bool Weather::parseSelection(const std::string& name, int setCount, Selection& out) {
    std::string s = name;
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    if (s.empty()) return false;
    if (isdigit((unsigned char)s[0])) {
        out.set = atoi(s.c_str());
        out.bad = false;
        out.rain = out.snow = false;
        return out.set >= 1 && out.set <= (setCount > 0 ? setCount : 8);
    }
    if (s == "clear" || s == "good" || s == "sun" || s == "sunny") {
        out.bad = false; out.rain = out.snow = false; return true;
    }
    if (s == "rain" || s == "wet") { out.bad = true; out.rain = true; out.snow = false; return true; }
    if (s == "snow") { out.bad = true; out.rain = false; out.snow = true; return true; }
    if (s == "bad" || s == "overcast" || s == "cloud") { out.bad = true; out.rain = out.snow = false; return true; }
    if (s == "fog" || s == "tunnel") { out.bad = true; out.rain = out.snow = false; return true; }
    return false;
}

} // namespace ht2

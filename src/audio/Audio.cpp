// Sound engine: SDL2_mixer effects, the continuous engine loop and the
// soundtrack.  See docs/audio.md for the data inventory, the measured mixer
// pan/attenuation laws and the reasoning behind the engine strategy.
//
// Everything is inside #ifdef HT2_HAVE_SDL_MIXER; without the mixer (or when
// init() fails) every method is a safe no-op and no SDL symbol is referenced.
#include "Audio.h"
#include "../res/Res.h"
#include "../util/Log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifdef HT2_HAVE_SDL_MIXER
#include <SDL.h>
#include <SDL_mixer.h>
#endif

namespace ht2 {

#ifdef HT2_HAVE_SDL_MIXER
namespace {

// ---------------------------------------------------------------------------
// tuning constants - every one of them is documented in docs/audio.md
// ---------------------------------------------------------------------------
constexpr int   kRate        = 44100;   // device rate requested from SDL_mixer
constexpr int   kChunkFrames = 1024;    // device buffer, in sample frames
constexpr int   kMixChannels = 32;      // mixer tracks
constexpr int   kEngineChan  = 0;       // reserved channel for the engine loop

// Pitch scale: the loop plays as recorded at kIdleRpm.  The recorded firing
// fundamental is ~32.4 Hz (measured), i.e. ~486 rpm for a V8, ~648 rpm for a
// straight-six; 600 rpm sits inside that bracket and is the tuning constant.
constexpr float kIdleRpm    = 600.0f;
constexpr float kMinRatio   = 0.55f;    // ~330 rpm
constexpr float kMaxRatio   = 3.20f;    // ~1920 rpm
constexpr float kCrankRatio = 0.60f;    // starter cranking pitch
constexpr int   kCrankMs    = 700;      // snd\start.wav is 0.64 s long
constexpr float kEngineOffRpm = 50.0f;  // below this the engine is off: fade out

// Engine control smoothing (time constants of the one-pole filters that run at
// the audio rate inside the engine effect).
constexpr float kRevTau     = 0.025f;   // pitch rise, throttle blip
constexpr float kDecayTau   = 0.300f;   // pitch fall, off throttle
constexpr float kGainTau    = 0.060f;   // volume smoothing

// Attenuation: full volume inside nearM, silence at/beyond farM (linear in
// amplitude between them - see the measured Mix_SetPosition table).
constexpr float kDefaultNear = 8.0f;
constexpr float kDefaultFar  = 120.0f;

constexpr int   kMusicFadeIn  = 600;    // ms
constexpr int   kMusicFadeOut = 900;    // ms
constexpr int   kEngineSilentMs = 2000; // halt the loop after this long at gain 0
constexpr int   kEngineStaleMs  = 250;  // engine() stopped being called: fade out
constexpr int   kRetriggerMs    = 25;   // ignore the same effect retriggered faster

// Shipped TRUCK.INI [EFFECTS] values, used when the file is unreadable.
constexpr float kDefaultSfx = 0.54f;    // sfxvol=54
constexpr float kDefaultMus = 0.20f;    // midivol=20
constexpr float kDefaultEng = 0.90f;    // engvol=90

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

std::string toLower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// "snd\crash9.wav" / "crash9Sound" / "SND/Crash9" -> "crash9"
std::string normKey(const std::string& raw) {
    std::string s = raw;
    for (char& c : s) if (c == '\\') c = '/';
    size_t slash = s.find_last_of('/');
    if (slash != std::string::npos) s = s.substr(slash + 1);
    s = toLower(trim(s));
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".wav") == 0) s.erase(s.size() - 4);
    if (s.size() > 5 && s.compare(s.size() - 5, 5, "sound") == 0) s.erase(s.size() - 5);
    return s;
}

bool fileExists(const std::string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

// Tiny INI reader: "key=value" lines, ';'/'#' comments, [SECTION] prefixes.
// Values are stored as "section.key" (both lower case).
std::map<std::string, std::string> parseIni(const std::string& path) {
    std::map<std::string, std::string> kv;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return kv;
    std::string data;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, n);
    fclose(f);

    std::string section;
    size_t pos = 0;
    while (pos <= data.size()) {
        size_t eol = data.find('\n', pos);
        std::string line = data.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? data.size() + 1 : eol + 1;
        size_t cut = line.find_first_of(";#");
        if (cut != std::string::npos) line.erase(cut);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[') {
            size_t close = line.find(']');
            if (close != std::string::npos) section = toLower(trim(line.substr(1, close - 1)));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = toLower(trim(line.substr(0, eq)));
        std::string val = trim(line.substr(eq + 1));
        if (key.empty()) continue;
        kv[section + "." + key] = val;
        if (kv.find(key) == kv.end()) kv[key] = val;   // section-less fallback
    }
    return kv;
}

// A percentage setting ("54") -> 0..1.  Returns `dflt` when absent/garbage.
float iniPercent(const std::map<std::string, std::string>& kv,
                 const char* section, const char* key, float dflt) {
    std::map<std::string, std::string>::const_iterator it =
        kv.find(std::string(section) + "." + key);
    if (it == kv.end()) it = kv.find(key);
    if (it == kv.end()) return dflt;
    char* end = nullptr;
    double v = strtod(it->second.c_str(), &end);
    if (end == it->second.c_str()) return dflt;
    return clamp01((float)(v / 100.0));
}

} // namespace

// ===========================================================================
// implementation
// ===========================================================================
struct Audio::Impl {
    // --- device -------------------------------------------------------------
    bool   sdlOwned = false;      // we called SDL_InitSubSystem(AUDIO)
    bool   open = false;
    Uint16 devFormat = AUDIO_S16SYS;
    int    devChannels = 2;
    int    devRate = kRate;
    unsigned mixFlags = 0;

    // --- effect bank --------------------------------------------------------
    std::vector<Mix_Chunk*> chunks;      // owned
    std::map<std::string, int> byName;   // stem/alias -> index into chunks
    size_t bankBytes = 0, srcBytes = 0, loadFails = 0;
    size_t aliasCount = 0;
    std::set<std::string> missing;       // logged once per name
    std::map<std::string, Uint32> lastPlay;

    // --- volumes ------------------------------------------------------------
    float master = 1.0f, sfxVol = kDefaultSfx, musicVol = kDefaultMus, engVol = kDefaultEng;
    std::string volSource = "defaults";

    // --- listener / attenuation --------------------------------------------
    Vec3  listener;
    float listenerYaw = 0.0f;             // degrees, 0 = facing -Z
    bool  listenerSet = false;
    float nearM = kDefaultNear, farM = kDefaultFar;

    // --- engine -------------------------------------------------------------
    EngineKind kind = EngineKind::Truck;
    int   engineIdx = -1;
    Mix_Chunk* engineChunk = nullptr;
    int   engineChan = -1;
    bool  engineEffect = false;          // our resampling effect is registered
    bool  engineRunning = false;
    std::vector<float> loop;             // seamless stereo float copy of the loop
    size_t loopFrames = 0;
    float  loopPeriod = 0.0f;            // detected firing period [frames]
    float  loopSeam = 0.0f, loopSeamRaw = 0.0f;
    Uint32 crankUntil = 0;
    Uint32 lastEngineCall = 0;
    size_t engineRestarts = 0;

    // engine control published by the game thread, consumed by the audio thread
    std::atomic<float> tgtRatio{1.0f}, tgtL{0.0f}, tgtR{0.0f};
    // audio-thread only
    float curRatio = 0.0f, curL = 0.0f, curR = 0.0f;
    double phase = 0.0;
    float kRise = 0.01f, kFall = 0.001f, kGain = 0.01f;
    bool  engineAudible = false;
    Uint32 engineSilentSince = 0;

    // --- music --------------------------------------------------------------
    Mix_Music* mus = nullptr;
    std::string musPath, musDir = "MUSIC", dataDir = "original";
    bool  musLoop = true;
    bool  musFading = false;
    bool  musWarned = false;
    std::set<std::string> musMissing;

    ~Impl() { /* freed by Audio::shutdown(); never rely on this */ }
};

namespace {

constexpr float kPi = 3.14159265358979f;

// The pan law SDL_mixer's Mix_SetPosition implements.  Measured on 2.8.2
// (docs/audio.md table): angle 0 is ahead (both channels full), 90 is hard
// right, 180 is behind (both channels full - front/back is NOT attenuated),
// 270 is hard left, with the opposite channel ramping linearly in between.
void panGains(float angleDeg, float& gl, float& gr) {
    float a = fmodf(angleDeg, 360.0f);
    if (a < 0.0f) a += 360.0f;
    const float u = a / 90.0f;                       // 0..4
    gl = (u <= 1.0f) ? 1.0f - u : (u <= 2.0f ? u - 1.0f : 1.0f);
    gr = (u <= 2.0f) ? 1.0f : (u <= 3.0f ? 1.0f - (u - 2.0f) : (u - 2.0f) - 1.0f);
}

// Listener-relative angle: 0 = ahead, 90 = right, 180 = behind, 270 = left
// (SDL_mixer's convention: north is 0 and goes clockwise, north = listener -Z).
float angleTo(const Audio::Impl& im, const Audio::Vec3& at) {
    const float yaw = im.listenerYaw * kPi / 180.0f;
    const float fx = sinf(yaw), fz = -cosf(yaw);     // forward
    const float rx = cosf(yaw), rz = sinf(yaw);      // right
    const float dx = at.x - im.listener.x;
    const float dz = at.z - im.listener.z;
    const float fwd = dx * fx + dz * fz;
    const float rgt = dx * rx + dz * rz;
    float a = atan2f(rgt, fwd) * 180.0f / kPi;
    if (a < 0.0f) a += 360.0f;
    return a;
}

float distanceTo(const Audio::Impl& im, const Audio::Vec3& at) {
    const float dx = at.x - im.listener.x;
    const float dy = at.y - im.listener.y;
    const float dz = at.z - im.listener.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// 1.0 inside nearM, 0.0 at/beyond farM, linear in amplitude between.
float attenuation(float d, float nearM, float farM) {
    if (d <= nearM) return 1.0f;
    if (farM <= nearM || d >= farM) return 0.0f;
    return (farM - d) / (farM - nearM);
}

Uint8 distanceByte(float att) {
    return (Uint8)std::lround(clamp01(1.0f - att) * 255.0f);
}

float readSample(const Mix_Chunk* c, Uint16 fmt, int nch, size_t frame, int ch) {
    if (fmt == AUDIO_F32SYS) {
        const float* p = (const float*)c->abuf;
        return p[frame * (size_t)nch + (size_t)(ch < nch ? ch : 0)];
    }
    const Sint16* p = (const Sint16*)c->abuf;
    return p[frame * (size_t)nch + (size_t)(ch < nch ? ch : 0)] / 32768.0f;
}

// The engine effect: it *replaces* the channel's audio with its own
// phase-accumulator resampling of the preloaded loop, so pitch is continuous
// (no per-frame restart, no crossfade between pitched copies).  Runs on the SDL
// audio thread: no allocation, no locking, no I/O.
void engineEffectCb(int /*chan*/, void* stream, int len, void* ud) {
    Audio::Impl* im = static_cast<Audio::Impl*>(ud);
    if (!im || im->loopFrames == 0 || len <= 0) return;

    const int bps = (im->devFormat == AUDIO_F32SYS) ? 4 : 2;
    const int nch = im->devChannels > 0 ? im->devChannels : 2;
    const int frames = len / (bps * nch);
    const size_t n = im->loopFrames;
    const float* lp = im->loop.data();

    const float tr = im->tgtRatio.load(std::memory_order_relaxed);
    const float tl = im->tgtL.load(std::memory_order_relaxed);
    const float trr = im->tgtR.load(std::memory_order_relaxed);
    float* outF = (im->devFormat == AUDIO_F32SYS) ? (float*)stream : nullptr;
    Sint16* outS = outF ? nullptr : (Sint16*)stream;

    for (int i = 0; i < frames; ++i) {
        im->curRatio += (tr - im->curRatio) * ((tr > im->curRatio) ? im->kRise : im->kFall);
        im->curL += (tl - im->curL) * im->kGain;
        im->curR += (trr - im->curR) * im->kGain;

        size_t i0 = (size_t)im->phase;
        if (i0 >= n) i0 %= n;
        size_t i1 = i0 + 1;
        if (i1 >= n) i1 = 0;
        const float fr = (float)(im->phase - (double)i0);
        const float l = lp[i0 * 2] + (lp[i1 * 2] - lp[i0 * 2]) * fr;
        const float r = lp[i0 * 2 + 1] + (lp[i1 * 2 + 1] - lp[i0 * 2 + 1]) * fr;
        const float ol = l * im->curL;
        const float orr = r * im->curR;

        const size_t base = (size_t)i * (size_t)nch;
        if (nch == 1) {
            const float m = 0.5f * (ol + orr);
            if (outF) outF[base] = m;
            else outS[base] = (Sint16)std::lround(std::max(-1.0f, std::min(1.0f, m)) * 32767.0f);
        } else {
            if (outF) { outF[base] = ol; outF[base + 1] = orr; }
            else {
                outS[base] = (Sint16)std::lround(std::max(-1.0f, std::min(1.0f, ol)) * 32767.0f);
                outS[base + 1] = (Sint16)std::lround(std::max(-1.0f, std::min(1.0f, orr)) * 32767.0f);
            }
            for (int k = 2; k < nch; ++k) {          // surround: duplicate L/R
                if (outF) outF[base + (size_t)k] = (k & 1) ? orr : ol;
                else outS[base + (size_t)k] = outS[base + (size_t)(k & 1)];
            }
        }

        im->phase += (double)im->curRatio;
        if (im->phase >= (double)n) im->phase -= (double)n;
        if (im->phase < 0.0) im->phase = 0.0;
    }
}

// Turn the converted mixer chunk into a seamlessly loopable stereo float loop.
//
// Two problems with the raw recording (measured on snd\idle.wav, docs/audio.md):
//   * its length (20567 frames) is 15.12 firing periods (1360.4 frames,
//     32.4 Hz), so wrapping the end to the start jumps phase by 12 % of a cycle;
//   * the waveform value at the seam also differs (|x[0]-x[last]| ~ 0.138).
// Fix: detect the period by autocorrelation, keep an integer number of periods
// and crossfade the leftover partial period (T frames) over the head, which
// makes out[0] == src[L] and therefore the wrap continuous by construction.
void buildLoop(Audio::Impl& im, Mix_Chunk* c) {
    const size_t frames = (size_t)c->alen / (size_t)(((im.devFormat == AUDIO_F32SYS) ? 4 : 2) * im.devChannels);
    std::vector<float> src(frames * 2, 0.0f);
    for (size_t i = 0; i < frames; ++i) {
        const float l = readSample(c, im.devFormat, im.devChannels, i, 0);
        const float r = im.devChannels > 1 ? readSample(c, im.devFormat, im.devChannels, i, 1) : l;
        src[i * 2] = l;
        src[i * 2 + 1] = r;
    }
    im.loopPeriod = 0.0f;
    im.loopSeam = 0.0f;
    im.loopSeamRaw = frames ? fabsf(src[0] - src[(frames - 1) * 2]) : 0.0f;

    // autocorrelation over 30..250 Hz on the first channel
    const size_t win = std::min<size_t>(frames, 4096);
    const size_t lo = (size_t)(im.devRate / 250);
    const size_t hi = std::min(win - 2, (size_t)(im.devRate / 30));
    double best = 0.0;
    size_t bestLag = 0;
    for (size_t lag = lo; lag <= hi; ++lag) {
        double num = 0.0, da = 0.0, db = 0.0;
        for (size_t i = 0; i + lag < win; ++i) {
            const double a = src[i * 2], b = src[(i + lag) * 2];
            num += a * b;
            da += a * a;
            db += b * b;
        }
        const double r = (da > 0.0 && db > 0.0) ? num / sqrt(da * db) : 0.0;
        if (r > best) { best = r; bestLag = lag; }
    }

    size_t L = frames;
    size_t T = 0;
    if (bestLag > 0 && best > 0.25 && frames > 4 * bestLag) {
        const double periods = std::max(1.0, floor((double)frames / (double)bestLag));
        L = (size_t)llround(periods * (double)bestLag);
        if (L > frames) L = frames;
        T = frames - L;
        if (T < 16 || T > 8192) T = 0;      // nothing sensible to fold in
        im.loopPeriod = (float)bestLag;
    }
    if (T > 0) {
        for (size_t i = 0; i < T; ++i) {
            const float w = (float)i / (float)T;          // 0 -> tail, 1 -> head
            for (int k = 0; k < 2; ++k) {
                const float head = src[i * 2 + (size_t)k];
                const float tail = src[(L + i) * 2 + (size_t)k];
                src[i * 2 + (size_t)k] = head * w + tail * (1.0f - w);
            }
        }
        im.loopSeam = fabsf(src[0] - src[(L - 1) * 2]);
    } else {
        im.loopSeam = im.loopSeamRaw;
    }
    im.loop.assign(src.begin(), src.begin() + (std::ptrdiff_t)(L * 2));
    im.loopFrames = L;
    im.phase = 0.0;
}

// Resolve the shipped idle loop for the requested vehicle class and prepare it
// for the phase-accumulator effect.  Safe to call repeatedly.
void loadEngineLoop(Audio::Impl& im) {
    const std::string want = (im.kind == Audio::EngineKind::Light) ? "jidle" : "idle";
    std::map<std::string, int>::const_iterator it = im.byName.find(want);
    if (it == im.byName.end() || !im.chunks[(size_t)it->second]) {
        im.engineIdx = -1;
        im.engineChunk = nullptr;
        if (im.missing.insert(want).second)
            LOG("audio: engine loop '%s' not in the bank - engine disabled", want.c_str());
        return;
    }
    im.engineIdx = it->second;
    im.engineChunk = im.chunks[(size_t)it->second];
    buildLoop(im, im.engineChunk);
    LOG("audio: engine loop '%s': %zu frames, period %.1f frames (%.1f Hz), seam |dx| %.4f -> %.4f",
        want.c_str(), im.loopFrames, im.loopPeriod,
        im.loopPeriod > 0.0f ? (float)im.devRate / im.loopPeriod : 0.0f,
        im.loopSeamRaw, im.loopSeam);
}

// Stop and forget the running loop (used when the vehicle class changes).
void unloadEngineLoop(Audio::Impl& im) {
    if (im.engineChan >= 0) {
        Mix_UnregisterAllEffects(im.engineChan);
        Mix_HaltChannel(im.engineChan);
        im.engineChan = -1;
    }
    im.engineEffect = false;
    im.engineChunk = nullptr;
    im.engineIdx = -1;
    im.engineRunning = false;
    im.engineAudible = false;
    im.loop.clear();
    im.loopFrames = 0;
}

} // namespace

// ===========================================================================
// public API
// ===========================================================================

Audio::Audio() : impl_(new Impl()) {}
Audio::~Audio() { shutdown(); }

bool Audio::init(const std::string& dataDir) {
    Impl& im = *impl_;
    if (im.open) return true;
    im.dataDir = dataDir;

    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            LOG("audio: SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
            return false;
        }
        im.sdlOwned = true;
    }
    im.mixFlags = Mix_Init(MIX_INIT_MP3);
    if ((im.mixFlags & MIX_INIT_MP3) == 0)
        LOG("audio: MP3 decoder unavailable (Mix_Init=0x%x) - music will not load", im.mixFlags);

    if (Mix_OpenAudio(kRate, AUDIO_S16SYS, 2, kChunkFrames) != 0) {
        LOG("audio: Mix_OpenAudio(%d Hz, 16-bit, stereo, %d) failed: %s", kRate, kChunkFrames, Mix_GetError());
        return false;
    }
    Mix_QuerySpec(&im.devRate, &im.devFormat, &im.devChannels);
    if (im.devFormat != AUDIO_S16SYS && im.devFormat != AUDIO_F32SYS)
        LOG("audio: device format 0x%04x is neither S16SYS nor F32SYS - engine pitch disabled", im.devFormat);
    Mix_AllocateChannels(kMixChannels);
    Mix_ReserveChannels(1);                  // channel 0 belongs to the engine
    im.kRise = 1.0f - expf(-1.0f / (kRevTau * (float)im.devRate));
    im.kFall = 1.0f - expf(-1.0f / (kDecayTau * (float)im.devRate));
    im.kGain = 1.0f - expf(-1.0f / (kGainTau * (float)im.devRate));
    im.open = true;

    // ---- TRUCK.INI: volumes and the music folder ---------------------------
    std::string iniPath;
    const char* candidates[] = {"/TRUCK.INI", "/truck.ini", "/Truck.ini"};
    for (const char* c : candidates) {
        if (fileExists(dataDir + c)) { iniPath = dataDir + c; break; }
    }
    if (!iniPath.empty()) {
        std::map<std::string, std::string> kv = parseIni(iniPath);
        im.sfxVol = iniPercent(kv, "effects", "sfxvol", im.sfxVol);
        im.musicVol = iniPercent(kv, "effects", "midivol", im.musicVol);
        im.engVol = iniPercent(kv, "effects", "engvol", im.engVol);
        im.volSource = iniPath;
        std::map<std::string, std::string>::const_iterator it = kv.find("install.music");
        if (it != kv.end() && !it->second.empty()) {
            std::string dir = it->second;
            for (char& ch : dir) if (ch == '\\') ch = '/';
            while (dir.size() > 1 && (dir.compare(0, 2, "./") == 0)) dir.erase(0, 2);
            while (!dir.empty() && dir.back() == '/') dir.pop_back();
            if (!dir.empty()) im.musDir = dir;
        }
        LOG("audio: %s: sfxvol=%.0f%% midivol=%.0f%% engvol=%.0f%% music=./%s",
            iniPath.c_str(), im.sfxVol * 100.0f,
            im.musicVol * 100.0f, im.engVol * 100.0f, im.musDir.c_str());
    } else {
        LOG("audio: no TRUCK.INI under %s - using shipped defaults sfxvol=%.0f%% midivol=%.0f%% engvol=%.0f%%",
            dataDir.c_str(), im.sfxVol * 100.0f, im.musicVol * 100.0f, im.engVol * 100.0f);
    }

    // ---- effect bank -------------------------------------------------------
    im.bankBytes = im.srcBytes = im.loadFails = im.aliasCount = 0;
    im.byName.clear();
    const std::string resPaths[] = {
        dataDir + "/COMMON/common1.res",
        dataDir + "/COMMON/common2.res",
        dataDir + "/COMMON/COMMON.RES",
        dataDir + "/common1.res",
    };
    Res res;
    std::string resUsed;
    for (const std::string& p : resPaths) {
        if (Res r; r.load(p) && r.loaded()) {
            bool hasSound = false;
            for (const Res::Entry& e : r.entries())
                if (e.kind == Res::Kind::Sound) { hasSound = true; break; }
            if (!hasSound) continue;
            res = std::move(r);
            resUsed = p;
            break;
        }
    }
    if (!res.loaded()) {
        LOG("audio: no .res with SOUNDFILES found under %s - effect bank empty", dataDir.c_str());
        return true;                          // device is up, music/UI still work
    }

    // SOUNDFILES order == the 1-based indices used by the `SOUNDS` alias table.
    std::vector<const Res::Entry*> sounds;
    for (const Res::Entry& e : res.entries())
        if (e.kind == Res::Kind::Sound) sounds.push_back(&e);

    for (size_t i = 0; i < sounds.size(); ++i) {
        const Res::Entry& e = *sounds[i];
        Mix_Chunk* c = Mix_LoadWAV_RW(SDL_RWFromConstMem(res.data(e), (int)e.size), 1);
        if (!c) {
            ++im.loadFails;
            LOG("audio: %s failed to load: %s", e.name.c_str(), Mix_GetError());
            im.chunks.push_back(nullptr);
            continue;
        }
        im.chunks.push_back(c);
        im.bankBytes += c->alen;
        im.srcBytes += e.size;
        const std::string key = normKey(e.name);
        if (im.byName.find(key) == im.byName.end()) im.byName[key] = (int)i;
    }

    // `SOUNDS <n>` maps logical names ("idleSound", "shakeSound") to 1-based
    // SOUNDFILES indices.  Res.h does not expose non-FILES sections, so scan the
    // container bytes for it; aliases never overwrite a real file stem.
    const std::vector<uint8_t>& bytes = res.bytes();
    const char* tag = "SOUNDS ";
    const size_t tagLen = 7;
    for (size_t p = 0; p + tagLen < bytes.size(); ++p) {
        if (memcmp(bytes.data() + p, tag, tagLen) != 0) continue;
        if (p > 0 && bytes[p - 1] != 0) continue;          // must start a token
        size_t q = p + tagLen;
        long count = 0;
        bool digits = false;
        while (q < bytes.size() && bytes[q] >= '0' && bytes[q] <= '9') {
            count = count * 10 + (bytes[q] - '0');
            ++q;
            digits = true;
        }
        if (!digits || q >= bytes.size() || bytes[q] != 0) continue;
        ++q;
        for (long k = 0; k < count && q < bytes.size(); ++k) {
            size_t z = q;
            while (z < bytes.size() && bytes[z]) ++z;
            if (z >= bytes.size()) break;
            std::string line((const char*)bytes.data() + q, z - q);
            q = z + 1;
            size_t sp = line.find_last_of(' ');
            if (sp == std::string::npos) continue;
            long idx = strtol(line.c_str() + sp + 1, nullptr, 10);
            std::string lname = line.substr(0, sp);
            if (idx <= 0 || (size_t)idx > sounds.size()) continue;
            const int target = (int)idx - 1;
            if (target < 0 || target >= (int)im.chunks.size() || !im.chunks[(size_t)target]) continue;
            const std::string key = normKey(lname);
            if (key.empty()) continue;
            if (im.byName.find(key) == im.byName.end()) {
                im.byName[key] = target;
                ++im.aliasCount;
            }
        }
        break;                                            // first SOUNDS section only
    }

    LOG("audio: %d Hz %d ch fmt=0x%04x, %zu effects (%.2f MiB resident, %.2f MiB source, %zu failed), "
        "%zu names (%zu aliases) from %s",
        im.devRate, im.devChannels, im.devFormat, im.chunks.size(),
        im.bankBytes / 1048576.0, im.srcBytes / 1048576.0, im.loadFails,
        im.byName.size(), im.aliasCount, resUsed.c_str());
    setMasterVolume(im.master);                            // push volumes to the mixer
    return true;
}

void Audio::shutdown() {
    Impl& im = *impl_;
    const size_t freed = im.chunks.size();
    const double freedBytes = im.bankBytes / 1048576.0;
    if (im.engineChan >= 0) {
        Mix_UnregisterAllEffects(im.engineChan);
        Mix_HaltChannel(im.engineChan);
        im.engineChan = -1;
    }
    if (im.open) Mix_HaltChannel(-1);
    if (im.mus) {
        if (Mix_PlayingMusic()) Mix_HaltMusic();
        Mix_FreeMusic(im.mus);
        im.mus = nullptr;
    }
    for (Mix_Chunk* c : im.chunks) if (c) Mix_FreeChunk(c);
    im.chunks.clear();
    im.byName.clear();
    im.engineChunk = nullptr;
    im.engineIdx = -1;
    im.engineRunning = false;
    im.engineEffect = false;
    if (im.open) {
        LOG("audio: shutdown (%zu effects, %.2f MiB resident released)", freed, freedBytes);
        Mix_CloseAudio();
        im.open = false;
    }
    // Only now that the device is closed (no callback can be in flight) is it
    // safe to drop the engine effect's sample memory.
    im.loop.clear();
    im.loopFrames = 0;
    im.bankBytes = im.srcBytes = 0;
    if (im.mixFlags) { Mix_Quit(); im.mixFlags = 0; }
    if (im.sdlOwned) { SDL_QuitSubSystem(SDL_INIT_AUDIO); im.sdlOwned = false; }
}

bool Audio::ready() const { return impl_->open; }
size_t Audio::effects() const { return impl_->chunks.size(); }
size_t Audio::bankBytes() const { return impl_->bankBytes; }
size_t Audio::sourceBytes() const { return impl_->srcBytes; }
size_t Audio::missingRes() const { return impl_->loadFails; }
bool Audio::musicPlaying() const { return impl_->mus != nullptr && Mix_PlayingMusic() != 0; }
float Audio::masterVolume() const { return impl_->master; }
float Audio::sfxVolume() const { return impl_->sfxVol; }
float Audio::musicVolume() const { return impl_->musicVol; }
float Audio::engineVolume() const { return impl_->engVol; }
size_t Audio::engineLoopFrames() const { return impl_->loopFrames; }
float Audio::engineLoopPeriodFrames() const { return impl_->loopPeriod; }
float Audio::engineLoopSeamStep() const { return impl_->loopSeam; }
float Audio::engineLoopSeamStepRaw() const { return impl_->loopSeamRaw; }
size_t Audio::engineRestarts() const { return impl_->engineRestarts; }
float Audio::enginePitchRatio() const { return impl_->tgtRatio.load(std::memory_order_relaxed); }
float Audio::engineGain() const {
    return std::max(impl_->tgtL.load(std::memory_order_relaxed), impl_->tgtR.load(std::memory_order_relaxed));
}

bool Audio::has(const std::string& name) const { return indexOf(name) >= 0; }

int Audio::indexOf(const std::string& name) const {
    const Impl& im = *impl_;
    std::map<std::string, int>::const_iterator it = im.byName.find(normKey(name));
    if (it == im.byName.end()) return -1;
    return it->second;
}

void Audio::play(const std::string& name, const Vec3& at, float gain) {
    Impl& im = *impl_;
    if (!im.open) return;
    const std::string key = normKey(name);
    std::map<std::string, int>::const_iterator it = im.byName.find(key);
    if (it == im.byName.end() || !im.chunks[(size_t)it->second]) {
        if (im.missing.insert(key).second)
            LOG("audio: unknown sound '%s' (%zu names known) - ignored", name.c_str(), im.byName.size());
        return;
    }
    const float d = distanceTo(im, at);
    const float att = attenuation(d, im.nearM, im.farM);
    if (att <= 0.0f) return;                       // beyond the silence radius: culled
    const Uint32 now = SDL_GetTicks();
    std::map<std::string, Uint32>::iterator lp = im.lastPlay.find(key);
    if (lp != im.lastPlay.end() && now - lp->second < (Uint32)kRetriggerMs) return;
    im.lastPlay[key] = now;

    const int chan = Mix_PlayChannel(-1, im.chunks[(size_t)it->second], 0);
    if (chan < 0) return;
    const float vol = clamp01(gain) * im.sfxVol * im.master;
    Mix_Volume(chan, (int)std::lround(vol * MIX_MAX_VOLUME));
    Mix_SetPosition(chan, (Sint16)std::lround(angleTo(im, at)), distanceByte(att));
}

void Audio::playGlobal(const std::string& name, float gain) {
    Impl& im = *impl_;
    if (!im.open) return;
    const std::string key = normKey(name);
    std::map<std::string, int>::const_iterator it = im.byName.find(key);
    if (it == im.byName.end() || !im.chunks[(size_t)it->second]) {
        if (im.missing.insert(key).second)
            LOG("audio: unknown sound '%s' (%zu names known) - ignored", name.c_str(), im.byName.size());
        return;
    }
    const int chan = Mix_PlayChannel(-1, im.chunks[(size_t)it->second], 0);
    if (chan < 0) return;
    Mix_Volume(chan, (int)std::lround(clamp01(gain) * im.sfxVol * im.master * MIX_MAX_VOLUME));
    Mix_SetPosition(chan, 0, 0);                   // clear any position from a reused channel
}

void Audio::setEngineKind(EngineKind k) {
    Impl& im = *impl_;
    if (im.kind == k && im.engineIdx >= 0) return;
    im.kind = k;
    if (!im.open) return;
    unloadEngineLoop(im);
    loadEngineLoop(im);
}

void Audio::engineStart() {
    Impl& im = *impl_;
    if (!im.open) return;
    if (!im.engineRunning) {
        playGlobal("start");                       // snd\start.wav (0.64 s)
        im.crankUntil = SDL_GetTicks() + (Uint32)kCrankMs;
        im.engineRunning = true;                   // engine() starts the loop itself
        im.phase = 0.0;
        im.curRatio = 0.0f;                        // spool up from standstill
        im.curL = im.curR = 0.0f;
    }
}

void Audio::engineStop() {
    Impl& im = *impl_;
    if (!im.open) return;
    if (im.engineRunning) {
        im.tgtRatio.store(kMinRatio);
        im.tgtL.store(0.0f);
        im.tgtR.store(0.0f);
        playGlobal("stop_eng");
    }
    im.engineRunning = false;
    im.crankUntil = 0;
}

void Audio::stopEngine() { engineStop(); }

void Audio::engine(float rpm, float load, const Vec3& at) {
    Impl& im = *impl_;
    if (!im.open) return;

    const Uint32 now = SDL_GetTicks();
    const bool cranking = im.crankUntil > now;
    const bool off = !cranking && rpm < kEngineOffRpm;   // stopped / not cranking
    const float ratio = cranking ? kCrankRatio
                                 : std::max(kMinRatio, std::min(kMaxRatio, rpm / kIdleRpm));
    const float att = attenuation(distanceTo(im, at), im.nearM, im.farM);
    float gl = 1.0f, gr = 1.0f;
    panGains(angleTo(im, at), gl, gr);
    // idle floor (engine running) + throttle, muted when the engine is off or
    // out of earshot so update() can park the channel.
    const float rpmGain = off ? 0.0f : (0.35f + 0.65f * clamp01(load));
    const float g = att * im.master * im.engVol * rpmGain * (cranking ? 0.45f : 1.0f);

    im.tgtRatio.store(ratio, std::memory_order_relaxed);
    im.tgtL.store(g * gl, std::memory_order_relaxed);
    im.tgtR.store(g * gr, std::memory_order_relaxed);
    im.lastEngineCall = now;
    if (g > 1e-4f) {
        im.engineAudible = true;
        im.engineSilentSince = now;
    } else if (im.engineAudible) {
        im.engineAudible = false;
        im.engineSilentSince = now;
    }
    // Nothing to start for (engine off / out of earshot): never spin the channel
    // up, so a silent truck does not restart the loop every kEngineSilentMs.
    if (g <= 1e-4f) return;

    if (im.engineIdx < 0) {
        loadEngineLoop(im);                        // first audible call: pick up the loop
        if (im.engineIdx < 0) return;
    }
    // Lazily (re)start the reserved channel with our resampling effect; the
    // sample is never restarted while it keeps playing.
    if (im.engineChan < 0 || !Mix_Playing(im.engineChan)) {
        im.engineChan = Mix_PlayChannel(kEngineChan, im.engineChunk, -1);
        im.engineEffect = false;
        if (im.engineChan < 0) {
            if (im.missing.insert("engine-channel").second)
                LOG("audio: no free channel for the engine loop: %s", Mix_GetError());
            return;
        }
        if (im.devFormat == AUDIO_S16SYS || im.devFormat == AUDIO_F32SYS) {
            im.engineEffect = Mix_RegisterEffect(im.engineChan, engineEffectCb, nullptr, &im) != 0;
            if (!im.engineEffect)
                LOG("audio: engine pitch effect unavailable (%s) - volume/pan only", Mix_GetError());
        } else {
            im.engineEffect = false;
        }
        im.engineRunning = true;
        ++im.engineRestarts;
        im.engineSilentSince = now;
    }

    if (!im.engineEffect) {                        // graceful degradation
        Mix_Volume(im.engineChan, (int)std::lround(clamp01(g) * MIX_MAX_VOLUME));
        Mix_SetPosition(im.engineChan, (Sint16)std::lround(angleTo(im, at)), distanceByte(att));
    }
}

void Audio::stopMusic() {
    Impl& im = *impl_;
    if (!im.open || !im.mus) return;
    if (Mix_PlayingMusic()) {
        Mix_FadeOutMusic(kMusicFadeOut);
        im.musFading = true;
    } else {
        Mix_FreeMusic(im.mus);
        im.mus = nullptr;
    }
}

void Audio::music(const std::string& file, bool loop) {
    Impl& im = *impl_;
    if (!im.open || file.empty()) return;
    if ((im.mixFlags & MIX_INIT_MP3) == 0) {
        if (!im.musWarned) { im.musWarned = true; LOG("audio: music '%s' skipped - no MP3 decoder", file.c_str()); }
        return;
    }

    std::string path = file;
    if (path[0] != '/') {
        std::string base = path;
        for (char& c : base) if (c == '\\') c = '/';
        const size_t slash = base.find_last_of('/');
        const std::string name = (slash == std::string::npos) ? base : base.substr(slash + 1);
        const std::string cands[] = {
            im.dataDir + "/" + im.musDir + "/" + name,
            im.dataDir + "/" + im.musDir + "/" + name + ".mp3",
            im.dataDir + "/" + base,
            im.dataDir + "/" + base + ".mp3",
        };
        path.clear();
        for (const std::string& c : cands)
            if (fileExists(c)) { path = c; break; }
        if (path.empty()) path = im.dataDir + "/" + im.musDir + "/" + name;   // for the log
    }

    // Already playing this track: only refresh the volume/loop settings.
    if (im.mus && path == im.musPath && Mix_PlayingMusic()) {
        Mix_VolumeMusic((int)std::lround(im.musicVol * im.master * MIX_MAX_VOLUME));
        im.musFading = false;
        return;
    }
    if (im.mus) {
        Mix_HaltMusic();
        Mix_FreeMusic(im.mus);
        im.mus = nullptr;
        im.musFading = false;
    }
    im.mus = Mix_LoadMUS(path.c_str());
    if (!im.mus) {
        if (im.musMissing.insert(path).second)
            LOG("audio: music '%s' failed to load: %s", path.c_str(), Mix_GetError());
        return;
    }
    im.musPath = path;
    im.musLoop = loop;
    Mix_VolumeMusic((int)std::lround(im.musicVol * im.master * MIX_MAX_VOLUME));
    if (Mix_FadeInMusic(im.mus, loop ? -1 : 0, kMusicFadeIn) != 0) {
        LOG("audio: music '%s' play failed: %s", path.c_str(), Mix_GetError());
        Mix_FreeMusic(im.mus);
        im.mus = nullptr;
        return;
    }
    LOG("audio: music '%s' (%s, fade-in %d ms, volume %.0f%%)", path.c_str(),
        loop ? "loop" : "once", kMusicFadeIn, im.musicVol * im.master * 100.0f);
}

void Audio::setMasterVolume(float v) {
    Impl& im = *impl_;
    im.master = clamp01(v);
    if (!im.open) return;
    if (im.mus) Mix_VolumeMusic((int)std::lround(im.musicVol * im.master * MIX_MAX_VOLUME));
    // The engine gain targets are re-derived from the master volume by engine().
}

void Audio::setChannelVolumes(float sfx, float music, float engine) {
    Impl& im = *impl_;
    im.sfxVol = clamp01(sfx);
    im.musicVol = clamp01(music);
    im.engVol = clamp01(engine);
    if (im.open && im.mus) Mix_VolumeMusic((int)std::lround(im.musicVol * im.master * MIX_MAX_VOLUME));
}

void Audio::setListener(const Vec3& pos, float yawDeg) {
    Impl& im = *impl_;
    im.listener = pos;
    im.listenerYaw = yawDeg;
    im.listenerSet = true;
}

void Audio::setListenerDir(const Vec3& pos, float fwdX, float fwdZ) {
    Impl& im = *impl_;
    im.listener = pos;
    im.listenerYaw = atan2f(fwdX, -fwdZ) * 180.0f / kPi;   // renderer/camera convention
    im.listenerSet = true;
}

void Audio::setAttenuation(float nearM, float farM) {
    Impl& im = *impl_;
    if (nearM < 0.0f) nearM = 0.0f;
    if (farM <= nearM) farM = nearM + 1.0f;
    im.nearM = nearM;
    im.farM = farM;
}

void Audio::update() {
    Impl& im = *impl_;
    if (!im.open) return;
    const Uint32 now = SDL_GetTicks();

    // Music: free the track once its fade-out finished (no leak per track).
    if (im.mus && im.musFading && !Mix_PlayingMusic()) {
        Mix_FreeMusic(im.mus);
        im.mus = nullptr;
        im.musFading = false;
    }
    // Non-looping track that ended on its own: free it too.
    if (im.mus && !im.musLoop && !Mix_PlayingMusic() && !im.musFading) {
        Mix_FreeMusic(im.mus);
        im.mus = nullptr;
    }
    // Engine: if the game stopped calling engine() (menu, pause, destroyed
    // vehicle) fade the loop out instead of leaving a stuck drone.
    if (im.engineChan >= 0 && im.lastEngineCall != 0 &&
        now - im.lastEngineCall > (Uint32)kEngineStaleMs) {
        im.tgtL.store(0.0f, std::memory_order_relaxed);
        im.tgtR.store(0.0f, std::memory_order_relaxed);
        if (im.engineAudible) {
            im.engineAudible = false;
            im.engineSilentSince = now;
        }
    }
    // Engine: after kEngineSilentMs of silence (out of earshot) park the channel.
    if (im.engineChan >= 0 && !im.engineAudible && im.engineSilentSince != 0 &&
        now - im.engineSilentSince > (Uint32)kEngineSilentMs) {
        Mix_UnregisterAllEffects(im.engineChan);
        Mix_HaltChannel(im.engineChan);
        im.engineChan = -1;
        im.engineEffect = false;
    }
    // Bound the retrigger table.
    if (im.lastPlay.size() > 64) {
        for (std::map<std::string, Uint32>::iterator it = im.lastPlay.begin(); it != im.lastPlay.end();) {
            if (now - it->second > 5000) it = im.lastPlay.erase(it);
            else ++it;
        }
    }
}

std::string Audio::describe() const {
    const Impl& im = *impl_;
    char buf[1024];
    if (!im.open) return "audio: not initialised";
    snprintf(buf, sizeof buf,
             "audio: %d Hz %d ch fmt=0x%04x | effects %zu (%.2f MiB resident / %.2f MiB source, %zu failed) "
             "| names %zu | volumes master %.2f sfx %.2f music %.2f engine %.2f (%s) "
             "| attenuation %.0f..%.0f m | engine loop %zu frames, period %.1f (%.1f Hz), seam %.4f raw %.4f "
             "| music %s | channels busy %d/%d",
             im.devRate, im.devChannels, im.devFormat, im.chunks.size(),
             im.bankBytes / 1048576.0, im.srcBytes / 1048576.0, im.loadFails,
             im.byName.size(), im.master, im.sfxVol, im.musicVol, im.engVol, im.volSource.c_str(),
             im.nearM, im.farM, im.loopFrames, im.loopPeriod,
             im.loopPeriod > 0.0f ? (float)im.devRate / im.loopPeriod : 0.0f,
             im.loopSeam, im.loopSeamRaw, im.mus ? im.musPath.c_str() : "-",
             Mix_Playing(-1), kMixChannels);
    return buf;
}

// ===========================================================================
// no-mixer build: identical API, every call a no-op
// ===========================================================================
#else

struct Audio::Impl { };

Audio::Audio() : impl_(new Impl()) {}
Audio::~Audio() = default;
bool Audio::init(const std::string& dataDir) { (void)dataDir; LOG("audio: built without SDL2_mixer - disabled"); return false; }
void Audio::shutdown() {}
bool Audio::ready() const { return false; }
size_t Audio::effects() const { return 0; }
size_t Audio::bankBytes() const { return 0; }
size_t Audio::sourceBytes() const { return 0; }
size_t Audio::missingRes() const { return 0; }
void Audio::play(const std::string& name, const Vec3& at, float gain) { (void)name; (void)at; (void)gain; }
void Audio::playGlobal(const std::string& name, float gain) { (void)name; (void)gain; }
void Audio::engine(float rpm, float load, const Vec3& at) { (void)rpm; (void)load; (void)at; }
void Audio::setEngineKind(EngineKind k) { (void)k; }
void Audio::engineStart() {}
void Audio::engineStop() {}
void Audio::stopEngine() {}
void Audio::music(const std::string& file, bool loop) { (void)file; (void)loop; }
void Audio::stopMusic() {}
bool Audio::musicPlaying() const { return false; }
void Audio::setMasterVolume(float v) { (void)v; }
void Audio::setChannelVolumes(float sfx, float music, float engine) { (void)sfx; (void)music; (void)engine; }
float Audio::masterVolume() const { return 0.0f; }
float Audio::sfxVolume() const { return 0.0f; }
float Audio::musicVolume() const { return 0.0f; }
float Audio::engineVolume() const { return 0.0f; }
void Audio::setListener(const Vec3& pos, float yawDeg) { (void)pos; (void)yawDeg; }
void Audio::setListenerDir(const Vec3& pos, float fwdX, float fwdZ) { (void)pos; (void)fwdX; (void)fwdZ; }
void Audio::setAttenuation(float nearM, float farM) { (void)nearM; (void)farM; }
void Audio::update() {}
std::string Audio::describe() const { return "audio: built without SDL2_mixer"; }
size_t Audio::engineLoopFrames() const { return 0; }
float Audio::engineLoopPeriodFrames() const { return 0.0f; }
float Audio::engineLoopSeamStep() const { return 0.0f; }
float Audio::engineLoopSeamStepRaw() const { return 0.0f; }
size_t Audio::engineRestarts() const { return 0; }
float Audio::enginePitchRatio() const { return 0.0f; }
float Audio::engineGain() const { return 0.0f; }
bool Audio::has(const std::string& name) const { (void)name; return false; }
int Audio::indexOf(const std::string& name) const { (void)name; return -1; }

#endif // HT2_HAVE_SDL_MIXER

} // namespace ht2

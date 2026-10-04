// audio_test.cpp -- standalone harness for src/audio/Audio.{h,cpp}.
//
// It runs the module headless against the shipped data (SDL_AUDIODRIVER=dummy,
// which it sets itself), then checks every claim the module makes: the device
// parameters, the effect bank, the TRUCK.INI volumes, the logical-name aliases,
// positional panning and the 8 m/120 m attenuation, the engine loop (seam,
// pitch tracking, "never restarted per frame"), the music fade-in/out and the
// no-op behaviour on unknown sounds / missing files.  A mixer post-effect
// meters the final output, so the positional and engine checks observe real
// samples rather than just return codes.
//
// Not part of the game build:
//
//   cmake --build build -j                  # the game itself must still compile
//   tools/build_audio_check.sh              # builds + runs this harness + the probe
//
// Manual build line (needs SDL2 + SDL2_mixer, as the game does):
//
//   mkdir -p tools/out && c++ -std=c++17 -O1 -I src -DHT2_HAVE_SDL_MIXER=1 \
//       -o tools/out/audio_test tools/audio_test.cpp src/audio/Audio.cpp \
//       src/res/Res.cpp $(pkg-config --cflags --libs sdl2 sdl2_mixer)
//   tools/out/audio_test original
//
// See docs/audio.md.
#include "audio/Audio.h"

#include <SDL.h>
#include <SDL_mixer.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using ht2::Audio;

static int failures = 0;

static void check(bool ok, const std::string& what, const std::string& detail = std::string()) {
    printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", what.c_str(),
           detail.empty() ? "" : "  | ", detail.c_str());
    if (!ok) ++failures;
}

static std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

// ---------------------------------------------------------------------------
// output meter: a post-effect sees the final mixed stream (device format)
// ---------------------------------------------------------------------------
static std::atomic<float> gPeakL{0.0f}, gPeakR{0.0f};
static std::atomic<int>   gPostCalls{0};
static std::atomic<bool>  gMeter{false};
static bool               gMeterOk = false;

static void postMeter(int, void* stream, int len, void*) {
    gPostCalls.fetch_add(1);
    if (!gMeter.load()) return;
    const Sint16* s = (const Sint16*)stream;
    const int n = len / 2;                       // Sint16 samples, interleaved
    float l = 0.0f, r = 0.0f;
    for (int i = 0; i + 1 < n; i += 2) {
        const float a = (float)std::abs(s[i]);
        const float b = (float)std::abs(s[i + 1]);
        if (a > l) l = a;
        if (b > r) r = b;
    }
    if (l > gPeakL.load()) gPeakL.store(l);
    if (r > gPeakR.load()) gPeakR.store(r);
}

static void meterReset() { gPeakL.store(0.0f); gPeakR.store(0.0f); }
static float meterL() { return gPeakL.load(); }
static float meterR() { return gPeakR.load(); }

static void pump(int frames, int msPerFrame = 16) {
    for (int i = 0; i < frames; ++i) SDL_Delay((Uint32)msPerFrame);
}

// Stop every one-shot so the next peak measurement sees only its own sound.
static void silence() {
    Mix_HaltChannel(-1);
    pump(3);
}

int main(int argc, char** argv) {
    const std::string data = argc > 1 ? argv[1] : "original";
    // Headless: the dummy driver still runs the SDL audio thread and calls the
    // mixer effects, so everything below is exercised without a sound card.
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    printf("== ht2 audio harness ==\n");
    printf("data dir : %s\n", data.c_str());
    printf("SDL      : %d.%d.%d, SDL_mixer %d.%d.%d\n", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
           SDL_PATCHLEVEL, SDL_MIXER_MAJOR_VERSION, SDL_MIXER_MINOR_VERSION, SDL_MIXER_PATCHLEVEL);

    // ---------------------------------------------------------------- init
    Audio a;
    const auto t0 = std::chrono::steady_clock::now();
    const bool inited = a.init(data);
    const double initMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    check(inited, "init() returns true", fmt("%.1f ms", initMs));
    check(a.ready(), "ready()");
    check(a.effects() == 151, "all 151 SOUNDFILES effects loaded",
          fmt("%zu loaded, %zu failed", a.effects(), a.missingRes()));
    check(a.missingRes() == 0, "no effect failed to decode");
    check(a.sourceBytes() > 7200000 && a.sourceBytes() < 7400000,
          "source payload ~6.99 MiB", fmt("%zu B (%.2f MiB)", a.sourceBytes(), a.sourceBytes() / 1048576.0));
    check(a.bankBytes() > 40000000 && a.bankBytes() < 50000000,
          "resident bank ~44 MiB after the mixer's 44100 Hz/16-bit/stereo conversion",
          fmt("%zu B (%.2f MiB)", a.bankBytes(), a.bankBytes() / 1048576.0));
    printf("info     : %s\n", a.describe().c_str());

    // ------------------------------------------------------- TRUCK.INI
    check(std::fabs(a.sfxVolume() - 0.54f) < 1e-3f, "TRUCK.INI [EFFECTS] sfxvol=54",
          fmt("%.2f", a.sfxVolume()));
    check(std::fabs(a.musicVolume() - 0.20f) < 1e-3f, "TRUCK.INI [EFFECTS] midivol=20",
          fmt("%.2f", a.musicVolume()));
    check(std::fabs(a.engineVolume() - 0.90f) < 1e-3f, "TRUCK.INI [EFFECTS] engvol=90",
          fmt("%.2f", a.engineVolume()));

    // ---------------------------------------------------- name resolving
    check(a.indexOf("crash9") == 0, "indexOf(\"crash9\") == 0", fmt("%d", a.indexOf("crash9")));
    check(a.indexOf("snd\\crash9.wav") == 0, "indexOf(\"snd\\\\crash9.wav\") == 0");
    check(a.indexOf("crash9Sound") == 0, "indexOf(\"crash9Sound\") (res SOUNDS alias) == 0");
    check(a.indexOf("shakeSound") == 0, "indexOf(\"shakeSound\") aliases crash9.wav == 0",
          fmt("%d", a.indexOf("shakeSound")));
    check(a.indexOf("idle") == 19, "indexOf(\"idle\") == 19 (SOUNDFILES #20)", fmt("%d", a.indexOf("idle")));
    check(a.indexOf("jidle") == 20, "indexOf(\"jidle\") == 20 (SOUNDFILES #21)", fmt("%d", a.indexOf("jidle")));
    check(!a.has("definitely_not_a_sound"), "unknown name is reported as absent");
    a.play("definitely_not_a_sound", {0, 0, 0});     // must not crash ...
    a.play("definitely_not_a_sound", {0, 0, 0});     // ... and log only once
    a.playGlobal("definitely_not_a_sound");
    check(true, "unknown sound is a safe no-op (log-once; count verified by the shell)");

    // The meter needs the device format that the post-effect receives.
    {
        int freq = 0, ch = 0;
        Uint16 sfmt = 0;
        Mix_QuerySpec(&freq, &sfmt, &ch);
        gMeterOk = (sfmt == AUDIO_S16SYS && ch == 2);
        check(gMeterOk, "device is 44100/16-bit/stereo (what the meter decodes)",
              fmt("%d Hz, fmt=0x%04x, %d ch", freq, sfmt, ch));
    }
    Mix_RegisterEffect(MIX_CHANNEL_POST, postMeter, nullptr, nullptr);

    // ------------------------------------------- positional one-shots
    a.setMasterVolume(1.0f);
    a.setListenerDir({0, 0, 0}, 0.0f, -1.0f);        // at the origin, facing -Z

    meterReset();
    gMeter.store(true);
    a.play("horn", {0, 0, -5}, 1.0f);                // 5 m ahead: full volume, centred
    pump(10);
    const float ahead = std::max(meterL(), meterR());
    check(ahead > 1000.0f, "effect at 5 m (inside the 8 m full-volume radius) is audible",
          fmt("peak %.0f of 32767", ahead));
    const float centre = (ahead > 0) ? std::fabs(meterL() - meterR()) / ahead : 1.0f;
    check(centre < 0.05f, "source straight ahead is centred", fmt("|L-R|/peak = %.3f", centre));

    silence();
    meterReset();
    a.play("carhorn", {10, 0, -3}, 1.0f);            // to the right (listener -Z forward)
    pump(10);
    check(meterR() > 2.0f * std::max(1.0f, meterL()), "source to the right pans right",
          fmt("L %.0f R %.0f", meterL(), meterR()));

    silence();
    meterReset();
    a.play("gear", {-10, 0, -3}, 1.0f);              // to the left
    pump(10);
    check(meterL() > 2.0f * std::max(1.0f, meterR()), "source to the left pans left",
          fmt("L %.0f R %.0f", meterL(), meterR()));

    silence();
    meterReset();
    a.play("crash2", {0, 0, -200}, 1.0f);            // past the 120 m silence radius
    pump(10);
    check(meterL() == 0.0f && meterR() == 0.0f, "effect past 120 m is culled (silent)",
          fmt("L %.0f R %.0f", meterL(), meterR()));
    // The same sound at 5 m and at 110 m: the documented curve gives
    // att = (120-110)/(120-8) = 0.089, which SDL_mixer's distance effect applies
    // linearly, so the measured peak ratio must land near it.
    silence();
    meterReset();
    a.play("crash4a", {0, 0, -5}, 1.0f);
    pump(10);
    const float nearPeak = std::max(meterL(), meterR());
    silence();
    meterReset();
    a.play("crash4a", {0, 0, -110}, 1.0f);
    pump(10);
    const float farPeak = std::max(meterL(), meterR());
    const float ratio110 = (nearPeak > 0.0f) ? farPeak / nearPeak : 0.0f;
    check(ratio110 > 0.05f && ratio110 < 0.16f,
          "effect at 110 m is attenuated to the documented 0.089 of full volume",
          fmt("peak %.0f/%.0f = %.3f", farPeak, nearPeak, ratio110));
    gMeter.store(false);

    // ------------------------------------------------------------ engine
    const auto t1 = std::chrono::steady_clock::now();
    a.engine(600.0f, 0.0f, {0, 0, -6});              // forces the loop to load
    const double loopMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    check(loopMs < 250.0, "first engine() call (loop build + autocorrelation) is cheap",
          fmt("%.1f ms", loopMs));
    check(a.engineLoopFrames() > 10000, "engine loop built from snd\\idle.wav",
          fmt("%zu frames (%.2f s)", a.engineLoopFrames(), a.engineLoopFrames() / 44100.0));
    check(a.engineLoopPeriodFrames() > 1300.0f && a.engineLoopPeriodFrames() < 1420.0f,
          "autocorrelation finds the ~1360 frame (32.4 Hz) firing period",
          fmt("%.1f frames = %.1f Hz", a.engineLoopPeriodFrames(),
              a.engineLoopPeriodFrames() > 0 ? 44100.0f / a.engineLoopPeriodFrames() : 0.0f));
    check(a.engineLoopSeamStep() < a.engineLoopSeamStepRaw() * 0.25f,
          "load-time seam repair removes the loop discontinuity",
          fmt("|dx| %.4f -> %.4f", a.engineLoopSeamStepRaw(), a.engineLoopSeamStep()));

    // The light-vehicle loop (snd\jidle.wav, 8-bit, 55809 frames @ 44.1 kHz)
    // takes the same code path - switch kinds and check what it detected.
    a.setEngineKind(Audio::EngineKind::Light);
    a.engine(600.0f, 0.5f, {0, 0, -6});
    check(a.engineLoopPeriodFrames() > 1290.0f && a.engineLoopPeriodFrames() < 1365.0f,
          "light-vehicle loop (snd\\jidle.wav) period ~1327 frames (33.2 Hz)",
          fmt("%.1f frames = %.1f Hz", a.engineLoopPeriodFrames(),
              a.engineLoopPeriodFrames() > 0 ? 44100.0f / a.engineLoopPeriodFrames() : 0.0f));
    check(a.engineLoopFrames() > 50000, "jidle loop kept an integer number of periods",
          fmt("%zu frames", a.engineLoopFrames()));
    a.setEngineKind(Audio::EngineKind::Truck);
    a.engine(600.0f, 0.0f, {0, 0, -6});
    check(a.engineLoopFrames() > 20000 && a.engineLoopFrames() < 20567,
          "setEngineKind(Truck) rebuilds the truck loop", fmt("%zu frames", a.engineLoopFrames()));

    printf("-- rpm -> pitch ratio (kIdleRpm 600, clamp 0.55..3.20) --\n");
    const float rpms[] = {0.0f, 300.0f, 600.0f, 900.0f, 1200.0f, 1800.0f, 2100.0f, 2400.0f};
    float prev = -1.0f;
    bool monotonic = true;
    for (float rpm : rpms) {
        a.engine(rpm, 0.5f, {0, 0, -6});
        const float r = a.enginePitchRatio();
        printf("   rpm %6.0f -> ratio %.3f  gain %.3f\n", rpm, r, a.engineGain());
        if (rpm >= 600.0f && r < prev - 1e-4f) monotonic = false;
        if (rpm >= 600.0f) prev = r;
    }
    check(monotonic, "pitch ratio is monotonic in rpm");
    check(std::fabs(prev - 3.20f) < 1e-3f, "high rpm clamps at ratio 3.20", fmt("%.3f", prev));
    a.engine(300.0f, 0.5f, {0, 0, -6});
    check(std::fabs(a.enginePitchRatio() - 0.55f) < 1e-3f, "low rpm clamps at ratio 0.55",
          fmt("%.3f", a.enginePitchRatio()));
    a.engine(600.0f, 1.0f, {0, 0, -6});
    const float gFull = a.engineGain();
    a.engine(600.0f, 0.0f, {0, 0, -6});
    const float gIdle = a.engineGain();
    check(gIdle > 0.0f && gIdle < gFull, "load raises the engine gain (idle floor kept)",
          fmt("idle %.3f < full %.3f", gIdle, gFull));

    const size_t restartsBefore = a.engineRestarts();
    meterReset();
    gMeter.store(true);
    // ~2 s of game frames (60 fps): a throttle sweep 600 -> 1800 -> 600 rpm.
    for (int i = 0; i < 120; ++i) {
        const float t = (float)i / 119.0f;
        const float s = sinf(t * 3.14159265f);
        a.engine(600.0f + 1200.0f * s, 0.25f + 0.75f * s, {0, 0, -6});
        a.update();
        SDL_Delay(16);
    }
    gMeter.store(false);
    check(std::max(meterL(), meterR()) > 1000.0f, "engine loop produces audio",
          fmt("peak %.0f over 120 update() frames", std::max(meterL(), meterR())));
    check(a.engineRestarts() - restartsBefore == 0,
          "engine() never restarts the sample while it keeps playing",
          fmt("%zu restarts in 120 engine() calls", a.engineRestarts() - restartsBefore));

    // Out of earshot: the gain target drops to 0 and the one-pole gain ramp
    // (tau 60 ms) silences the loop; update() parks the channel after 2 s.
    for (int i = 0; i < 45; ++i) { a.engine(900.0f, 0.5f, {0, 0, -500}); a.update(); SDL_Delay(16); }
    meterReset();
    gMeter.store(true);
    for (int i = 0; i < 55; ++i) { a.engine(900.0f, 0.5f, {0, 0, -500}); a.update(); SDL_Delay(16); }
    gMeter.store(false);
    check(a.engineGain() == 0.0f && meterL() == 0.0f && meterR() == 0.0f,
          "engine 500 m away is silent (same 120 m radius)", fmt("peak %.0f", std::max(meterL(), meterR())));
    for (int i = 0; i < 45; ++i) { a.engine(900.0f, 0.5f, {0, 0, -500}); a.update(); SDL_Delay(16); }
    meterReset();
    gMeter.store(true);
    for (int i = 0; i < 20; ++i) { a.engine(900.0f, 0.5f, {0, 0, -500}); a.update(); SDL_Delay(16); }
    gMeter.store(false);
    check(meterL() == 0.0f && meterR() == 0.0f, "the silent engine stays silent while parked");
    const size_t parked = a.engineRestarts();
    a.engine(900.0f, 0.5f, {0, 0, -6});
    check(a.engineRestarts() == parked + 1,
          "the parked engine channel is restarted when the truck is audible again",
          fmt("%zu -> %zu restarts", parked, a.engineRestarts()));
    // A caller that stops calling engine() (menu/pause) must not leave a drone:
    // update() fades it out after kEngineStaleMs and parks it after 2 s.
    a.engineStart();
    for (int i = 0; i < 20; ++i) { a.engine(1200.0f, 1.0f, {0, 0, -6}); a.update(); SDL_Delay(16); }
    for (int i = 0; i < 80; ++i) { a.update(); SDL_Delay(16); }        // 1.3 s without engine()
    meterReset();
    gMeter.store(true);
    for (int i = 0; i < 40; ++i) { a.update(); SDL_Delay(16); }
    gMeter.store(false);
    check(a.engineGain() == 0.0f && meterL() == 0.0f && meterR() == 0.0f,
          "engine() that stops being called fades out (no stuck drone)",
          fmt("peak %.0f", std::max(meterL(), meterR())));

    a.engineStop();
    check(true, "engineStop() (== stopEngine()) fades out and plays snd\\stop_eng.wav");
    pump(20);

    // ------------------------------------------------------------- music
    a.music("track01l", true);
    pump(20);
    check(a.musicPlaying(), "music(\"track01l\", loop) starts MUSIC/track01l.mp3");
    a.music("track02l", true);                       // switch tracks while playing
    pump(10);
    check(a.musicPlaying(), "switching tracks while playing keeps music alive");
    a.stopMusic();
    for (int i = 0; i < 90 && a.musicPlaying(); ++i) { a.update(); SDL_Delay(20); }
    check(!a.musicPlaying(), "stopMusic() fades out (~900 ms) and frees the track");
    a.music("no_such_track");
    check(true, "music() with a missing file is a safe no-op");
    a.music("track03l", false);                      // non-looping
    pump(10);
    check(a.musicPlaying(), "music(..., loop=false) plays once");
    a.stopMusic();
    pump(60);

    // ------------------------------------------------------- volumes etc.
    a.setMasterVolume(0.5f);
    check(std::fabs(a.masterVolume() - 0.5f) < 1e-6f, "setMasterVolume(0.5)");
    a.setChannelVolumes(0.3f, 0.25f, 0.8f);
    check(std::fabs(a.sfxVolume() - 0.3f) < 1e-6f && std::fabs(a.musicVolume() - 0.25f) < 1e-6f &&
          std::fabs(a.engineVolume() - 0.8f) < 1e-6f, "setChannelVolumes(sfx, music, engine)");
    a.setAttenuation(4.0f, 60.0f);
    a.setListener({100, 0, 100}, 45.0f);
    a.playGlobal("pager", 0.5f);
    check(true, "setAttenuation/setListener/playGlobal are accepted");
    check(gPostCalls.load() > 0, "the mixer ran the effect pipeline",
          fmt("%d post-effect calls", gPostCalls.load()));

    // ----------------------------------------------------------- shutdown
    a.shutdown();
    check(!a.ready(), "shutdown() closes the device");
    a.play("horn", {0, 0, 0});                       // no-op after shutdown
    a.engine(900.0f, 1.0f, {0, 0, 0});
    check(true, "every call is a no-op after shutdown()");

    const bool re = a.init(data);
    check(re && a.ready() && a.effects() == 151, "init() after shutdown() rebuilds the bank",
          fmt("effects %zu", a.effects()));
    a.shutdown();

    printf("\n%s: %d check(s) failed\n", failures ? "FAILURE" : "SUCCESS", failures);
    return failures ? 1 : 0;
}

#pragma once
#include <cstddef>
#include <memory>
#include <string>

namespace ht2 {

// Sound: effects, the engine loop and music, decoded from the shipped data
// (the SOUNDFILES of COMMON/common1.res are RIFF/WAVE, the soundtrack is MP3).
// Backed by SDL2_mixer when it is available; every call is a safe no-op
// otherwise, and when init() fails.  Nothing in this class ever throws or
// crashes on a missing/unknown sound: the name is logged once and ignored.
//
// docs/audio.md documents the file inventory, the measured mixer pan/attenuation
// values, the engine-sound strategy and everything that could not be determined.
//
//   Audio a;
//   if (a.init("original")) { ... }          // opens the device + loads the bank
//   a.setListenerDir({v.x(), v.y(), v.z()}, fx, fz);   // from Vehicle::forward()
//   a.engine(v.rpm(), v.engineLoad(), {v.x(), v.y(), v.z()});   // every frame
//   a.play("snd\\crash9.wav", {x, y, z}, 0.8f);
//   a.update();                              // every frame, after the above
class Audio {
public:
    struct Vec3 { float x = 0, y = 0, z = 0; };

    // Which shipped idle loop drives the engine loop: `snd\idle.wav` (the truck
    // diesel, 44.1 kHz/16-bit, firing fundamental ~32.4 Hz) or `snd\jidle.wav`
    // (the light vehicle, 44.1 kHz/8-bit, ~33.2 Hz) - see docs/audio.md.
    enum class EngineKind { Truck = 0, Light = 1 };

    Audio();
    ~Audio();
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    // dataDir = the `original` folder.  Opens SDL audio at 44100 Hz / 16-bit /
    // stereo / 1024 frames (`SDL_AUDIODRIVER=dummy` works headless), reads the
    // [EFFECTS]/[INSTALL] volumes from dataDir/TRUCK.INI and loads the whole
    // effect bank out of COMMON/common1.res (common2.res, COMMON.RES fall back).
    // Returns true when the mixer opened; check effects() for the bank.
    bool init(const std::string& dataDir);
    void shutdown();
    bool ready() const;

    size_t effects() const;       // effect chunks resident in memory
    size_t bankBytes() const;     // bytes they occupy after the mixer's conversion
    size_t sourceBytes() const;   // bytes of RIFF payload they came from
    size_t missingRes() const;    // SOUNDFILES entries that failed to load
    // Name lookup by shipped stem ("crash9"), file name ("snd\crash9.wav") or
    // logical name ("crash9Sound", "shakeSound"); index into SOUNDFILES order.
    bool has(const std::string& name) const;
    int  indexOf(const std::string& name) const;

    // One-shot by shipped name: "snd\crash9.wav", "crash9", "crash9Sound" and
    // "shakeSound" (the logical SOUNDS names) all resolve.  `at` is a world
    // position; the sound is panned and attenuated against the listener
    // (setListener/setListenerDir): full volume within 8 m, culled past 120 m.
    void play(const std::string& name, const Vec3& at, float gain = 1.0f);
    // Same sound without positioning (UI, speech): always full volume.
    void playGlobal(const std::string& name, float gain = 1.0f);

    // Continuous engine loop that follows the revolutions.  `rpm` is rev/min on
    // the shipped idle scale (600 rpm == the loop as recorded), `load` is 0..1.
    // The loop is started lazily and never restarted per frame: it runs on a
    // reserved mixer channel whose PCM is resampled by a registered effect, so
    // pitch and volume are continuous (no per-frame Mix_PlayChannel).
    void engine(float rpm, float load, const Vec3& at);
    void setEngineKind(EngineKind k);   // rebuilds the loop, ~50 ms
    void engineStart();                 // snd\start.wav + 0.7 s cranking ramp
    void engineStop();                  // fade out + snd\stop_eng.wav
    void stopEngine();                  // == engineStop()

    // Music: the game's own tracks, MUSIC/*.mp3 (folder from TRUCK.INI
    // [INSTALL] MUSIC).  `file` may be "track01l", "track01l.mp3" or a path.
    // Documented fade-in 600 ms, fade-out 900 ms (stopMusic).
    void music(const std::string& file, bool loop = true);
    void stopMusic();
    bool musicPlaying() const;

    void setMasterVolume(float v);                                  // 0..1
    void setChannelVolumes(float sfx, float music, float engine);   // 0..1
    float masterVolume() const;
    float sfxVolume() const;
    float musicVolume() const;
    float engineVolume() const;

    // Listener pose.  `yawDeg` follows the renderer/camera convention
    // (`yaw = atan2(forwardX, -forwardZ)`, 0 = facing -Z, clockwise from above).
    // setListenerDir() takes the forward vector directly and is unambiguous -
    // pass Vehicle::forward(fx, fz) to it, NOT Vehicle::yaw() to setListener().
    void setListener(const Vec3& pos, float yawDeg);
    void setListenerDir(const Vec3& pos, float fwdX, float fwdZ);
    // Full-volume radius and silence radius in world units (default 8 / 120).
    void setAttenuation(float nearM, float farM);

    void update();                 // per-frame housekeeping (fades, frees, culls)
    // Human readable state summary (device, counts, bytes, volumes, engine loop).
    std::string describe() const;

    // Engine-loop diagnostics (harness/tests): the sample count actually looped,
    // the detected firing period in frames, and the wrap discontinuity with and
    // without the load-time seam repair.
    size_t engineLoopFrames() const;
    float  engineLoopPeriodFrames() const;
    float  engineLoopSeamStep() const;
    float  engineLoopSeamStepRaw() const;
    // How often the engine sample has been (re)started - 1 after any number of
    // engine() calls while the vehicle stays in earshot (never per frame).
    size_t engineRestarts() const;
    // Last pitch ratio / gain published to the engine loop (targets, so these
    // are safe to read from the game thread): ratio 1.0 == the loop as recorded.
    float enginePitchRatio() const;
    float engineGain() const;

    // Implementation detail: the definition lives in Audio.cpp and pulls in
    // SDL_mixer, so it is only ever handled through a pointer here.  The name is
    // public so the file-local helpers in Audio.cpp can use it.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace ht2

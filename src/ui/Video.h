#pragma once
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace ht2 {
class Renderer;

// Full-screen video playback (the intro / demo movies).
//
// The shipped movies are Indeo Video 5, which no portable library decodes, so
// tools/make_video.sh converts them once with ffmpeg into a decoder-free frame
// pack (.rvid: RGB565 frames, RLE coded - see docs/video.md) plus a WAV of the
// audio track; this player streams the pack from disk, uploads one frame at a
// time into a single GL texture and draws it letterboxed.
//
//   Video v;
//   if (v.open(dataDir, "kotr")) { v.play(); playing = true; }
//   ...
//   if (playing && !v.tick(r)) playing = false;   // ended or skipped
//
// The frame index is derived from a *clock* (SDL_GetTicks since the first frame
// went on screen), never from a per-call dt sum: a loop that cannot keep up
// skips frames instead of falling behind the sound.  The soundtrack is opened
// paused by play() and starts with the first presented frame, so it cannot run
// ahead of a picture that is still decoding its first frames.
//
// Everything is safe when the assets are missing: open() returns false, the
// other calls do nothing.
class Video {
public:
    struct Clip {
        std::string video, audio;   // resolved paths
        int w = 0, h = 0, fps = 12, frames = 0;
        uint64_t packedBytes = 0;   // size of the .rvid
        uint64_t rawBytes = 0;      // uncompressed RGB565 size (for reports)
    };

    Video() = default;
    ~Video();
    Video(const Video&) = delete;
    Video& operator=(const Video&) = delete;

    // Finds <dataDir>/<name>.rvid (+ .wav), then <dataDir>/VIDEO/, then
    // <dataDir>/media/, then media/ and build/media/ (where make_video.sh
    // writes).  Returns false when no pack is found or it is malformed.
    bool open(const std::string& dataDir, const std::string& name);
    void close();
    bool ready() const { return file_ != nullptr; }
    const Clip& clip() const { return clip_; }

    bool playing() const { return playing_; }
    // Resets the clock to 0 and opens the WAV/device *paused*: the sound starts
    // with the first frame on screen (tick()), not here.
    void play(bool withAudio = true);
    void stop();
    // One playback step: reads the master clock, decodes the frame the clock
    // points at, uploads it and draws it.  Because the index is a pure function
    // of the clock, a slow loop skips frames; the audio is fed from the same
    // clock and starts with frame 0, so the sound cannot run ahead.  Returns
    // false when the clip ended (or was skipped), true while it is on screen.
    bool tick(Renderer& r);
    // Deterministic frame stepping: `dt` advances a synthetic clock by hand,
    // frame by frame, with no wall clock involved.  Kept for the API harness
    // (tools/video_check.cpp); the game calls tick().
    bool update(Renderer& r, float dt);
    // Draws the current frame again without reading the clock (screenshot paths).
    void present(Renderer& r);
    // Deterministic seek: `seconds` (or `index`) becomes the clock, that frame is
    // decoded/uploaded/drawn and a playing soundtrack is moved to the matching
    // offset.  --movie-time/--movie-frames use this, so no timing is involved.
    bool seek(Renderer& r, double seconds);
    bool seekFrame(Renderer& r, size_t index);
    void skip() { playing_ = false; audioClose(); }
    size_t frame() const { return frame_; }

    // ---- extras -----------------------------------------------------------
    float elapsed() const { return (float)timeSec_; }
    // The clock the frame index is derived from: SDL_GetTicks since the first
    // presented frame, or the value forced by setClockOverride().
    double videoTime() const { return timeSec_; }
    // Mixer position in seconds: the sound the device has actually taken out of
    // the queue (0 while there is no soundtrack).  Video time - audio time is
    // the A/V drift the sync report measures; it must stay inside one frame.
    double audioTime() const;
    double audioDuration() const;   // length of the soundtrack in seconds
    bool audioRunning() const;      // device open, started and still fed
    float duration() const { return clip_.fps > 0 ? (float)clip_.frames / (float)clip_.fps : 0.0f; }
    // Decodes frame `index` out of the pack into rgba().  Does not need a GL
    // context; tick() uses it internally.
    bool loadFrame(size_t index);
    const std::vector<uint8_t>& rgba() const { return rgba_; }
    void setAudioEnabled(bool on) { audioWanted_ = on; }
    bool audioReady() const { return audioDev_ != 0; }
    // Test hook (--movie-sync-report): forces videoTime() to `seconds` without a
    // wall clock or a sound device; < 0 restores the real clock.  Lets a headless
    // check prove that the frame index is a function of the clock.
    void setClockOverride(double seconds) { clockOverride_ = seconds; }
    // Letterboxed destination rectangle of the current frame in pixels.
    void dstRect(const Renderer& r, float& x, float& y, float& w, float& h) const;
    // Drops the GL texture.  close()/the destructor do this too when a GL
    // context is still current; call it explicitly before Renderer::shutdown().
    void releaseTexture(Renderer& r);

private:
    std::string resolve(const std::string& dataDir, const std::string& name,
                        const char* ext) const;
    void draw(Renderer& r);
    bool upload(Renderer& r);
    // decode (when needed) + upload + draw, and make `index` the current frame
    bool show(Renderer& r, size_t index);
    double masterTime();
    void audioOpen();
    void audioStart(double fromSec);   // unpause + anchor, called with frame 0 up
    void audioSeekTo(double fromSec);  // drop the queue and refill from fromSec
    void audioPump(double upto);
    double wavBytesPerSec() const;
    void audioClose();

    Clip clip_;
    std::FILE* file_ = nullptr;
    std::vector<uint32_t> sizes_;     // per frame; bit31 set = stored raw
    std::vector<uint64_t> offsets_;   // per frame, absolute file offset
    std::vector<uint8_t> payload_;    // compressed frame scratch
    std::vector<uint16_t> pix_;       // decoded RGB565 frame
    std::vector<uint8_t> rgba_;       // decoded RGBA8 frame (for the texture)

    long long loadedFrame_ = -1;
    size_t frame_ = 0;
    bool playing_ = false;

    // Master clock: SDL_GetTicks since the frame that actually went on screen,
    // or - while a soundtrack is playing - the mixer position, so the picture is
    // slaved to the sound and can never run ahead of it.
    bool clockStarted_ = false;
    double clockBaseSec_ = 0.0;
    uint32_t clockAnchorMs_ = 0;
    double timeSec_ = 0.0;
    double clockOverride_ = -1.0;      // tests; < 0 = the real clock
    double nextSyncLog_ = 0.0;         // periodic drift line

    unsigned tex_ = 0;
    int texW_ = 0, texH_ = 0;

    // Audio: SDL core (no SDL_mixer).  The device is opened paused and started
    // together with the clock, see tick()/audioStart().  Silently disabled when
    // the device or the WAV cannot be used.
    bool audioWanted_ = true;
    bool audioStarted_ = false;
    bool audioMaster_ = false;    // the mixer position drives the clock
    unsigned audioDev_ = 0;
    void* audioStream_ = nullptr;     // SDL_AudioStream*
    bool streamFlushed_ = false;
    uint8_t* wavData_ = nullptr;
    uint32_t wavLen_ = 0, wavSent_ = 0;
    int wavFreq_ = 0, wavChannels_ = 0, wavFrameBytes_ = 2;
    int devFreq_ = 0, devChannels_ = 0, devFrameBytes_ = 2;
    uint64_t queuedBytes_ = 0;        // device bytes handed to SDL since the anchor
    double audioBaseSec_ = 0.0;       // media time of the queued audio (seeks)
    double lastAudioSec_ = 0.0;       // progress watchdog: position and the tick
    uint32_t lastAudioMs_ = 0;        // at which it last moved
};

} // namespace ht2

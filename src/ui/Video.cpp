#include "Video.h"
#include "../gfx/Renderer.h"
#include "../util/Log.h"
#include <SDL.h>
#include <algorithm>
#include <cstring>
#include <sys/stat.h>

namespace ht2 {

// ---------------------------------------------------------------------------
// The pack format is documented in docs/video.md:
//   "RVID", u32 version/w/h/fps/frameCount/flags/tableOffset, then a u32 size
//   table (bit31 = frame stored raw) followed by one payload per frame.  A
//   payload is w*h little-endian RGB565 pixels, top-down, R in bits 15..11, or
//   a PackBits style byte stream over those 16-bit pixels:
//     control < 0x80 : literal run of (control+1) pixels, 2 bytes each
//     control >=0x80 : repeat  run of ((control&0x7F)+2) pixels, one 16-bit value
// ---------------------------------------------------------------------------

static const uint32_t RAW_FLAG = 0x80000000u;
static const int MAX_FRAMES = 400000;
static const int MAX_DIM = 8192;

// Seconds of soundtrack kept queued ahead of the picture, and how often the
// video/mixer drift is logged while a movie plays (docs/video.md 6).
static const double AUDIO_AHEAD = 0.75;
static const double SYNC_LOG_EVERY = 10.0;

static bool fileExists(const std::string& p) {
    struct stat st;
    return !p.empty() && stat(p.c_str(), &st) == 0 && (st.st_mode & S_IFREG);
}

// Renderer offers createTexture()/deleteTexture() but no way to update a
// texture in place, so the per-frame upload goes straight through GL.  All
// three entry points are GL 1.1 and therefore present in any core context; if
// they are missing the player falls back to rebuilding the texture.
typedef void (*PFN_BindTexture)(unsigned, unsigned);
typedef void (*PFN_TexParameteri)(unsigned, unsigned, int);
typedef void (*PFN_TexSubImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);
typedef void (*PFN_DeleteTextures)(int, const unsigned*);
static PFN_BindTexture s_glBindTexture = nullptr;
static PFN_TexParameteri s_glTexParameteri = nullptr;
static PFN_TexSubImage2D s_glTexSubImage2D = nullptr;
static PFN_DeleteTextures s_glDeleteTextures = nullptr;
static bool s_glProbed = false;

static void probeGL() {
    if (s_glProbed) return;
    s_glProbed = true;
    s_glBindTexture = (PFN_BindTexture)SDL_GL_GetProcAddress("glBindTexture");
    s_glTexParameteri = (PFN_TexParameteri)SDL_GL_GetProcAddress("glTexParameteri");
    s_glTexSubImage2D = (PFN_TexSubImage2D)SDL_GL_GetProcAddress("glTexSubImage2D");
    s_glDeleteTextures = (PFN_DeleteTextures)SDL_GL_GetProcAddress("glDeleteTextures");
    if (!s_glTexSubImage2D)
        LOG("video: glTexSubImage2D unavailable, texture updates will rebuild");
}

Video::~Video() { close(); }

std::string Video::resolve(const std::string& dataDir, const std::string& name,
                           const char* ext) const {
    std::string base = name;
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && base.compare(dot, 5, ".rvid") == 0)
        base = base.substr(0, dot);
    if (base.empty()) return std::string();
    const std::string dirs[] = {dataDir, dataDir + "/VIDEO", dataDir + "/media",
                                "build/media", "media", "."};
    std::vector<std::string> all(dirs, dirs + sizeof(dirs) / sizeof(dirs[0]));
    // Also look next to the executable, so the game finds build/media/ no matter
    // which directory it was started from (the binary lives in build/).
    if (char* baseDir = SDL_GetBasePath()) {
        all.push_back(std::string(baseDir) + "media");
        all.push_back(std::string(baseDir) + "../build/media");
        all.push_back(std::string(baseDir) + "../media");
        SDL_free(baseDir);
    }
    for (const std::string& d : all) {
        std::string p = (d.empty() || d == ".") ? base + ext : d + "/" + base + ext;
        if (fileExists(p)) return p;
    }
    return std::string();
}

bool Video::open(const std::string& dataDir, const std::string& name) {
    close();
    if (name.empty()) return false;
    const std::string vpath = resolve(dataDir, name, ".rvid");
    if (vpath.empty()) {
        LOG("video: %s.rvid not found - run tools/make_video.sh", name.c_str());
        return false;
    }
    file_ = std::fopen(vpath.c_str(), "rb");
    if (!file_) { LOG("video: cannot open %s", vpath.c_str()); return false; }

    std::fseek(file_, 0, SEEK_END);
    const long long fsize = std::ftell(file_);
    std::fseek(file_, 0, SEEK_SET);
    unsigned char hdr[32];
    if (fsize < 32 || std::fread(hdr, 1, 32, file_) != 32 ||
        std::memcmp(hdr, "RVID", 4) != 0) {
        LOG("video: %s is not an RVID pack", vpath.c_str());
        close();
        return false;
    }
    uint32_t f[7];
    std::memcpy(f, hdr + 4, sizeof(f));
    const uint32_t version = f[0], w = f[1], h = f[2], fps = f[3],
                   frames = f[4], flags = f[5], tableOff = f[6];
    if (version != 1 || w == 0 || h == 0 || fps == 0 || frames == 0 ||
        w > MAX_DIM || h > MAX_DIM || frames > MAX_FRAMES || tableOff != 32) {
        LOG("video: %s has an unsupported header (v%u %ux%u %ufps %u frames)",
            vpath.c_str(), version, w, h, fps, frames);
        close();
        return false;
    }

    sizes_.resize(frames);
    if (std::fread(sizes_.data(), 4, frames, file_) != frames) {
        LOG("video: %s size table is truncated", vpath.c_str());
        close();
        return false;
    }
    offsets_.resize(frames);
    uint64_t pos = 32ull + 4ull * frames;
    uint64_t maxPayload = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        offsets_[i] = pos;
        const uint64_t sz = sizes_[i] & ~RAW_FLAG;
        if (sz == 0 || sz > (uint64_t)w * h * 2) {
            LOG("video: %s frame %u has a bad size (%llu)", vpath.c_str(), i,
                (unsigned long long)sz);
            close();
            return false;
        }
        maxPayload = std::max(maxPayload, sz);
        pos += sz;
    }
    if (pos != (uint64_t)fsize) {
        LOG("video: %s table+payload is %llu bytes, file is %lld",
            vpath.c_str(), (unsigned long long)pos, fsize);
        close();
        return false;
    }

    clip_.video = vpath;
    clip_.audio = resolve(dataDir, name, ".wav");
    clip_.w = (int)w; clip_.h = (int)h; clip_.fps = (int)fps; clip_.frames = (int)frames;
    clip_.packedBytes = (uint64_t)fsize;
    clip_.rawBytes = (uint64_t)w * h * 2 * frames;
    payload_.resize((size_t)maxPayload);
    pix_.resize((size_t)w * h);
    rgba_.assign((size_t)w * h * 4, 255);
    frame_ = 0;
    loadedFrame_ = -1;
    timeSec_ = 0.0;
    LOG("video: %s %ux%u @%ufps %u frames (%.1f s, %.1f MB%s)",
        vpath.c_str(), w, h, fps, frames, (double)frames / fps, fsize / 1e6,
        (flags & 1) ? ", rle" : ", raw");
    return true;
}

void Video::releaseTexture(Renderer& r) {
    if (!tex_) return;
    r.deleteTexture(tex_);
    tex_ = 0;
    texW_ = texH_ = 0;
}

void Video::close() {
    audioClose();
    // Deleting a texture needs a current context; if the window is already gone
    // just forget the id (the context takes it with it).
    if (tex_) {
        probeGL();
        if (s_glDeleteTextures && SDL_GL_GetCurrentContext()) {
            unsigned t = tex_;
            s_glDeleteTextures(1, &t);
        }
        tex_ = 0;
    }
    if (file_) { std::fclose(file_); file_ = nullptr; }
    sizes_.clear(); offsets_.clear();
    payload_.clear(); pix_.clear(); rgba_.clear();
    clip_ = Clip();
    loadedFrame_ = -1; frame_ = 0; playing_ = false;
    clockStarted_ = false; clockBaseSec_ = 0.0; clockAnchorMs_ = 0;
    timeSec_ = 0.0; clockOverride_ = -1.0; nextSyncLog_ = 0.0;
    texW_ = texH_ = 0;
}

void Video::play(bool withAudio) {
    if (!file_) return;
    frame_ = 0;
    loadedFrame_ = -1;
    playing_ = true;
    audioClose();
    clockStarted_ = false;          // starts with the first frame on screen
    clockBaseSec_ = 0.0;
    clockAnchorMs_ = 0;
    timeSec_ = 0.0;
    clockOverride_ = -1.0;
    nextSyncLog_ = 0.0;
    audioWanted_ = withAudio;
    // The device is opened paused; the soundtrack itself starts in tick(), with
    // the first frame, so it cannot run ahead while the picture is still loading.
    if (withAudio) audioOpen();
}

void Video::stop() {
    playing_ = false;
    audioClose();
    clockStarted_ = false; clockBaseSec_ = 0.0; clockAnchorMs_ = 0;
    timeSec_ = 0.0; clockOverride_ = -1.0; nextSyncLog_ = 0.0;
    frame_ = 0;
}

bool Video::loadFrame(size_t index) {
    if (!file_ || index >= sizes_.size()) return false;
    const uint32_t entry = sizes_[index];
    const size_t size = entry & ~RAW_FLAG;
    if (size > payload_.size() || pix_.empty()) return false;
    if (std::fseek(file_, (long)offsets_[index], SEEK_SET) != 0) return false;
    if (std::fread(payload_.data(), 1, size, file_) != size) {
        LOG("video: short read on frame %zu", index);
        return false;
    }

    const size_t n = pix_.size();
    if (entry & RAW_FLAG) {
        if (size != n * 2) return false;
        std::memcpy(pix_.data(), payload_.data(), size);
    } else {
        const uint8_t* p = payload_.data();
        const uint8_t* end = p + size;
        size_t out = 0;
        while (p < end && out < n) {
            const uint8_t ctrl = *p++;
            if (ctrl < 0x80) {                       // literal run
                const size_t cnt = (size_t)ctrl + 1;
                if (p + cnt * 2 > end || out + cnt > n) return false;
                for (size_t k = 0; k < cnt; ++k, p += 2)
                    pix_[out++] = (uint16_t)(p[0] | (p[1] << 8));
            } else {                                 // repeat run
                const size_t cnt = (size_t)(ctrl & 0x7F) + 2;
                if (p + 2 > end || out + cnt > n) return false;
                const uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
                p += 2;
                for (size_t k = 0; k < cnt; ++k) pix_[out++] = v;
            }
        }
        if (out != n) return false;
    }

    for (size_t i = 0; i < n; ++i) {                 // RGB565 -> RGBA8
        const uint16_t v = pix_[i];
        const uint8_t r = (uint8_t)((v >> 11) & 0x1F), g = (uint8_t)((v >> 5) & 0x3F),
                      b = (uint8_t)(v & 0x1F);
        uint8_t* d = &rgba_[i * 4];
        d[0] = (uint8_t)((r << 3) | (r >> 2));
        d[1] = (uint8_t)((g << 2) | (g >> 4));
        d[2] = (uint8_t)((b << 3) | (b >> 2));
        d[3] = 255;
    }
    loadedFrame_ = (long long)index;
    return true;
}

bool Video::upload(Renderer& r) {
    if (rgba_.empty() || clip_.w <= 0 || clip_.h <= 0) return false;
    probeGL();
    if (!tex_) {
        // Renderer::init() must have run: whiteTex() is created there, and a
        // texture can only be made with a live GL context.
        if (r.whiteTex() == 0) return false;
        tex_ = r.createTexture(rgba_.data(), clip_.w, clip_.h);
        if (!tex_) return false;
        texW_ = clip_.w; texH_ = clip_.h;
        // createTexture() builds mipmaps for *this* frame only; a linear min
        // filter keeps the stale levels from ghosting through when the movie is
        // drawn smaller than its native size.
        if (s_glTexParameteri) s_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        return true;
    }
    // Prefer the public Renderer entry point (it also drops the stale mip levels
    // by switching the min filter); fall back to our own GL path when the
    // renderer has not been rebuilt with it.
    if (texW_ == clip_.w && texH_ == clip_.h && r.updateTexture(tex_, rgba_.data(), clip_.w, clip_.h))
        return true;
    if (s_glTexSubImage2D && s_glBindTexture && texW_ == clip_.w && texH_ == clip_.h) {
        s_glBindTexture(GL_TEXTURE_2D, tex_);
        s_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, clip_.w, clip_.h,
                          GL_RGBA, GL_UNSIGNED_BYTE, rgba_.data());
        return true;
    }
    // No glTexSubImage2D (or the clip changed geometry): rebuild in place.
    r.deleteTexture(tex_);
    tex_ = r.createTexture(rgba_.data(), clip_.w, clip_.h);
    if (!tex_) return false;
    texW_ = clip_.w; texH_ = clip_.h;
    if (s_glTexParameteri) s_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    return true;
}

void Video::dstRect(const Renderer& r, float& x, float& y, float& w, float& h) const {
    const float W = (float)r.width_, H = (float)r.height_;
    x = y = w = h = 0.0f;
    if (clip_.w <= 0 || clip_.h <= 0 || W <= 0.0f || H <= 0.0f) return;
    const float sc = std::min(W / (float)clip_.w, H / (float)clip_.h);
    w = clip_.w * sc;
    h = clip_.h * sc;
    x = (W - w) * 0.5f;
    y = (H - h) * 0.5f;
}

void Video::draw(Renderer& r) {
    if (!tex_) return;
    float x, y, w, h;
    dstRect(r, x, y, w, h);
    if (w <= 0.0f || h <= 0.0f) return;
    const float W = (float)r.width_, H = (float)r.height_;
    r.begin2D();
    const unsigned white = r.whiteTex();
    if (white) {                                  // black bars / letterbox
        if (x > 0.5f) {
            r.quad2D(white, 0, 0, x, H, 0, 0, 0, 1);
            r.quad2D(white, x + w, 0, W - x - w, H, 0, 0, 0, 1);
        }
        if (y > 0.5f) {
            r.quad2D(white, 0, 0, W, y, 0, 0, 0, 1);
            r.quad2D(white, 0, y + h, W, H - y - h, 0, 0, 0, 1);
        }
    }
    r.quad2D(tex_, x, y, w, h, 1, 1, 1, 1, 0, 0, 1, 1);
    r.end2D();
}

// --- the master clock ------------------------------------------------------
// The frame index is a pure function of a real clock, never of a per-call
// increment: a loop that cannot keep up skips frames instead of accumulating
// lag.  While a soundtrack plays, that clock *is* the mixer position (the sound
// the device has taken out of the queue), so the picture is slaved to the sound
// and can never run ahead of it - the reported bug.  Without a usable device the
// clock is SDL_GetTicks, anchored at the moment the first frame went on screen;
// the soundtrack is started at that very instant (audioStart() below), so it
// cannot run ahead during startup either.
static Uint32 queuedDeviceBytes(unsigned dev) {
    return dev ? SDL_GetQueuedAudioSize((SDL_AudioDeviceID)dev) : 0;
}

double Video::masterTime() {
    if (clockOverride_ >= 0.0) return timeSec_ = clockOverride_;
    if (!clockStarted_) return timeSec_;      // 0 until the first frame is up
    if (audioMaster_ && audioDev_ && audioStarted_) {
        const double a = audioTime();
        if (a > lastAudioSec_ + 1e-4) {       // the mixer is still moving
            lastAudioSec_ = a;
            lastAudioMs_ = SDL_GetTicks();
        }
        const bool drained = wavSent_ >= wavLen_ && queuedDeviceBytes(audioDev_) == 0;
        const bool stalled = (uint32_t)(SDL_GetTicks() - lastAudioMs_) > 1000;
        if (!drained && !stalled) return timeSec_ = a;
        // The sound is over (end of the track) or the device stopped consuming
        // (a dummy/offline backend): continue on the wall clock from the point
        // where the mixer stopped, so a silent device can never freeze a movie.
        audioMaster_ = false;
        clockBaseSec_ = a;
        clockAnchorMs_ = SDL_GetTicks();
        if (stalled)
            LOG("video: mixer stopped advancing at %.3f s - picture continues on the wall clock", a);
    }
    return timeSec_ = clockBaseSec_ +
                      (double)(uint32_t)(SDL_GetTicks() - clockAnchorMs_) / 1000.0;
}

bool Video::show(Renderer& r, size_t index) {
    if (loadedFrame_ != (long long)index || rgba_.empty()) {
        if (!loadFrame(index)) {
            LOG("video: frame %zu failed to decode", index);
            return false;
        }
        upload(r);
    } else if (!tex_) {
        upload(r);
    }
    draw(r);
    return true;
}

bool Video::tick(Renderer& r) {
    if (!playing_ || !file_) return false;
    if (clip_.fps <= 0 || clip_.frames <= 0) { playing_ = false; return false; }

    const double t = masterTime();
    const size_t want = (size_t)(t * (double)clip_.fps);   // clock -> frame index
    if (want >= (size_t)clip_.frames) {                    // end of the clip
        playing_ = false;
        frame_ = (size_t)clip_.frames - 1;
        audioClose();
        return false;
    }
    frame_ = want;
    if (!show(r, want)) { playing_ = false; return false; }

    if (!clockStarted_) {
        // The first frame is on screen: start the clock and the soundtrack
        // together.  Everything frame 0 paid for (decode, texture upload) is
        // behind us, so neither can lag the other.
        clockStarted_ = true;
        clockBaseSec_ = 0.0;
        clockAnchorMs_ = SDL_GetTicks();
    }
    if (audioDev_ && !audioStarted_)      audioStart(timeSec_);  // frame 0: offset 0
    else if (audioStarted_)               audioPump(timeSec_ + AUDIO_AHEAD);

    if (clockOverride_ < 0.0 && timeSec_ >= nextSyncLog_) {  // headless drift line
        nextSyncLog_ = timeSec_ + SYNC_LOG_EVERY;
        if (audioRunning())
            LOG("video: sync t=%.1fs frame %zu/%d, mixer %.3f s (video-mixer %+.0f ms)",
                timeSec_, frame_, clip_.frames, audioTime(), (timeSec_ - audioTime()) * 1000.0);
        else
            LOG("video: sync t=%.1fs frame %zu/%d, no soundtrack (wall clock)",
                timeSec_, frame_, clip_.frames);
    }
    return true;
}

bool Video::update(Renderer& r, float dt) {
    if (dt > 0.0f) {                 // deterministic stepping (tools/video_check.cpp)
        const double base = (clockOverride_ >= 0.0) ? clockOverride_ : timeSec_;
        clockOverride_ = base + (double)dt;
    }
    return tick(r);
}

void Video::present(Renderer& r) { draw(r); }

bool Video::seekFrame(Renderer& r, size_t index) {
    if (!file_ || clip_.fps <= 0 || clip_.frames <= 0) return false;
    if (index >= (size_t)clip_.frames) index = (size_t)clip_.frames - 1;
    if (!loadFrame(index)) {
        LOG("video: frame %zu failed to decode", index);
        return false;
    }
    frame_ = index;
    playing_ = true;                 // a seek resumes playback at that frame
    const double secs = (double)index / (double)clip_.fps;
    clockOverride_ = -1.0;
    clockStarted_ = true;
    clockBaseSec_ = secs;
    clockAnchorMs_ = SDL_GetTicks();
    timeSec_ = secs;
    nextSyncLog_ = secs + SYNC_LOG_EVERY;
    if (audioStarted_) audioSeekTo(secs);
    upload(r);
    draw(r);
    return true;
}

bool Video::seek(Renderer& r, double seconds) {
    if (!file_ || clip_.fps <= 0 || clip_.frames <= 0) return false;
    const double last = (double)clip_.frames / (double)clip_.fps -
                        0.5 / (double)clip_.fps;
    if (!(seconds > 0.0)) seconds = 0.0;     // also catches NaN
    if (seconds > last) seconds = last;
    if (seconds < 0.0) seconds = 0.0;
    size_t idx = (size_t)(seconds * (double)clip_.fps + 1e-6);
    if (idx >= (size_t)clip_.frames) idx = (size_t)clip_.frames - 1;
    return seekFrame(r, idx);
}

// --- audio -----------------------------------------------------------------
double Video::wavBytesPerSec() const {
    if (wavFreq_ <= 0 || wavChannels_ <= 0 || wavFrameBytes_ <= 0) return 0.0;
    return (double)wavFreq_ * (double)wavFrameBytes_;
}

// How much of the soundtrack the device has already taken out of the queue, in
// seconds of the source track.  This is the "mixer position" the sync report
// compares the picture against; when it trails the video by less than a frame
// the two are in sync.  0 while no soundtrack is playing.
double Video::audioTime() const {
    if (!audioDev_ || !audioStarted_ || devFreq_ <= 0 || devFrameBytes_ <= 0) return 0.0;
    const Uint32 q = SDL_GetQueuedAudioSize((SDL_AudioDeviceID)audioDev_);
    const double consumed =
        (queuedBytes_ > (uint64_t)q) ? (double)(queuedBytes_ - (uint64_t)q) : 0.0;
    return audioBaseSec_ + consumed / ((double)devFreq_ * (double)devFrameBytes_);
}

double Video::audioDuration() const {
    const double bps = wavBytesPerSec();
    return bps > 0.0 ? (double)wavLen_ / bps : 0.0;
}

bool Video::audioRunning() const {
    if (!audioDev_ || !audioStarted_) return false;
    return !(wavSent_ >= wavLen_ &&
             SDL_GetQueuedAudioSize((SDL_AudioDeviceID)audioDev_) == 0);
}

void Video::audioOpen() {
    if (!audioWanted_ || audioDev_ || !file_ || clip_.audio.empty()) return;
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0 && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        LOG("video: SDL audio subsystem unavailable (%s)", SDL_GetError());
        return;
    }
    SDL_AudioSpec want, have;
    std::memset(&want, 0, sizeof(want));
    std::memset(&have, 0, sizeof(have));
    if (!SDL_LoadWAV(clip_.audio.c_str(), &want, &wavData_, &wavLen_) || !wavData_ || !wavLen_) {
        LOG("video: cannot read %s", clip_.audio.c_str());
        wavData_ = nullptr;
        wavLen_ = 0;
        return;
    }
    // A small device buffer keeps the video/mixer offset (the mixer trails by
    // roughly one buffer) well inside a frame at 12 fps.
    if (want.samples == 0 || want.samples > 2048) want.samples = 1024;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!dev) dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_ANY_CHANGE);
    if (!dev) {
        LOG("video: no audio device (%s) - playing silently", SDL_GetError());
        SDL_FreeWAV(wavData_);
        wavData_ = nullptr;
        wavLen_ = 0;
        return;
    }
    audioDev_ = (unsigned)dev;
    wavFreq_ = want.freq;
    wavChannels_ = want.channels;
    wavFrameBytes_ = SDL_AUDIO_BITSIZE(want.format) / 8 * want.channels;
    if (wavFrameBytes_ <= 0) wavFrameBytes_ = 2;
    devFreq_ = have.freq;
    devChannels_ = have.channels;
    devFrameBytes_ = SDL_AUDIO_BITSIZE(have.format) / 8 * have.channels;
    if (devFrameBytes_ <= 0) devFrameBytes_ = 2;
    if (want.format != have.format || want.freq != have.freq || want.channels != have.channels) {
        audioStream_ = SDL_NewAudioStream(want.format, want.channels, want.freq,
                                          have.format, have.channels, have.freq);
        if (!audioStream_) LOG("video: audio conversion unavailable (%s)", SDL_GetError());
    }
    // left paused on purpose: tick() unpauses it together with the clock
    LOG("video: audio %s (%d Hz %dch -> device %d Hz %dch, %d samples)%s",
        clip_.audio.c_str(), want.freq, want.channels, have.freq, have.channels,
        have.samples, audioStream_ ? ", converted" : "");
}

// Unpause the device and anchor the mixer position at `fromSec`.  Called from
// tick() the moment the first frame is on screen; a later seek re-anchors it
// through audioSeekTo() from seekFrame().
void Video::audioStart(double fromSec) {
    if (!audioDev_ || wavLen_ == 0) return;
    audioStarted_ = true;
    audioMaster_ = true;              // from here on the mixer position is the clock
    audioSeekTo(fromSec);
    SDL_PauseAudioDevice((SDL_AudioDeviceID)audioDev_, 0);
    LOG("video: soundtrack starts with frame %zu (t=%.3f s, %.1f s long)", frame_, fromSec,
        audioDuration());
}

void Video::audioSeekTo(double fromSec) {
    if (!audioDev_ || !wavData_ || wavLen_ == 0) return;
    SDL_ClearQueuedAudio((SDL_AudioDeviceID)audioDev_);
    const double bps = wavBytesPerSec();
    double off = bps > 0.0 ? fromSec * bps : 0.0;
    if (!(off > 0.0)) off = 0.0;
    if (off > (double)wavLen_) off = (double)wavLen_;
    wavSent_ = (uint32_t)off;
    audioBaseSec_ = bps > 0.0 ? (double)wavSent_ / bps : fromSec;
    queuedBytes_ = 0;
    lastAudioSec_ = audioBaseSec_;
    lastAudioMs_ = SDL_GetTicks();
    audioPump(audioBaseSec_ + AUDIO_AHEAD);
}

void Video::audioPump(double upto) {
    if (!audioDev_ || !wavData_ || wavLen_ == 0) return;
    const double bps = wavBytesPerSec();
    if (bps <= 0.0) return;
    int guard = 0;
    while (wavSent_ < wavLen_ && (double)wavSent_ / bps < upto && guard++ < 32) {
        Uint32 n = wavLen_ - wavSent_;
        if (n > 16384) n = 16384;
        if (audioStream_) {
            SDL_AudioStream* st = (SDL_AudioStream*)audioStream_;
            if (SDL_AudioStreamPut(st, wavData_ + wavSent_, (int)n) != 0) break;
            streamFlushed_ = false;
            uint8_t buf[16384];
            int got;
            while ((got = SDL_AudioStreamGet(st, buf, (int)sizeof(buf))) > 0) {
                if (SDL_QueueAudio((SDL_AudioDeviceID)audioDev_, buf, (Uint32)got) != 0) break;
                queuedBytes_ += (uint32_t)got;
            }
        } else {
            if (SDL_QueueAudio((SDL_AudioDeviceID)audioDev_, wavData_ + wavSent_, n) != 0) break;
            queuedBytes_ += n;
        }
        wavSent_ += n;
    }
    if (wavSent_ >= wavLen_ && audioStream_ && !streamFlushed_) {  // flush the resampler tail
        SDL_AudioStream* st = (SDL_AudioStream*)audioStream_;
        streamFlushed_ = true;
        if (SDL_AudioStreamFlush(st) == 0) {
            uint8_t buf[16384];
            int got;
            while ((got = SDL_AudioStreamGet(st, buf, (int)sizeof(buf))) > 0) {
                if (SDL_QueueAudio((SDL_AudioDeviceID)audioDev_, buf, (Uint32)got) != 0) break;
                queuedBytes_ += (uint32_t)got;
            }
        }
    }
}

void Video::audioClose() {
    if (audioDev_) {
        SDL_ClearQueuedAudio((SDL_AudioDeviceID)audioDev_);
        SDL_CloseAudioDevice((SDL_AudioDeviceID)audioDev_);
        audioDev_ = 0;
    }
    if (audioStream_) {
        SDL_FreeAudioStream((SDL_AudioStream*)audioStream_);
        audioStream_ = nullptr;
    }
    if (wavData_) {
        SDL_FreeWAV(wavData_);
        wavData_ = nullptr;
    }
    audioStarted_ = false;
    audioMaster_ = false;
    streamFlushed_ = false;
    wavLen_ = wavSent_ = 0;
    wavFreq_ = wavChannels_ = devFreq_ = devChannels_ = 0;
    wavFrameBytes_ = devFrameBytes_ = 2;
    queuedBytes_ = 0;
    audioBaseSec_ = 0.0;
    lastAudioSec_ = 0.0;
    lastAudioMs_ = 0;
}

} // namespace ht2

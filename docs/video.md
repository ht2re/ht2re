# Movies and the loading screen

*Workstream: `src/ui/Video.{h,cpp}`, `src/ui/Loading.{h,cpp}`, `tools/make_video.sh`,
`tools/rvid_pack.cpp`, `tools/video_probe.py`, `tools/video_check.cpp`.*

## 1. Summary

The two shipped movies are **Indeo Video 5** (`original/kotr.avi`, `original/kotrDemo.avi`),
which no portable library decodes (Windows had a proprietary IV50 codec). ffmpeg *can*
decode them, so the movies are converted **once, at build time**, into a
decoder-free frame pack that the engine streams from disk:

| clip | source | pack | frames | fps | duration | size | wall clock |
|---|---|---|---|---|---|---|---|
| `kotr` (intro) | 640x480, indeo5, 30.5 MB | `build/media/kotr.rvid` 512x384 | 1308 | 12 | 109.0 s | **312.5 MB** | ~1.1 s |
| `kotrDemo` (gameplay) | 640x440, indeo5, 38.5 MB | `build/media/kotrDemo.rvid` 512x352 | 1848 | 12 | 154.0 s | **647.3 MB** | ~1.6 s |
| `kotr` audio | pcm_s16le 22050 Hz mono | `build/media/kotr.wav` | – | – | 109.0 s | 4.8 MB | |
| `kotrDemo` audio | pcm_s16le 22050 Hz mono | `build/media/kotrDemo.wav` | – | – | 154.0 s | 6.8 MB | |

Both clips decode as real pictures: `tools/video_probe.py` dumps an arbitrary frame to a
PNG and reports luminance statistics, and `tools/build_video_check.sh` streams 900 consecutive
frames through the real player into `tools/out/video_frame900.png`.

Picture and sound are locked together: the frame index is derived from a clock (the mixer
position while the WAV plays, `SDL_GetTicks` otherwise) and the soundtrack is started with
the first frame on screen, never by `play()`, so the sound cannot run ahead of the intro
(section 6, "the frame index is a function of a clock"). `--movie-sync-report` measures
the drift headlessly (section 7).

## 2. Why a frame pack instead of the AVI

* Indeo 5 is not decodable without the original proprietary codec; there is no portable
  decoder to link against.
* The alternative would be to ship ffmpeg (or shell out to it) at runtime, which is a
  heavy dependency for a 109 s intro and would make the engine depend on an external
  binary being installed.
* A frame pack is exactly what a 12 fps movie needs anyway: 12 frame reads per second,
  one texture upload per frame, no codec state.

## 3. Conversion pipeline (`tools/make_video.sh`)

```
$ tools/make_video.sh                    # both clips, 512 px wide (default)
$ tools/make_video.sh --native           # original 640 px resolution
$ tools/make_video.sh --width 384        # smaller
$ tools/make_video.sh --fps 12 --raw     # explicit rate, RLE off
$ tools/make_video.sh --force kotr       # ignore the up-to-date check
```

The exact command lines it runs for the default (`<raw>` is `build/media/.<clip>.raw565`,
deleted afterwards):

```sh
# 1) video -> raw RGB565 frames, top-down, one file per source frame
/opt/homebrew/bin/ffmpeg -v error -y -i original/kotr.avi \
    -vf scale=512:384:flags=lanczos -fps_mode passthrough -an -sn \
    -pix_fmt rgb565le -f rawvideo build/media/.kotr.raw565

# 2) raw RGB565 -> RLE coded container (tools/rvid_pack.cpp, compiled on demand)
tools/out/rvid_pack 512 384 12 build/media/.kotr.raw565 build/media/kotr.rvid

# 3) audio track -> WAV (the AVI already carries pcm_s16le, so this is a remux)
/opt/homebrew/bin/ffmpeg -v error -y -i original/kotr.avi \
    -vn -sn -acodec pcm_s16le -f wav build/media/kotr.wav
```

Notes:

* `-vf scale=...` is only added when the target differs from the source, so `--native`
  does not resample at all (byte-exact RGB565 of the decoded frame).
* `-fps_mode passthrough` guarantees exactly one output frame per source frame; without
  it ffmpeg may duplicate/drop frames to reach a constant rate and the pack's frame count
  would no longer match the AVI.
* `--fps N` switches to `-r N -fps_mode cfr` instead.
* The script is **idempotent**: it skips a clip when both outputs are newer than the AVI
  *and* `build/media/<clip>.rvid.cmd` records the same parameters, so changing `--width`
  rebuilds instead of silently keeping the old geometry.
* ffmpeg is looked up on `PATH`, then `/opt/homebrew/bin`, then `/usr/local/bin`;
  `FFMPEG=/path/to/ffmpeg` overrides.

## 4. Container: `.rvid` (version 1, little endian)

Refined from the suggested "magic + header + raw RGB565 frames": the frames are RLE
coded, with a per-frame fallback to raw, and the frame table is explicit so the player can
seek/stream in any order. **Everything is still decoder-free** - the decompressor is 20
lines in `Video::loadFrame()` and 25 lines in `tools/video_probe.py`, and it is
independent of ffmpeg.

| offset | size | field |
|---|---|---|
| 0 | 4 | magic `RVID` |
| 4 | 4 | version = 1 |
| 8 | 4 | width |
| 12 | 4 | height |
| 16 | 4 | fps |
| 20 | 4 | frameCount |
| 24 | 4 | flags: bit0 = 1 → frames are RLE coded, 0 → all frames raw |
| 28 | 4 | tableOffset = 32 |
| 32 | 4·frameCount | size table: one u32 per frame, **bit31 set = this frame is stored raw** |
| 32+4N | … | frame payloads, back to back, in display order |

A frame payload is one of

* **raw**: `width*height` RGB565 pixels, row-major **top-down**, little-endian 16-bit
  values with R in bits 15..11, G in 10..5, B in 4..0 (this is ffmpeg's `rgb565le`, and
  it is also the layout GL expects for `GL_UNSIGNED_SHORT_5_6_5`); or
* **RLE**: a PackBits-style byte stream over those 16-bit pixels:
  * `control < 0x80` → literal run of `control+1` pixels, `2*(control+1)` bytes follow;
  * `control >= 0x80` → repeat run of `(control & 0x7F) + 2` pixels, one u16 follows.

  A frame decodes to exactly `width*height` pixels. The encoder (`tools/rvid_pack.cpp`)
  always stores whichever of RLE/raw is smaller, so a noisy frame costs raw size + 0 and
  can never blow up. The whole file must satisfy
  `32 + 4*frameCount + Σ(sizes & 0x7FFFFFFF) == filesize`; both the player and the probe
  verify this and reject the pack otherwise.

No palette, no per-frame pixels-per-row padding, and no endianness ambiguity: the
container is written and read on the same little-endian hosts the engine targets
(x86-64, arm64).

## 5. Size vs fidelity

Measured on the reference machine (Apple silicon, ffmpeg 9, RLE on, 12 fps):

| pack width | `kotr` (1308 f) | `kotrDemo` (1848 f) | total | note |
|---|---|---|---|---|
| 640 (native) | 484.8 MB | 1005.7 MB | 1.49 GB | no rescale at all |
| **512 (default)** | **312.5 MB** | **647.3 MB** | **0.96 GB** | 4:3 preserved, 2.5x/1.8x upscale to 1280x720 |
| 384 | 176.6 MB | 365.4 MB | 0.54 GB | visibly soft at 720p |

Why 512 is the default:

* Raw RGB565 at the native 640x480 would be 803 MB for the intro alone (1.8 GB for both
  clips); the whole point of the pack is to stay well under that.
* RLE buys **1.65x** on the intro but only **1.03x** on `kotrDemo` - that clip is a
  cockpit recording whose dithered gradients and moving scenery leave almost no runs
  (measured: 544 KB/frame RLE vs 563 KB/frame raw at 640x440). Resolution is therefore the
  only lever that works on both clips, which is why the default scales instead of
  promising compression that is not there.
* 512x384 is the size the original brief suggested; it keeps the total under 1 GB and the
  movie is already a grainy 12 fps 2002 encode, so the difference to 640 at 720p output is
  smaller than the 2x disk cost suggests. Use `--native` when fidelity matters more.
* `--raw` skips the RLE step: verified at 512x384 it produces 514.3 MB (1308 raw frames,
  1.00x) and the player/probe accept it unchanged - the flag is only a fallback for
  debugging the container.

Things that were measured and rejected:

* **XOR-delta + RLE** (frame *i* XOR frame *i-1*, then RLE): 1.16x on the demo, 1.50-2.10x
  on the intro - not enough to justify a second decoder path.
* **8-bit palette (`pal8`) + RLE**: halves the byte count (demo 265 KB/frame instead of
  544 KB) but adds dithering that both hurts the picture and *reduces* the RLE ratio
  (1.06x); a 256-colour movie also shimmers in motion. Not worth it.

Audio is stored as the original PCM s16le WAV (22050 Hz mono), so it needs no codec either.

## 6. Engine side

### `Video` (`src/ui/Video.h`)

```cpp
struct Clip { std::string video, audio; int w, h, fps, frames; uint64_t packedBytes, rawBytes; };

bool open(const std::string& dataDir, const std::string& name);  // finds the pack
void close();
bool ready() const;  const Clip& clip() const;
bool playing() const;
void play(bool withAudio = true);   // resets the clock, opens the WAV **paused**
void stop();
bool tick(Renderer& r);              // real step: frame = floor(clock*fps)
bool update(Renderer& r, float dt);  // deterministic stepping (tools/video_check.cpp)
void present(Renderer& r);           // re-draw the current frame, no clock read
bool seek(Renderer& r, double seconds);      // deterministic seek (moves the audio)
bool seekFrame(Renderer& r, size_t index);   // the same, by frame index
void skip();                        // stops playback (also clears the audio queue)
size_t frame() const;
// extras
float elapsed() const;  float duration() const;
double videoTime() const;  double audioTime() const;  // the A/V drift measurement
double audioDuration() const;  bool audioRunning() const;
bool loadFrame(size_t index);       // decode without GL; rgba() then holds the frame
const std::vector<uint8_t>& rgba() const;
void setAudioEnabled(bool);  bool audioReady() const;
void setClockOverride(double);      // test hook: force videoTime(), < 0 = real clock
void dstRect(const Renderer& r, float& x, float& y, float& w, float& h) const;
void releaseTexture(Renderer& r);
```

* `open()` looks for `<dataDir>/<name>.rvid`, `<dataDir>/VIDEO/…`, `<dataDir>/media/…`,
  `media/…`, `build/media/…`, and finally next to the executable
  (`<exedir>/media/…`, `<exedir>/../build/media/…`, via `SDL_GetBasePath()`), so the game
  finds the packs no matter which directory it was started from; a `name` may carry the
  `.rvid` extension. It returns `false` - and logs one line - when nothing is found or the
  header/table is inconsistent (bad magic, bad geometry, a frame size outside
  `1..w*h*2`, or a size table that does not add up to the file size).
* **The frame index is a function of a clock, never of a per-call `dt` sum.** `tick()`
  reads the master clock and decodes `floor(clock*fps)`; if the loop is late it *skips*
  frames, it never falls behind by accumulating them (the old `update(dt)` accumulated a
  `dt` that `App::run()` clamped to 0.1 s, so a loop slower than 20 fps made the picture
  fall behind the sound, and - because the same `update()` ran twice per loop iteration -
  a fast loop played the picture at double speed). It returns `false` when the clip ends or
  was skipped, `true` while the movie is on screen; without an open clip it is a no-op that
  returns `false`.
* **The master clock is the mixer position while a soundtrack plays**, and `SDL_GetTicks`
  (anchored at the frame that actually went on screen) otherwise. The picture is therefore
  *slaved to the sound*: it can never run ahead of it, and the drift cannot grow with time.
  `play()` only opens the WAV and the device **paused**; `tick()` unpauses them at the
  moment the first frame is up (and, after `seek()`, at the matching offset), so the
  soundtrack cannot lead the picture while the intro is still decoding. If the mixer stops
  advancing for a second (a device that never consumes) or the track runs out, the clock
  continues on the wall clock from where the sound stopped - a missing or dead soundtrack
  can never freeze the movie.
* `seek()`/`seekFrame()` are the deterministic path used by `--movie-time`,
  `--movie-frames` and the sync report: the frame is decoded directly (no stepping loop),
  the clock is put at `index/fps` and a playing soundtrack is re-filled from the matching
  byte offset (`SDL_ClearQueuedAudio` + `SDL_QueueAudio` from `t*bytesPerSec`).
* `update(r, dt)` is kept for the API harness (`tools/video_check.cpp`): a positive `dt`
  advances a synthetic clock by hand, frame by frame, with no wall clock. The game itself
  always calls `tick()` (and `present()` when it only has to re-draw, e.g. the
  screenshot paths).
* The frame is uploaded into **one** texture created from the first frame with
  `Renderer::createTexture()`; later frames go through `glTexSubImage2D` on that texture
  (one `GL_LINEAR` min filter is set once, because `createTexture()` leaves mipmaps that
  would ghost through when the movie is drawn smaller than native). A frame that is already
  resident is *not* re-uploaded, so the loop may tick faster than the movie's frame rate for
  free. The GL entry points
  (`glBindTexture`, `glTexSubImage2D`, `glTexParameteri`, `glDeleteTextures`) are resolved
  locally with `SDL_GL_GetProcAddress`, because `Renderer` is owned by another workstream
  and must not be edited here. If `glTexSubImage2D` is missing the player falls back to
  rebuilding the texture.
* Audio uses SDL core only (`SDL_LoadWAV` + `SDL_OpenAudioDevice` + `SDL_QueueAudio`),
  with an `SDL_AudioStream` if the device cannot take the file's format. The device buffer
  is pinned to 1024 samples (≈46 ms at 22050 Hz - the residual offset between the mixer
  position and what is audible), the queue is kept ~0.75 s ahead of the mixer position, and
  every failure path (no device, no WAV, subsystem off) degrades to silent playback on the
  wall clock.

### `Loading` (`src/ui/Loading.h`)

```cpp
void begin(const std::string& what);                  // heading, "" -> "ЗАГРУЗКА"
void step(int done, int total, const std::string& item);  // total <= 0 = indeterminate
void draw(Renderer& r, Font& font);                   // safe at any time
void end();
bool active() const;
// extras
void update(float dt);   float progress() const;   void setNote(const std::string&);
static const char* versionString();                   // "v 8.0 30.01.03"
```

Drawn procedurally on the same 800x600 virtual canvas as `Menu` (dark background, amber
rules, framed amber bar, percent label, item line with trailing dots, and the original's
`v 8.0 30.01.03` string plus an `HT2:RE` tag in the corner). `LOAD.RES` turned out to
contain no loading-screen mask (only menu/vehicle/dialog art), so nothing has to be loaded
and the screen works before the world, before `Renderer::init()` (it returns without
drawing if the renderer is not up) and after `end()`.

The percent label switches to dark ink once the fill passes under it, otherwise white on
amber is unreadable.

## 7. Verification

```sh
$ tools/make_video.sh                     # produce build/media/*.rvid + *.wav
$ python3 tools/video_probe.py build/media/kotr.rvid        # header/size/decode check
$ python3 tools/video_probe.py build/media/kotr.rvid --decode-all   # every frame
$ tools/build_video_check.sh              # off-screen render + API behaviour
$ SDL_AUDIODRIVER=dummy build/ht2re --movie-sync-report                  # A/V drift
$ SDL_AUDIODRIVER=dummy build/ht2re --movie-sync-report --movie-sync-seconds 110
$ build/ht2re --movie-shot tools/out/movie400.bmp --movie-frames 400      # one frame
```

`video_probe.py` parses the header, checks that the size table accounts for exactly the
file size, decodes sample frames (or all of them with `--decode-all`), prints per-frame
luminance/colour statistics and writes one real frame to a PNG. `build_video_check.sh`
links `Renderer`/`Font`/`Video`/`Loading` into a small off-screen harness that renders the
loading screen, streams 900 frames of each clip, and exercises the failure paths (missing
pack, bad frame index, drawing before `begin()`, skip, end of clip) - it writes
`tools/out/loading*.bmp` and `tools/out/video_frame900.bmp`.

`--movie-sync-report` is the headless A/V-drift measurement for the "the soundtrack leads
the picture" bug. It runs in two phases and exits non-zero if phase 1 fails:

1. **Deterministic clock check** (no wall clock, no audio device):
   `Video::setClockOverride()` feeds the player a synthetic clock at 0/30/60/100 s and
   through a deliberately stuttering loop (a 0.75 s step every 1.5 s). The line

   ```
   movie-sync[clock]: clock=100.000s frame=1200 expected=1200 (+0 frames) ok: ...
   ```

   must come out with `frame == floor(clock*fps)` at every checkpoint: the picture follows
   the clock, and the stall *skips* frames instead of accumulating lag.
2. **Real playback** for `--movie-sync-seconds` (default 4, `SDL_AUDIODRIVER=dummy` keeps
   time), printing video time against the mixer position - the value
   `SDL_GetQueuedAudioSize()` implies the device has consumed - once per second:

   ```
   movie-sync: video=1.951s audio=1.951s drift=+0ms frame=23/1308 mixer=running
   ```

   and then seeking to 30/60/100 s (`movie-sync[seek@100s]: video=100.233s audio=100.233s
   drift=+0ms frame=1202/1308 mixer=running`) so the far end of the clip is measured
   without waiting 100 s. `drift` is `video - mixer`; it is bounded by the device buffer
   (≤ ~46 ms) and cannot grow, because while the soundtrack plays the mixer position *is*
   the video clock.

`tools/shot_test.py` asserts the 0/30/60/100 s clock checkpoints, the "soundtrack starts
with frame 0" line, `|drift| <= 1/12 s` for every measured sample including the 30/60/100 s
seeks, and that the playback really advanced (frames 0 → ≥24).

The harness also writes its own `.rvid` from an **independent** implementation of the
format in section 4 (repeat *and* literal runs, plus a raw-flagged frame) and checks that
`Video::loadFrame()` reproduces those pixels bit-exactly after the RGB565→RGBA expansion;
a truncated copy of the same pack must be refused. That is what pins the C++ decoder and
`tools/rvid_pack.cpp` to the documented layout - the real clips only prove they agree with
each other.

## 8. What the parent still has to do

1. Call `tools/make_video.sh` once after checkout (or wire it into the build); the packs
   live in `build/media/`, never in the repository root. **`build/media/` disappears
   whenever the build directory is wiped** - regenerating both clips takes ~3 s, so the
   cheapest wiring is a CMake target:

   ```cmake
   # CMakeLists.txt (parent owns this file)
   add_custom_command(
       OUTPUT ${CMAKE_BINARY_DIR}/media/kotr.rvid ${CMAKE_BINARY_DIR}/media/kotr.wav
       COMMAND ${CMAKE_SOURCE_DIR}/tools/make_video.sh
       DEPENDS ${CMAKE_SOURCE_DIR}/tools/make_video.sh
               ${CMAKE_SOURCE_DIR}/tools/rvid_pack.cpp
               ${CMAKE_SOURCE_DIR}/original/kotr.avi)
   add_custom_target(media ALL DEPENDS ${CMAKE_BINARY_DIR}/media/kotr.rvid)
   # binary ends up in <root>/build/, so no CWD assumption is needed: Video::open()
   # also searches <exedir>/media/ and <exedir>/../build/media/.
   ```
2. Wire the state machine, e.g.

   ```cpp
   if (video_.open(opt_.dataDir, "kotr")) { video_.play(/*withAudio=*/true); state_ = State::Movie; }
   ...
   case State::Movie:
       if (!video_.tick(r_)) { video_.stop(); state_ = State::Menu; }
       break;
   ```
   `tick()` owns the A/V clock, so the loop must call it exactly once per frame (`App::frame()`
   only calls `present()` for the movie, to re-draw the current frame for screenshots).
   `Loading` is used the same way: `lg.begin("ЗАГРУЗКА МИРА")`, then
   `lg.step(i, n, name)` from the tile/way loops with `lg.update(dt); lg.draw(r_, font_);`
   once per frame.
3. Escape/Space should call `video_.skip()`; the player then reports "finished" on the
   next `tick()`.
4. Optional `Renderer` addition - it would remove the last direct GL calls from
   `Video.cpp`:

   ```cpp
   // Renderer.h
   // Re-uploads a texture created with createTexture() from a full RGBA frame
   // (glBindTexture + glTexSubImage2D) and drops its mipmaps so the caller can
   // stream frames into it.  Returns false for an unknown texture.
   bool updateTexture(unsigned tex, const uint8_t* rgba, int w, int h);
   ```

   With that in place `Video::upload()` reduces to `r.updateTexture(tex_, rgba_.data(),
   clip_.w, clip_.h)` and the `SDL_GL_GetProcAddress` block in `Video.cpp` can go away.
   Until then everything works as is.
5. `Video::close()` (and `~Video`) deletes its texture only when a GL context is still
   current; call `video_.releaseTexture(r_)` before `Renderer::shutdown()` if the player
   outlives the window.

## 9. Known limitations

* The source AVIs carry their own black letterbox bars inside the 640x480 / 640x440
  frame, so the picture shows those bars *plus* the ones the player adds for the window
  aspect. That is faithful to the original; cropping would have to be a separate decision.
* Frame-accurate seeking is by frame index only (no keyframe logic needed - every frame is
  independent).
* Audio is not resampled when the device refuses 22050 Hz mono *and* `SDL_AudioStream`
  cannot be created; in that (unlikely) case playback is silent rather than desynchronised.
* While a soundtrack plays, the picture is slaved to the **mixer position**, which is the
  point the device has taken data out of the queue - one device buffer (≈46 ms at the
  pinned 1024 samples / 22050 Hz) ahead of what is audible. The picture can therefore lead
  the *heard* sound by up to that buffer, i.e. half a frame at 12 fps; a real sound card's
  hardware latency adds a few more milliseconds. This is deliberately in the safe
  direction: the sound can never lead the picture.
* If the device stops consuming for more than a second (or the WAV is shorter than the
  clip), the player continues on `SDL_GetTicks` from the mixer's last position, so the
  remaining picture keeps running - at the price of the two being no longer locked.
* `SDL_AUDIODRIVER=dummy` is only approximately real time (measured: ≈2.5 % slow, because
  the backend paces itself with `SDL_Delay`); headless playback therefore runs slightly
  under real time, while `drift` stays 0 because the picture follows the mixer. On a real
  device the mixer position is the sound-card clock and the movie runs at real time.
* The coding of a pack is fixed at conversion time; a pack built with `--raw` is larger
  but decodes identically - the player reads the per-frame raw flag and never needs to
  know how the pack was made.

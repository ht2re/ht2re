// Unified input: keyboard + SDL game controller (handhelds such as an Anbernic
// expose an XInput-style pad).  See docs/input.md for the mapping table, the
// filter numbers and the hot-plug behaviour, and tools/input_test.cpp for the
// headless harness that exercises every filter below.
//
// Build line for the harness (it is not part of the app target on purpose):
//   c++ -std=c++17 -O2 -I src tools/input_test.cpp src/app/Input.cpp \
//       -o tools/out/input_test $(pkg-config --cflags --libs sdl2)
#include "Input.h"
#include "../util/Log.h"
#include <SDL.h>
#include <cmath>
#include <string>
#include <vector>

namespace ht2 {

// RawPad::buttons must mirror SDL_CONTROLLER_BUTTON_* exactly, because
// readPad() casts PadButton to SDL_GameControllerButton when sampling.
static_assert(static_cast<int>(Input::PAD_A) == static_cast<int>(SDL_CONTROLLER_BUTTON_A), "PadButton A");
static_assert(static_cast<int>(Input::PAD_B) == static_cast<int>(SDL_CONTROLLER_BUTTON_B), "PadButton B");
static_assert(static_cast<int>(Input::PAD_X) == static_cast<int>(SDL_CONTROLLER_BUTTON_X), "PadButton X");
static_assert(static_cast<int>(Input::PAD_Y) == static_cast<int>(SDL_CONTROLLER_BUTTON_Y), "PadButton Y");
static_assert(static_cast<int>(Input::PAD_BACK) == static_cast<int>(SDL_CONTROLLER_BUTTON_BACK), "PadButton Back");
static_assert(static_cast<int>(Input::PAD_START) == static_cast<int>(SDL_CONTROLLER_BUTTON_START), "PadButton Start");
static_assert(static_cast<int>(Input::PAD_LB) == static_cast<int>(SDL_CONTROLLER_BUTTON_LEFTSHOULDER), "PadButton LB");
static_assert(static_cast<int>(Input::PAD_RB) == static_cast<int>(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER), "PadButton RB");
static_assert(static_cast<int>(Input::PAD_DPAD_UP) == static_cast<int>(SDL_CONTROLLER_BUTTON_DPAD_UP), "PadButton D-up");
static_assert(static_cast<int>(Input::PAD_DPAD_RIGHT) == static_cast<int>(SDL_CONTROLLER_BUTTON_DPAD_RIGHT),
              "PadButton D-right");
static_assert(static_cast<int>(Input::PAD_BUTTON_COUNT) <= static_cast<int>(SDL_CONTROLLER_BUTTON_MAX),
              "PadButton count must stay inside the SDL button range");

static const uint32_t kNoRemove = 0xFFFFFFFFu;   // pendingRemove_ "empty" marker
static const int kNoRemap = -1;                  // pendingRemap_ "empty" marker

// ---------------------------------------------------------------------------
// Pure filters
// ---------------------------------------------------------------------------
float Input::Filter::clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

float Input::Filter::applyStick(float raw, float deadZone) {
    const float dz = clampf(deadZone, 0.0f, 0.95f);
    const float a = std::fabs(raw);
    if (a <= dz) return 0.0f;
    // Re-normalise: the travel left after the dead zone is stretched back over
    // 0..1 so the full physical range of the stick is usable.
    const float s = clampf((a - dz) / (1.0f - dz), 0.0f, 1.0f);
    return raw < 0.0f ? -s : s;
}

float Input::Filter::applyTrigger(float raw, float deadZone) {
    // SDL reports triggers as -32768 (released) .. 32767 (pressed), i.e. -1..1.
    const float x = clampf((raw + 1.0f) * 0.5f, 0.0f, 1.0f);
    const float dz = clampf(deadZone, 0.0f, 0.95f);
    if (x <= dz) return 0.0f;
    return clampf((x - dz) / (1.0f - dz), 0.0f, 1.0f);
}

float Input::Filter::shapePedal(float x, float linear) {
    x = clampf(x, 0.0f, 1.0f);
    const float l = clampf(linear, 0.0f, 1.0f);
    return clampf(l * x + (1.0f - l) * x * x, 0.0f, 1.0f);
}

float Input::Filter::shapeSteer(float x, float expo) {
    x = clampf(x, -1.0f, 1.0f);
    const float e = clampf(expo, 0.0f, 1.0f);
    return clampf((1.0f - e) * x + e * x * x * x, -1.0f, 1.0f);
}

float Input::Filter::rateLimit(float current, float target, float dt,
                               float riseRate, float fallRate) {
    if (dt <= 0.0f) return current;
    const float d = target - current;
    if (d == 0.0f) return target;
    const float rate = (std::fabs(target) >= std::fabs(current)) ? riseRate : fallRate;
    if (rate <= 0.0f) return target;             // 0 disables the limiter
    const float step = rate * dt;
    if (std::fabs(d) <= step) return target;
    return current + (d > 0.0f ? step : -step);
}

float Input::Filter::blend(float a, float b) {
    return std::fabs(a) >= std::fabs(b) ? a : b;
}

// ---------------------------------------------------------------------------
// SDL helpers
// ---------------------------------------------------------------------------
static const Uint8* keyboardState() {
    static const Uint8 zeros[SDL_NUM_SCANCODES] = {0};
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0 && SDL_WasInit(SDL_INIT_EVENTS) == 0) return zeros;
    const Uint8* kb = SDL_GetKeyboardState(nullptr);
    return kb ? kb : zeros;
}

static float axisNorm(SDL_GameController* c, SDL_GameControllerAxis a) {
    if (!c) return 0.0f;
    return Input::Filter::clampf(static_cast<float>(SDL_GameControllerGetAxis(c, a)) / 32767.0f,
                                 -1.0f, 1.0f);
}

static bool haveFile(const std::string& p) {
    SDL_RWops* rw = SDL_RWFromFile(p.c_str(), "rb");
    if (!rw) return false;
    SDL_RWclose(rw);
    return true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
Input::~Input() { shutdown(); }

bool Input::init() { return init(std::string()); }

bool Input::init(const std::string& dataDir) {
    if (inited_) return true;

    // --- game controller subsystem (never fatal: keyboard-only is valid) ---
    if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
            LOG("input: SDL_InitSubSystem(GAMECONTROLLER) failed: %s", SDL_GetError());
            LOG("input: running keyboard-only (this is not an error)");
        } else {
            ownControllerSubsystem_ = true;
        }
    }
    if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) != 0) {
        SDL_GameControllerEventState(SDL_ENABLE);   // keep hot-plug events coming
        loadMappings(dataDir);
    }
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0)
        LOG("input: note: the video subsystem is not up yet - keyboard state stays empty until it is");

    if (!watchInstalled_ && SDL_WasInit(SDL_INIT_EVENTS) != 0) {
        SDL_AddEventWatch(&Input::eventWatch, this);
        watchInstalled_ = true;
    }

    inited_ = true;
    pendingAdd_.store(-1);
    pendingRemove_.store(kNoRemove);
    pendingRemap_.store(kNoRemap);
    rescan();

    LOG("input: ready (controller: %s, mapping: %s)",
        pad_ ? padName_.c_str() : "none - keyboard only", mappingSource_.c_str());
    return true;
}

void Input::shutdown() {
    // Deliberately tolerant of being called after SDL_Quit(): every SDL call is
    // guarded with SDL_WasInit so a late destructor can never touch freed state.
    if (watchInstalled_ && SDL_WasInit(SDL_INIT_EVENTS) != 0)
        SDL_DelEventWatch(&Input::eventWatch, this);
    watchInstalled_ = false;

    closePad();

    if (ownControllerSubsystem_ && SDL_WasInit(SDL_INIT_GAMECONTROLLER) != 0)
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    ownControllerSubsystem_ = false;

    inited_ = false;
    injected_ = false;
    injectedPad_ = RawPad();
    steer_ = 0.0f;
    joyCount_ = 0;
    nextOpenMs_ = 0;
    view_.reset(); buy_.reset(); job_.reset(); deliver_.reset(); menu_.reset(); map_.reset();
    kbMapToggle_.reset();
    kbView_.reset(); kbBuy_.reset(); kbJob_.reset(); kbDeliver_.reset(); kbMenu_.reset(); kbMap_.reset();
}

// ---------------------------------------------------------------------------
// Mapping database
// ---------------------------------------------------------------------------
void Input::loadMappings(const std::string& dataDir) {
    std::vector<std::string> cands;
    const char* env = SDL_getenv("HT2_GAMECONTROLLERDB");
    if (env && *env) cands.push_back(env);
    if (!dataDir.empty()) {
        cands.push_back(dataDir + "/gamecontrollerdb.txt");       // inside the data dir
        cands.push_back(dataDir + "/../gamecontrollerdb.txt");    // next to it
    }
    cands.push_back("gamecontrollerdb.txt");                      // cwd / next to the binary
    char* base = SDL_GetBasePath();
    if (base) {
        cands.push_back(std::string(base) + "gamecontrollerdb.txt");
        SDL_free(base);
    }
    // Common Linux/handheld locations for the community DB.
    cands.push_back("/usr/share/gamecontrollerdb/gamecontrollerdb.txt");
    cands.push_back("/usr/share/sdl2-gamecontrollerdb/gamecontrollerdb.txt");

    for (const std::string& p : cands) {
        if (!haveFile(p)) continue;
        const int n = SDL_GameControllerAddMappingsFromFile(p.c_str());
        if (n < 0) {
            LOG("input: %s: %s", p.c_str(), SDL_GetError());
            continue;                       // try the next candidate
        }
        mappingSource_ = p + " (" + std::to_string(n) + " mappings)";
        LOG("input: loaded %d controller mappings from %s", n, p.c_str());
        return;
    }
    mappingSource_ = "SDL built-in database";
    LOG("input: no gamecontrollerdb.txt found - using SDL's built-in mapping database");
}

// ---------------------------------------------------------------------------
// Pad management / hot-plug
// ---------------------------------------------------------------------------
int Input::eventWatch(void* userdata, SDL_Event* e) {
    if (userdata && e) static_cast<Input*>(userdata)->handleEvent(*e);
    return 0;   // 0 = keep the event in the queue for the app
}

void Input::handleEvent(const SDL_Event& e) {
    // Only flags are set here - this may run inside SDL_PumpEvents(), so it must
    // not call into the joystick API.  Having the watch installed *and* the
    // caller forwarding events is harmless: the flags are idempotent.
    switch (e.type) {
        case SDL_CONTROLLERDEVICEADDED:      pendingAdd_.store(e.cdevice.which); break;
        case SDL_CONTROLLERDEVICEREMOVED:    pendingRemove_.store(static_cast<uint32_t>(e.cdevice.which)); break;
        case SDL_CONTROLLERDEVICEREMAPPED:   pendingRemap_.store(e.cdevice.which); break;
        default: break;
    }
}

void Input::onControllerAdded(int deviceIndex) {
    if (!inited_) return;
    LOG("input: hot-plug: controller added (device %d)", deviceIndex);
    nextOpenMs_ = 0;        // try immediately
    rescan();
}

void Input::onControllerRemoved(uint32_t instanceId) {
    if (!inited_) return;
    LOG("input: hot-plug: controller removed (instance %u)", instanceId);
    if (pad_ && static_cast<int>(instanceId) == padInstance_) closePad();
    rescan();
}

void Input::processPendingDevices() {
    if (!inited_) return;
    const int add = pendingAdd_.exchange(-1);
    if (add >= 0) onControllerAdded(add);
    const uint32_t rem = pendingRemove_.exchange(kNoRemove);
    if (rem != kNoRemove) onControllerRemoved(rem);
    const int remap = pendingRemap_.exchange(kNoRemap);
    if (remap >= 0 && pad_ && remap == padInstance_) {
        // The mapping table changed under an open pad: reopen so new bindings apply.
        LOG("input: controller remapped - reopening \"%s\"", padName_.c_str());
        closePad();
        nextOpenMs_ = 0;
        rescan();
    }
}

void Input::rescan() {
    if (!inited_ || SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) return;

    joyCount_ = SDL_NumJoysticks();

    if (pad_) {
        auto* c = static_cast<SDL_GameController*>(pad_);
        if (SDL_GameControllerGetAttached(c)) return;   // still there: nothing to do
        LOG("input: controller detached: \"%s\"", padName_.c_str());
        closePad();
    }

    if (SDL_GetTicks() < nextOpenMs_) return;           // failed-open backoff
    for (int i = 0; i < joyCount_; ++i) {
        if (!SDL_IsGameController(i)) continue;
        openPad(i);
        break;
    }
}

void Input::openPad(int deviceIndex) {
    SDL_GameController* c = SDL_GameControllerOpen(deviceIndex);
    if (!c) {
        LOG("input: SDL_GameControllerOpen(%d) failed: %s", deviceIndex, SDL_GetError());
        nextOpenMs_ = SDL_GetTicks() + 2000;    // do not spam retries every frame
        return;
    }
    pad_ = c;
    nextOpenMs_ = 0;
    SDL_Joystick* js = SDL_GameControllerGetJoystick(c);
    padInstance_ = js ? static_cast<int>(SDL_JoystickInstanceID(js)) : -1;
    const char* nm = SDL_GameControllerName(c);
    padName_ = (nm && *nm) ? nm : "unknown controller";

    hasRumble_ = true;
#if SDL_VERSION_ATLEAST(2, 0, 18)
    if (SDL_GameControllerHasRumble(c) != SDL_TRUE) {
        hasRumble_ = false;
        LOG("input: \"%s\" reports no rumble support", padName_.c_str());
    }
#endif
    rumbleWarned_ = false;
    LOG("input: controller %d: \"%s\" (instance %d, mapping: %s, rumble: %s)",
        deviceIndex, padName_.c_str(), padInstance_, mappingSource_.c_str(),
        hasRumble_ ? "yes" : "no");
}

void Input::closePad() {
    if (!pad_) return;
    auto* c = static_cast<SDL_GameController*>(pad_);
    pad_ = nullptr;
    padInstance_ = -1;
    hasRumble_ = false;
    if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) != 0) SDL_GameControllerClose(c);
    LOG("input: controller closed: \"%s\"", padName_.c_str());
    padName_.clear();
}

// ---------------------------------------------------------------------------
// Rumble
// ---------------------------------------------------------------------------
void Input::rumble(float low, float high, uint32_t ms) {
    auto* c = static_cast<SDL_GameController*>(pad_);
    if (!c) {
        if (!rumbleWarned_) {
            LOG("input: rumble %.2f/%.2f ignored - no controller connected", low, high);
            rumbleWarned_ = true;
        }
        return;
    }
    if (!hasRumble_) {
        if (!rumbleWarned_) {
            LOG("input: rumble not supported by \"%s\" - ignored", padName_.c_str());
            rumbleWarned_ = true;
        }
        return;
    }
    const Uint16 lowRaw = static_cast<Uint16>(Filter::clampf(low, 0.0f, 1.0f) * 65535.0f + 0.5f);
    const Uint16 highRaw = static_cast<Uint16>(Filter::clampf(high, 0.0f, 1.0f) * 65535.0f + 0.5f);
    if (SDL_GameControllerRumble(c, lowRaw, highRaw, ms) != 0) {
        hasRumble_ = false;
        LOG("input: SDL_GameControllerRumble failed (%s) - rumble disabled", SDL_GetError());
    }
}

void Input::stopRumble() {
    auto* c = static_cast<SDL_GameController*>(pad_);
    if (!c || !hasRumble_) return;
    SDL_GameControllerRumble(c, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------
Input::RawPad Input::readPad() const {
    if (injected_) return injectedPad_;

    RawPad r;
    auto* c = static_cast<SDL_GameController*>(pad_);
    if (!c || SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) return r;

    r.connected = true;
    r.lx = axisNorm(c, SDL_CONTROLLER_AXIS_LEFTX);
    r.ly = axisNorm(c, SDL_CONTROLLER_AXIS_LEFTY);
    r.rx = axisNorm(c, SDL_CONTROLLER_AXIS_RIGHTX);
    r.ry = axisNorm(c, SDL_CONTROLLER_AXIS_RIGHTY);
    r.lt = axisNorm(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    r.rt = axisNorm(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    for (uint32_t b = 0; b < static_cast<uint32_t>(PAD_BUTTON_COUNT); ++b)
        if (SDL_GameControllerGetButton(c, static_cast<SDL_GameControllerButton>(b)) != 0)
            r.buttons |= (1u << b);
    return r;
}

void Input::injectRawPad(const RawPad& p) {
    injectedPad_ = p;
    injectedPad_.connected = true;
    injected_ = true;
}

void Input::clearInjectedPad() {
    injected_ = false;
    injectedPad_ = RawPad();
}

// ---------------------------------------------------------------------------
// Per-frame state
// ---------------------------------------------------------------------------
Input::State Input::computeState(const RawPad& raw, float dt) {
    State s;
    const Tuning& t = tuning_;
    const Uint8* kb = keyboardState();

    const float dz = Filter::clampf(t.deadZone, 0.0f, 0.95f);
    const float tdz = Filter::clampf(t.triggerDeadZone, 0.0f, 0.95f);
    dt = Filter::clampf(dt, 0.0f, 0.1f);       // a hitch must not snap the wheel

    auto held = [&](PadButton b) { return (raw.buttons & (1u << static_cast<uint32_t>(b))) != 0; };
    auto key = [&](SDL_Scancode sc) { return kb[sc] != 0; };

    // --- steering: left stick (analog) + keyboard/D-pad (digital) ----------
    const float analogSteer = Filter::applyStick(raw.lx, dz);
    float digiSteer = (key(SDL_SCANCODE_D) || key(SDL_SCANCODE_RIGHT) ? 1.0f : 0.0f)
                    - (key(SDL_SCANCODE_A) || key(SDL_SCANCODE_LEFT) ? 1.0f : 0.0f);
    if (t.dpadDriving)
        digiSteer += (held(PAD_DPAD_RIGHT) ? 1.0f : 0.0f) - (held(PAD_DPAD_LEFT) ? 1.0f : 0.0f);
    digiSteer = Filter::clampf(digiSteer, -1.0f, 1.0f);

    const float steerTarget = Filter::shapeSteer(Filter::blend(analogSteer, digiSteer), t.steerExpo);
    steer_ = Filter::rateLimit(steer_, steerTarget, dt, t.steerRiseRate, t.steerFallRate);
    if (std::fabs(steer_) < 5e-4f) steer_ = 0.0f;
    s.steer = steer_;

    // --- pedals: RT throttle, LT brake, right stick Y alternative ----------
    const float trigThrottle = Filter::shapePedal(Filter::applyTrigger(raw.rt, tdz), t.pedalLinear);
    const float trigBrake = Filter::shapePedal(Filter::applyTrigger(raw.lt, tdz), t.pedalLinear);
    float stickThrottle = 0.0f, stickBrake = 0.0f;
    if (t.stickPedals) {
        stickThrottle = Filter::shapePedal(
            Filter::clampf(Filter::applyStick(-raw.ry, dz), 0.0f, 1.0f), t.pedalLinear);
        stickBrake = Filter::shapePedal(
            Filter::clampf(Filter::applyStick(raw.ry, dz), 0.0f, 1.0f), t.pedalLinear);
    }
    float digiThrottle = (key(SDL_SCANCODE_W) || key(SDL_SCANCODE_UP) ? 1.0f : 0.0f)
                       - (key(SDL_SCANCODE_S) || key(SDL_SCANCODE_DOWN) ? 1.0f : 0.0f);
    if (t.dpadDriving)
        digiThrottle += (held(PAD_DPAD_UP) ? 1.0f : 0.0f) - (held(PAD_DPAD_DOWN) ? 1.0f : 0.0f);
    digiThrottle = Filter::clampf(digiThrottle, -1.0f, 1.0f);

    // Keyboard beats analog when both are active; throttle stays signed so
    // "S"/negative throttle means brake/reverse once the caller does
    // throttle - brake (see the wiring comment in Input.h).
    s.throttle = Filter::clampf(Filter::blend(digiThrottle, Filter::blend(trigThrottle, stickThrottle)),
                                -1.0f, 1.0f);
    s.brake = Filter::clampf(Filter::blend(trigBrake, stickBrake), 0.0f, 1.0f);

    // --- buttons ----------------------------------------------------------
    // Keyboard and pad edges are evaluated separately (both edge objects must
    // advance every frame, even when the other source already fired).
    const bool padView = held(PAD_Y) || held(PAD_RB);
    const bool padBuy = held(PAD_X);
    const bool padJob = held(PAD_B);
    const bool padDeliver = held(PAD_LB);
    const bool padMenu = held(PAD_START);
    const bool padMap = held(PAD_BACK);

    const bool eView = view_.update(padView);
    const bool eKbView = kbView_.update(key(SDL_SCANCODE_C) || key(SDL_SCANCODE_V));
    const bool eBuy = buy_.update(padBuy);
    const bool eKbBuy = kbBuy_.update(key(SDL_SCANCODE_B));
    const bool eJob = job_.update(padJob);
    const bool eKbJob = kbJob_.update(key(SDL_SCANCODE_T));
    const bool eDeliver = deliver_.update(padDeliver);
    const bool eKbDeliver = kbDeliver_.update(key(SDL_SCANCODE_Y));
    const bool eMenu = menu_.update(padMenu);
    const bool eKbMenu = kbMenu_.update(key(SDL_SCANCODE_ESCAPE));
    const bool eMap = map_.update(padMap);
    const bool eKbMap = kbMap_.update(key(SDL_SCANCODE_TAB));

    s.viewToggle = eView || eKbView;
    s.buy = eBuy || eKbBuy;
    s.job = eJob || eKbJob;
    s.deliver = eDeliver || eKbDeliver;
    s.menu = eMenu || eKbMenu;
    s.map = eMap || eKbMap;
    // M toggles the original's road-map overlay (the skip/service edge above
    // stays on Tab / Back so the panel keys keep working).
    s.mapToggle = kbMapToggle_.update(key(SDL_SCANCODE_M));
    s.handbrake = held(PAD_A) || key(SDL_SCANCODE_SPACE);

    // --- "is the player doing anything at all" (menus / attract mode) ------
    bool any = raw.buttons != 0 || s.throttle != 0.0f || s.steer != 0.0f || s.brake != 0.0f ||
               s.handbrake || s.viewToggle || s.buy || s.job || s.deliver || s.menu ||
               s.map || s.mapToggle;
    const float eps = 0.05f;
    if (!any)
        any = std::fabs(raw.lx) > eps || std::fabs(raw.ly) > eps ||
              std::fabs(raw.rx) > eps || std::fabs(raw.ry) > eps ||
              raw.lt > -1.0f + eps || raw.rt > -1.0f + eps;
    if (!any)
        for (int i = 0; i < SDL_NUM_SCANCODES; ++i)
            if (kb[i]) { any = true; break; }
    s.any = any;

    return s;
}

Input::State Input::poll(float dt) {
    processPendingDevices();
    if (!injected_) rescan();
    return computeState(readPad(), dt);
}

void Input::setDeadZone(float dz) {
    tuning_.deadZone = Filter::clampf(dz, 0.0f, 0.95f);
}

} // namespace ht2

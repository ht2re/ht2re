#pragma once
#include <atomic>
#include <cstdint>
#include <string>

// Forward declaration only: this header deliberately does not include the SDL
// headers, so tools/input_test.cpp can exercise the filters and the whole poll
// path (through injectRawPad) without linking anything but SDL itself.
union SDL_Event;

namespace ht2 {

// Unified input: keyboard + SDL game controller (handhelds such as an Anbernic
// expose an XInput-style pad: two sticks, D-pad, ABXY, LB/RB, analog LT/RT,
// Start/Select).  Analog steering comes from the left stick, so the engine is
// fully playable without a keyboard; the keyboard stays a first-class fallback
// and both can be mixed at any time.
//
// The module is pure input: it never looks at the game state and only produces
// Input::State.  Call order in App:
//
//   input_.init(opt_.dataDir);          // after the renderer/video is up; never fails
//   ...
//   Input::State st = input_.poll(dt);  // once per frame, after SDL_PollEvent
//   Vehicle::Input vi;                  // (Vehicle.h, not included here on purpose)
//   vi.throttle  = clampf(st.throttle - st.brake, -1.0f, 1.0f);   // brake is 0..1
//   vi.steer     = st.steer;
//   vi.handbrake = st.handbrake;
//   if (st.viewToggle) viewMode_ = (viewMode_ + 1) % 3;
//   if (st.buy)     ...;   // edge, fires once per press
//   if (st.job)     ...;
//   if (st.deliver) ...;
//   if (st.menu)    ...;   // Start / Esc
//   if (st.map)     ...;   // Back / Select / Tab (map screen, skip)
//   ... input_.rumble(0.8f, 0.4f, 220);  // crash / off-road feedback
//   ...
//   input_.shutdown();                  // before or after SDL_Quit, both safe
//
// Mapping table, filter constants and hot-plug behaviour: docs/input.md.
class Input {
public:
    struct State {
        float throttle = 0;     // -1..1
        float steer = 0;        // -1..1
        float brake = 0;        // 0..1
        bool handbrake = false;
        bool viewToggle = false;   // edge
        bool buy = false;          // edge
        bool job = false;          // edge
        bool deliver = false;      // edge
        bool menu = false;         // edge
        bool any = false;          // any activity this frame
        bool map = false;          // edge: map/skip (Back/Select, Tab) - extension
        // edge: toggle the original's road-map overlay (M key).  Separate from
        // `map` because the original binds the map toggle to M
        // (docs/original_ui.md 3.1: "<M> Switch Camera On/Off" is the camera key
        // list; the in-game map key of the retail build is M).
        bool mapToggle = false;
    };

    // Bit indices for RawPad::buttons.  They mirror SDL_CONTROLLER_BUTTON_*
    // (static_assert'ed in Input.cpp) so a test can inject "button 3" by index.
    enum PadButton : uint32_t {
        PAD_A = 0, PAD_B, PAD_X, PAD_Y, PAD_BACK, PAD_GUIDE, PAD_START,
        PAD_LSTICK, PAD_RSTICK, PAD_LB, PAD_RB,
        PAD_DPAD_UP, PAD_DPAD_DOWN, PAD_DPAD_LEFT, PAD_DPAD_RIGHT,
        PAD_BUTTON_COUNT
    };

    // Raw pad snapshot exactly as the SDL controller API reports it (and the
    // synthetic format used by tools/input_test.cpp to drive the same filters).
    struct RawPad {
        float lx = 0, ly = 0;    // left stick, -1..1
        float rx = 0, ry = 0;    // right stick, -1..1 (y: -1 = up)
        float lt = -1, rt = -1;  // triggers, SDL range: -1 released, +1 pressed
        uint32_t buttons = 0;    // bit (1u << PadButton)
        bool connected = false;  // informational: the snapshot came from a pad
    };

    // All filter constants live here; setTuning() makes them configurable in
    // code without a config file (also used by the test harness).
    struct Tuning {
        float deadZone = 0.18f;         // per-axis stick dead zone, rescaled
        float triggerDeadZone = 0.08f;  // analog trigger dead zone
        float steerExpo = 0.35f;        // steering expo, 0 = linear, 1 = cubic
        float pedalLinear = 0.65f;      // pedal curve: L*x + (1-L)*x^2
        float steerRiseRate = 3.0f;     // units/s toward more lock (0 disables)
        float steerFallRate = 6.0f;     // units/s back toward centre (0 disables)
        bool  stickPedals = true;       // right stick Y = throttle/brake
        bool  dpadDriving = true;       // D-pad = digital steer + throttle/brake
    };

    // Pure, SDL-free filter helpers; unit-tested by tools/input_test.cpp.
    struct Filter {
        static float clampf(float v, float lo, float hi);
        // Centred stick axis: dead zone, then the remaining travel is rescaled
        // so the full physical range is usable.  |raw| <= dz -> 0.
        static float applyStick(float raw, float deadZone);
        // SDL trigger axis (-1 released .. +1 pressed) -> 0..1, with its own
        // dead zone and the same rescale.
        static float applyTrigger(float raw, float deadZone);
        // Throttle/brake response curve (0..1 -> 0..1), monotonic, smooth at 0:
        //   shaped = linear*x + (1-linear)*x^2
        static float shapePedal(float x, float linear);
        // Steering expo curve (-1..1 -> -1..1):
        //   shaped = (1-expo)*x + expo*x^3
        static float shapeSteer(float x, float expo);
        // Slew limiter: move `current` toward `target` by at most rate*dt.
        // riseRate is used while |target| >= |current|, fallRate otherwise.
        // A rate <= 0 disables the limiter.
        static float rateLimit(float current, float target, float dt,
                               float riseRate, float fallRate);
        // Take whichever of a/b has the larger magnitude (keeps the sign).
        static float blend(float a, float b);
    };

    Input() = default;
    ~Input();
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;

    // Never fails: with no pad (or no joystick support at all) the module still
    // works keyboard-only.  `dataDir` is used to find gamecontrollerdb.txt.
    bool init();
    bool init(const std::string& dataDir);
    void shutdown();

    // Polls the pad state + keyboard state.  SDL events must have been pumped
    // by the caller (App's frame loop already does); device hot-plug is handled
    // either through an internal SDL event watch or by forwarding events to
    // handleEvent().  dt is clamped to [0, 0.1].  Call it exactly once per
    // frame: the one-shot edges are consumed by the call that sees them.
    State poll(float dt);

    // Optional: forward pumped events (SDL_CONTROLLERDEVICEADDED/REMOVED/...).
    // A private event watch does this too, and both paths are idempotent.
    void handleEvent(const SDL_Event& e);

    bool controller() const { return pad_ != nullptr; }
    const std::string& controllerName() const { return padName_; }
    // Human-readable source of the active mapping DB ("file (N mappings)" or
    // "SDL built-in database").
    const std::string& mappingSource() const { return mappingSource_; }
    int joystickCount() const { return joyCount_; }

    // Dead zone for the analog sticks (0..1).
    void setDeadZone(float dz);
    void setTuning(const Tuning& t) { tuning_ = t; }
    const Tuning& tuning() const { return tuning_; }

    // Rumble feedback (crash / off-road later).  low/high are 0..1 and are
    // scaled to the SDL 0..65535 range; ms is the duration.  A no-op when there
    // is no pad or the pad reports no rumble support.
    void rumble(float low, float high, uint32_t ms);
    void stopRumble();
    bool rumbleSupported() const { return pad_ != nullptr && hasRumble_; }

    // --- hot-plug handling (also exposed so a caller/test can drive it) -----
    void onControllerAdded(int deviceIndex);
    void onControllerRemoved(uint32_t instanceId);

    // --- synthetic input (tests / headless demos) --------------------------
    // While injected, poll() uses this snapshot instead of the SDL pad, but runs
    // exactly the same filters.
    void injectRawPad(const RawPad& p);
    void clearInjectedPad();
    bool injected() const { return injected_; }

private:
    // One-shot edge detector: true on exactly the frame the input goes 0 -> 1.
    struct EdgeState {
        bool prev = false;
        bool update(bool now) { const bool e = now && !prev; prev = now; return e; }
        void reset() { prev = false; }
    };

    State computeState(const RawPad& raw, float dt);
    RawPad readPad() const;
    void openPad(int deviceIndex);
    void closePad();
    void rescan();
    void processPendingDevices();
    void loadMappings(const std::string& dataDir);
    static int eventWatch(void* userdata, SDL_Event* e);

    void* pad_ = nullptr;              // SDL_GameController*
    int padInstance_ = -1;
    std::string padName_;
    std::string mappingSource_ = "SDL built-in database";

    Tuning tuning_;
    float steer_ = 0;                  // rate-limited steering, kept across frames
    EdgeState view_, buy_, job_, deliver_, menu_, map_;
    EdgeState kbView_, kbBuy_, kbJob_, kbDeliver_, kbMenu_, kbMap_, kbMapToggle_;

    bool inited_ = false;
    bool ownControllerSubsystem_ = false;
    bool watchInstalled_ = false;
    bool hasRumble_ = false;
    bool rumbleWarned_ = false;
    int joyCount_ = 0;
    uint32_t nextOpenMs_ = 0;           // backoff after a failed SDL_GameControllerOpen

    std::atomic<int> pendingAdd_{-1};
    std::atomic<uint32_t> pendingRemove_{0};
    std::atomic<int> pendingRemap_{0};

    bool injected_ = false;
    RawPad injectedPad_;
};

// Alias so callers can write ht2::InputFilter as well as ht2::Input::Filter.
using InputFilter = Input::Filter;

} // namespace ht2

// input_test.cpp -- headless harness for src/app/Input.{h,cpp}.
//
// Runs with no controller attached (the normal case on a dev machine): it drives
// the real filter path with synthetic SDL axis/button values through
// Input::injectRawPad(), so dead zone, pedal curve, steering rate limiting and
// edge detection are measured on the module that ships, not on a copy.
//
// Build (it is not part of the app target on purpose):
//   c++ -std=c++17 -O2 -I src tools/input_test.cpp src/app/Input.cpp \
//       -o tools/out/input_test $(pkg-config --cflags --libs sdl2)
//   tools/build_input_test.sh          # same thing, runs it too
// Run:
//   SDL_VIDEODRIVER=dummy ./tools/out/input_test [dataDir]
//   (the harness sets SDL_VIDEODRIVER=dummy itself when it is unset)
#include "app/Input.h"
#include <SDL.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using ht2::Input;

static int gChecks = 0, gFails = 0;

static void check(const char* what, bool ok) {
    ++gChecks;
    if (!ok) ++gFails;
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
}

static bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

// SDL reports stick axes as SDL_JoystickGetAxis int16s; the controller API
// normalises them to -1..1 exactly like Input::readPad() does.
static float normAxis(int v) {
    const float f = static_cast<float>(v) / 32767.0f;
    return f < -1.0f ? -1.0f : (f > 1.0f ? 1.0f : f);
}

static const float kDt = 1.0f / 60.0f;

// Run `frames` polls with a fixed raw pad snapshot, return the last state.
static Input::State runFixed(Input& in, const Input::RawPad& p, int frames, float dt = kDt) {
    Input::State s;
    for (int i = 0; i < frames; ++i) {
        in.injectRawPad(p);
        s = in.poll(dt);
    }
    return s;
}

// Let the steering slew limiter return to centre so every table row starts clean.
static void resetSteer(Input& in, int maxFrames = 400) {
    for (int i = 0; i < maxFrames; ++i) {
        in.injectRawPad(Input::RawPad());
        if (in.poll(kDt).steer == 0.0f) return;
    }
}

static void section(const char* title) { std::printf("\n== %s\n", title); }

int main(int argc, char** argv) {
    // Headless: dummy video (keyboard state still works), dummy audio.
    if (!SDL_getenv("SDL_VIDEODRIVER")) SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (!SDL_getenv("SDL_AUDIODRIVER")) SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);

    const std::string dataDir = argc > 1 ? argv[1] : "original";
    std::printf("== ht2re unified input harness ==\n");
    std::printf("data dir: %s | SDL_VIDEODRIVER=%s\n", dataDir.c_str(), SDL_getenv("SDL_VIDEODRIVER"));

    // Exactly like the app: the renderer brings up SDL_INIT_VIDEO only, the
    // input module has to bring up the controller subsystem itself.
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::printf("SDL_Init failed: %s\n", SDL_GetError());
        return 2;
    }
    std::printf("controller subsystem before Input::init(): %s\n",
                SDL_WasInit(SDL_INIT_GAMECONTROLLER) ? "up" : "down (init() must bring it up)");

    // poll() before init() must not crash either (defensive).
    {
        Input early;
        const Input::State es = early.poll(kDt);
        std::printf("poll() before init(): ok (throttle %.2f, controller %s)\n",
                    (double)es.throttle, early.controller() ? "yes" : "no");
    }
    SDL_version linked;
    SDL_GetVersion(&linked);
    std::printf("SDL %d.%d.%d\n", linked.major, linked.minor, linked.patch);

    // -----------------------------------------------------------------------
    section("1. open the module (no pad expected on this machine)");
    Input in;
    const bool initOk = in.init(dataDir);
    check("init() returned true", initOk);
    std::printf("  controller detected : %s\n", in.controller() ? "yes" : "no - keyboard only");
    std::printf("  controller name     : %s\n", in.controller() ? in.controllerName().c_str() : "(none)");
    std::printf("  mapping source      : %s\n", in.mappingSource().c_str());
    std::printf("  joysticks reported  : %d\n", in.joystickCount());
    std::printf("  rumble supported    : %s\n", in.rumbleSupported() ? "yes" : "no (no pad)");
    std::printf("  dead zone (sticks)  : %.2f\n", in.tuning().deadZone);
    std::printf("  trigger dead zone   : %.2f\n", in.tuning().triggerDeadZone);
    std::printf("  steer expo / pedal  : %.2f / %.2f\n", in.tuning().steerExpo, in.tuning().pedalLinear);
    std::printf("  steer rate limiter  : rise %.1f u/s, fall %.1f u/s\n",
                in.tuning().steerRiseRate, in.tuning().steerFallRate);
    check("init() succeeds with no pad and reports keyboard-only", initOk && !in.controller());
    check("init() brought up SDL_INIT_GAMECONTROLLER itself (hot-plug events will be generated)",
          SDL_WasInit(SDL_INIT_GAMECONTROLLER) != 0);

    // -----------------------------------------------------------------------
    section("2. filter units (pure helpers)");
    using F = Input::Filter;
    check("applyStick(0.00, 0.18) == 0", near(F::applyStick(0.00f, 0.18f), 0.0f));
    check("applyStick(0.10, 0.18) == 0 (inside dead zone)", near(F::applyStick(0.10f, 0.18f), 0.0f));
    check("applyStick(0.18, 0.18) == 0 (edge of dead zone)", near(F::applyStick(0.18f, 0.18f), 0.0f));
    check("applyStick(0.50, 0.18) == 0.3902 (rescaled)",
          near(F::applyStick(0.50f, 0.18f), (0.50f - 0.18f) / 0.82f));
    check("applyStick(1.00, 0.18) == 1.0000 (full travel usable, not 0.82)",
          near(F::applyStick(1.00f, 0.18f), 1.0f));
    check("applyStick(-0.50, 0.18) == -0.3902 (symmetric)",
          near(F::applyStick(-0.50f, 0.18f), -0.3902f));
    check("applyTrigger(-1.00, 0.08) == 0 (released)", near(F::applyTrigger(-1.00f, 0.08f), 0.0f));
    check("applyTrigger(+1.00, 0.08) == 1 (fully pressed)", near(F::applyTrigger(1.00f, 0.08f), 1.0f));
    check("applyTrigger(0.00, 0.08) == 0.4565",
          near(F::applyTrigger(0.00f, 0.08f), (0.5f - 0.08f) / 0.92f));
    check("shapePedal(0.5, 0.65) == 0.4125", near(F::shapePedal(0.5f, 0.65f), 0.4125f));
    check("shapePedal(1.0, 0.65) == 1.0", near(F::shapePedal(1.0f, 0.65f), 1.0f));
    check("shapeSteer(0.5, 0.35) == 0.36875", near(F::shapeSteer(0.5f, 0.35f), 0.36875f));
    check("shapeSteer(1.0, 0.35) == 1.0", near(F::shapeSteer(1.0f, 0.35f), 1.0f));
    check("rateLimit rise: 0 -> 1 in 1/60 s = 0.05 (3.0 u/s)",
          near(F::rateLimit(0.0f, 1.0f, kDt, 3.0f, 6.0f), 0.05f));
    check("rateLimit fall: 1 -> 0 in 1/60 s = 0.90 (6.0 u/s)",
          near(F::rateLimit(1.0f, 0.0f, kDt, 3.0f, 6.0f), 0.90f));
    check("rateLimit reaches target exactly", near(F::rateLimit(0.98f, 1.0f, kDt, 3.0f, 6.0f), 1.0f));
    check("rateLimit disabled with rate 0", near(F::rateLimit(0.0f, 1.0f, kDt, 0.0f, 0.0f), 1.0f));

    // -----------------------------------------------------------------------
    section("3. dead zone / pedal curve: synthetic SDL axis values -> State");
    std::printf("  left stick X (SDL int16 -> filter)                  |  right trigger (SDL int16 -> filter)\n");
    std::printf("  %8s %9s %9s %-9s |  %8s %9s %9s %-9s\n",
                "raw", "norm", "steer", "note", "raw", "norm", "throttle", "note");
    const int stickRaw[] = {-32768, -16384, -6000, -2000, 0, 2000, 6000, 16384, 32767};
    const int trigRaw[] = {-32768, -16000, -6000, 0, 8000, 16384, 24576, 32767};
    for (int i = 0; i < 8; ++i) {
        resetSteer(in);
        Input::RawPad sp;
        sp.lx = normAxis(stickRaw[i]);
        Input::State ss = runFixed(in, sp, 200);
        Input::RawPad tp;
        tp.rt = normAxis(trigRaw[i]);
        Input::State ts = runFixed(in, tp, 2);
        std::printf("  %8d %9.4f %9.4f %-9s |  %8d %9.4f %9.4f %-9s\n",
                    stickRaw[i], sp.lx, ss.steer, std::fabs(sp.lx) <= 0.18f ? "dead zone" : "",
                    trigRaw[i], tp.rt, ts.throttle, tp.rt >= 0.999f ? "full" : "");
    }
    {
        resetSteer(in);
        Input::RawPad p; p.lx = 1.0f;
        Input::State s = runFixed(in, p, 600);
        check("stick at full right lock gives steer 1.0 (dead zone does not shrink the range)",
              near(s.steer, 1.0f));
        resetSteer(in);
        p.lx = -1.0f;
        s = runFixed(in, p, 600);
        check("stick at full left lock gives steer -1.0", near(s.steer, -1.0f));
    }
    {
        // Per-axis dead zone: a diagonal push with X inside the dead zone must not steer.
        Input::RawPad p; p.lx = 0.15f; p.ly = -1.0f;
        resetSteer(in);
        Input::State s = runFixed(in, p, 60);
        std::printf("  diagonal push (lx=0.15 inside dz, ly=-1.0) -> steer %.4f\n", s.steer);
        check("per-axis dead zone ignores the X axis only (steer == 0)", near(s.steer, 0.0f));
    }
    {
        // Right stick Y as an alternative throttle/brake.
        resetSteer(in);
        Input::RawPad up; up.ry = -1.0f;
        Input::State sUp = runFixed(in, up, 2);
        Input::RawPad dn; dn.ry = 1.0f;
        Input::State sDn = runFixed(in, dn, 2);
        std::printf("  right stick up -> throttle %.3f | right stick down -> brake %.3f\n",
                    sUp.throttle, sDn.brake);
        check("right stick up gives full throttle", near(sUp.throttle, 1.0f));
        check("right stick down gives full brake", near(sDn.brake, 1.0f));
    }
    {
        // LT brake and RT throttle are independent.
        resetSteer(in);
        Input::RawPad p; p.rt = 1.0f; p.lt = 1.0f;
        Input::State s = runFixed(in, p, 2);
        std::printf("  both triggers full -> throttle %.3f, brake %.3f (caller: throttle-brake = %.3f)\n",
                    s.throttle, s.brake, s.throttle - s.brake);
        check("both triggers at once are reported separately (throttle 1, brake 1)",
              near(s.throttle, 1.0f) && near(s.brake, 1.0f));
    }

    // -----------------------------------------------------------------------
    section("4. steering rate limiting (stick flick must not spin the truck)");
    {
        resetSteer(in);
        Input::RawPad full; full.lx = 1.0f;
        in.injectRawPad(full);
        Input::State s1 = in.poll(kDt);
        Input::State s = s1;
        int frames = 1;
        while (s.steer < 0.999f && frames < 600) { s = in.poll(kDt); ++frames; }
        std::printf("  one frame of full stick -> steer %.4f; full lock after %d frames (%.2f s)\n",
                    s1.steer, frames, frames / 60.0f);
        check("one frame of full stick gives 0.05, not 1.0 (rate limited)", near(s1.steer, 0.05f));
        check("full lock takes 20 frames = 0.33 s at rise 3.0 u/s", frames == 20);

        in.injectRawPad(Input::RawPad());
        int back = 0;
        do { s = in.poll(kDt); ++back; } while (s.steer > 0.001f && back < 600);
        std::printf("  release -> centre in %d frames (%.2f s)\n", back, back / 60.0f);
        check("self-centring takes 10 frames = 0.17 s at fall 6.0 u/s", back == 10);

        // A quick flick to 30% of travel must not overshoot into full lock.
        resetSteer(in);
        Input::RawPad flick; flick.lx = 0.30f;
        Input::State s2 = runFixed(in, flick, 60);
        std::printf("  held flick lx=0.30 -> steer %.4f (shaped target %.4f)\n",
                    s2.steer, Input::Filter::shapeSteer(Input::Filter::applyStick(0.30f, 0.18f), 0.35f));
        check("a small stick movement already settles on a small lock (no overshoot)",
              near(s2.steer, Input::Filter::shapeSteer(Input::Filter::applyStick(0.30f, 0.18f), 0.35f), 5e-3f));
    }

    // -----------------------------------------------------------------------
    section("5. edge detection (holding a button must fire exactly once)");
    {
        struct Case { const char* name; int bit; int frames; };
        const Case cases[] = {
            {"Y/Triangle  viewToggle", Input::PAD_Y, 61},
            {"X/Square    buy", Input::PAD_X, 61},
            {"B/Circle    job", Input::PAD_B, 61},
            {"LB          deliver", Input::PAD_LB, 61},
            {"Start       menu", Input::PAD_START, 61},
            {"Back/Select map", Input::PAD_BACK, 61},
            {"RB          viewToggle (alt)", Input::PAD_RB, 61},
        };
        bool allOnce = true;
        for (const Case& c : cases) {
            runFixed(in, Input::RawPad(), 2);            // release any previous edge
            int fires = 0;
            for (int f = 0; f < c.frames; ++f) {
                Input::RawPad p;
                p.buttons = 1u << c.bit;
                Input::State s = runFixed(in, p, 1);
                bool fired = false;
                switch (c.bit) {
                    case Input::PAD_Y: case Input::PAD_RB: fired = s.viewToggle; break;
                    case Input::PAD_X: fired = s.buy; break;
                    case Input::PAD_B: fired = s.job; break;
                    case Input::PAD_LB: fired = s.deliver; break;
                    case Input::PAD_START: fired = s.menu; break;
                    default: fired = s.map; break;
                }
                if (fired) ++fires;
            }
            std::printf("  %-28s held %3d frames -> fired %d time(s)\n", c.name, c.frames, fires);
            if (fires != 1) allOnce = false;
        }
        check("every held button fired exactly once (no repeat while held)", allOnce);

        Input::RawPad a; a.buttons = 1u << Input::PAD_A;
        Input::State held = runFixed(in, a, 30);
        Input::State released = runFixed(in, Input::RawPad(), 2);
        std::printf("  A/Cross handbrake: held -> %s, released -> %s\n",
                    held.handbrake ? "true" : "false", released.handbrake ? "true" : "false");
        check("handbrake is level-based (true while held, false after release)",
              held.handbrake && !released.handbrake);
        check("holding a button keeps any == true", held.any);
    }

    // -----------------------------------------------------------------------
    section("6. keyboard fallback (values written into SDL_GetKeyboardState)");
    {
        // Test-only trick: SDL2 keeps the keyboard state in a writable internal
        // array, so the harness can create key presses without a video driver.
        // Nothing in src/app/Input.cpp does this.
        Uint8* kb = const_cast<Uint8*>(SDL_GetKeyboardState(nullptr));
        in.clearInjectedPad();
        if (kb) {
            auto clear = [&]() { for (int i = 0; i < SDL_NUM_SCANCODES; ++i) kb[i] = 0; };
            clear();
            for (int i = 0; i < 20; ++i) in.poll(kDt);   // settle the wheel to centre

            kb[SDL_SCANCODE_W] = 1;
            Input::State s = in.poll(kDt);
            std::printf("  W held 1 frame      -> throttle %.3f, steer %.3f\n", s.throttle, s.steer);
            check("keyboard W gives throttle 1.0 (no filter lag)", near(s.throttle, 1.0f));

            clear(); kb[SDL_SCANCODE_S] = 1;
            s = in.poll(kDt);
            std::printf("  S held 1 frame      -> throttle %.3f\n", s.throttle);
            check("keyboard S gives throttle -1.0 (brake/reverse)", near(s.throttle, -1.0f));

            clear(); kb[SDL_SCANCODE_SPACE] = 1;
            int hbFrames = 0;
            for (int i = 0; i < 5; ++i) { s = in.poll(kDt); if (s.handbrake) ++hbFrames; }
            check("keyboard Space gives handbrake", hbFrames == 5);

            clear(); kb[SDL_SCANCODE_D] = 1;
            s = in.poll(kDt);
            std::printf("  D held 1 frame      -> steer %.4f (same limiter as the stick)\n", s.steer);
            check("keyboard D is rate limited too (0.05 after one frame)", near(s.steer, 0.05f));

            clear();
            for (int i = 0; i < 20; ++i) in.poll(kDt);
            kb[SDL_SCANCODE_C] = 1;
            int viewFires = 0;
            for (int i = 0; i < 40; ++i) { s = in.poll(kDt); if (s.viewToggle) ++viewFires; }
            std::printf("  C held 40 frames    -> viewToggle fired %d time(s)\n", viewFires);
            check("keyboard C fires the view edge once", viewFires == 1);

            clear();
            for (int i = 0; i < 5; ++i) in.poll(kDt);
            check("keyboard released -> any == false", !in.poll(kDt).any);
        } else {
            std::printf("  SDL_GetKeyboardState returned NULL - keyboard part skipped\n");
        }
    }

    // -----------------------------------------------------------------------
    section("7. 600-poll scripted run (rt ramp, sine steering, stick pedals, buttons)");
    {
        in.clearInjectedPad();
        Input::State s;
        int nView = 0, nBuy = 0, nJob = 0, nDeliver = 0, nMenu = 0, nMap = 0, anyFrames = 0;
        float maxThr = 0, maxBrake = 0, minSteer = 0, maxSteer = 0;
        for (int f = 0; f < 600; ++f) {
            Input::RawPad p;
            p.rt = -1.0f + 2.0f * std::fmin(1.0f, f / 120.0f);      // trigger ramps over 2 s
            p.lx = std::sin(f * 0.05f) * 0.9f;                       // analog steering
            p.ry = (f % 200 < 100) ? -0.7f : 0.7f;                   // alt stick pedals
            if (f >= 200 && f < 260) p.buttons |= 1u << Input::PAD_Y;
            if (f == 300) p.buttons |= 1u << Input::PAD_X;
            if (f == 320) p.buttons |= 1u << Input::PAD_B;
            if (f == 340) p.buttons |= 1u << Input::PAD_LB;
            if (f == 360) p.buttons |= 1u << Input::PAD_START;
            if (f == 380) p.buttons |= 1u << Input::PAD_BACK;
            if (f >= 400 && f < 431) p.buttons |= 1u << Input::PAD_A;   // handbrake
            in.injectRawPad(p);
            s = in.poll(kDt);
            nView += s.viewToggle; nBuy += s.buy; nJob += s.job;
            nDeliver += s.deliver; nMenu += s.menu; nMap += s.map;
            if (s.any) ++anyFrames;
            maxThr = std::fmax(maxThr, s.throttle);
            maxBrake = std::fmax(maxBrake, s.brake);
            minSteer = std::fmin(minSteer, s.steer);
            maxSteer = std::fmax(maxSteer, s.steer);
        }
        std::printf("  600 polls: throttle max %.3f | brake max %.3f | steer %.3f .. %.3f\n",
                    maxThr, maxBrake, minSteer, maxSteer);
        std::printf("  edges: view %d, buy %d, job %d, deliver %d, menu %d, map %d | any in %d/600 frames\n",
                    nView, nBuy, nJob, nDeliver, nMenu, nMap, anyFrames);
        check("scripted run: throttle reached 1.0 from the trigger ramp", maxThr > 0.98f);
        check("scripted run: right stick Y produced brake", maxBrake > 0.3f);
        check("scripted run: steering used the analog range", minSteer < -0.7f && maxSteer > 0.7f);
        check("scripted run: all six edges fired exactly once",
              nView == 1 && nBuy == 1 && nJob == 1 && nDeliver == 1 && nMenu == 1 && nMap == 1);
        check("scripted run: any was true on every active frame", anyFrames == 600);
    }

    // -----------------------------------------------------------------------
    section("8. hot-plug / disconnect paths (no hardware available here)");
    {
        in.onControllerAdded(0);      // index does not exist: must not crash
        in.onControllerRemoved(0);    // instance id does not exist: must not crash
        // Simulated event delivery: exactly what the internal SDL event watch does.
        SDL_Event e;
        std::memset(&e, 0, sizeof(e));
        e.type = SDL_CONTROLLERDEVICEADDED;
        e.cdevice.which = 7;
        in.handleEvent(e);
        e.type = SDL_CONTROLLERDEVICEREMOVED;
        e.cdevice.which = 12345;
        in.handleEvent(e);
        Input::State s = in.poll(kDt);
        std::printf("  after simulated ADDED/REMOVED: controller=%s, joysticks=%d, poll ok (any=%d)\n",
                    in.controller() ? in.controllerName().c_str() : "none", in.joystickCount(),
                    (int)s.any);
        check("simulated ADDED/REMOVED/handleEvent do not crash", true);
        check("poll() keeps working after the hot-plug paths", in.joystickCount() == 0);
    }

    // -----------------------------------------------------------------------
    section("9. rumble hooks must be safe no-ops without a pad");
    {
        in.rumble(0.8f, 0.4f, 220);   // no pad: logs once, never crashes
        in.rumble(-5.0f, 12.0f, 100); // out-of-range values are clamped
        in.stopRumble();
        std::printf("  rumble(0.8, 0.4, 220) / rumble(-5, 12, 100) / stopRumble() -> no crash, supported=%s\n",
                    in.rumbleSupported() ? "yes" : "no");
        check("rumble() is a no-op when no controller is connected", !in.rumbleSupported());
    }

    // -----------------------------------------------------------------------
    section("10. shutdown safety (App may destroy the module after SDL_Quit)");
    {
        SDL_Quit();
        in.shutdown();          // must not touch freed SDL state
        in.shutdown();          // must be idempotent
        std::printf("  SDL_Quit() then shutdown() twice -> no crash\n");
        check("shutdown() is safe after SDL_Quit and idempotent", true);
    }

    // -----------------------------------------------------------------------
    section("summary");
    std::printf("  checks: %d, failures: %d\n", gChecks, gFails);
    std::printf("  controller: %s\n", in.controller() ? in.controllerName().c_str() : "none (keyboard only)");
    std::printf("  mapping: %s\n", in.mappingSource().c_str());
    std::printf("  %s\n", gFails == 0 ? "RESULT: PASS" : "RESULT: FAIL");

    return gFails == 0 ? 0 : 1;
}

#pragma once
#include <string>
#include "../gfx/Renderer.h"
#include "../ui/Font.h"
#include "../ui/Menu.h"
#include "../ui/EscMenu.h"
#include "../ui/Video.h"
#include "../ui/Loading.h"
#include "../world/Terrain.h"
#include "../world/Way.h"
#include "../world/B3D.h"
#include "../world/SceneTextures.h"
#include "../world/Overlay.h"
#include "../game/Vehicle.h"
#include "../world/GroundMap.h"
#include "../world/Weather.h"
#include "../script/GameData.h"
#include "../script/ScriptRules.h"
#include "../game/Economy.h"
#include "../game/Traffic.h"
#include "../game/Collision.h"
#include "../game/Cockpit.h"
#include "../gfx/Mirror.h"
#include "../audio/Audio.h"
#include "Input.h"
#include "../ui/Hud.h"
#include "../ui/Map.h"
#include <map>
#include <memory>
#include <set>

namespace ht2 {

struct Options {
    std::string dataDir = "./original";
    bool screenshot = false;
    std::string out = "shot.bmp";
    float cam[5] = {0, 900, 6200, 0, -12};
    bool camGiven = false;
    float above = 0.0f;
    int w = 1280, h = 720;
    std::string scene = "menu";   // menu | world
    float time = 0.0f;            // fixed menu-scene time (screenshots)
    bool b3dinfo = false;
    bool resinfo = false;
    bool resdump = false;
    bool worldB3d = false;        // draw ENV/aa.b3d geometry in the world state
    std::string msg;              // debug: show this status line on the menu
    std::string vehicle = "ZilR"; // vehicle model container in COMMON/TRUCKS.B3D
    // --overlay FILE: the map/vehicle editor's output (tools/editor,
    // src/world/Overlay.cpp, docs/editor.md).  The shipped data is never
    // modified; the overlay carries absolute node positions, segment
    // attr/width, part visibility/material and anchor transforms.
    std::string overlay;
    bool allTiles = true;         // merge every ENV tile into the world
    bool scripts = false;        // --scripts: decrypt+parse every script, print a report
    bool scriptReport = false;   // --script-report: the recovered rule coverage
    bool scriptCoverage = false; // --script-coverage: also dump the (form, field) table
    std::string scriptDump;      // --script-dump FILE: write the decrypted text to stdout
    bool listVehicles = false;   // --vehicles: print the catalogue from evehicles.sch
    std::string hideMat;         // --hide-mat PREFIX: skip world submeshes by material
    std::string onlyMat;         // --only-mat PREFIX: keep only these (diagnostics)
    std::string onlyName;        // --only-name PREFIX: keep only these (diagnostics)
    bool noGround = false;       // --no-ground: skip the generated ground/terrain mesh
    bool probe = false;          // --probe x,z[,r]: list world submeshes near a point
    float probeX = 0, probeZ = 0, probeR = 120.0f;
    bool roadProbe = false;      // --road-probe: vertical error of the traffic over
                                 // the road graph + the asphalt/ground gap
    float roadProbeStep = 8.0f;  // sampling step along the road network (metres)
    bool roadProbeSite = false;  // --road-probe-site x,z[,r]: dump the worst samples
    bool rideProbe = false;      // --ride-probe x,z: dump every surface layer there
    float trafficWheelProbe = 0.0f;   // --traffic-wheel-probe [SEC]: wheel error of the
                                      // simulated AI at several densities
    float roadProbeX = 0, roadProbeZ = 0, roadProbeR = 200.0f;
    bool noCull = false;         // debug: disable backface culling
    bool drawTerrain = false;     // M0 terrain mesh (its heights do not match the tiles)
    std::string onlyTile;         // when set, load just this ENV tile (debug)
    float drive = 0.0f;           // headless drive test: seconds of full throttle
    float warmup = 0.0f;          // fast-forward the vehicle N seconds before drawing
    bool autostart = false;
    std::string view = "cockpit";   // --view chase|cockpit|bonnet
    int traffic = 16;              // --traffic N: number of AI cars (0 = none)
    int trafficModels = 5;         // --traffic-models N: distinct AI models
    float trafficRadius = 900.0f;
    float trafficReport = 0.0f;    // --traffic-report SECONDS: headless AI report
    bool trafficReportOncoming = false;  // ... park the player against the flow
    // Approximation of king.exe's road PositionId transition.  The original
    // has no broad proximity trigger; 20 world units only associates the free
    // driving pose with the named .way node being crossed.
    float eventRadius = (float)ScriptRules::kNodePassRadius;
    bool collisionProbe = false;   // --collision-probe x,z[,r]
    bool obstaclesOpen = false;    // --obstacles open: hide road-closure geometry
    bool fullscreen = false;       // --fullscreen (handhelds)
    bool noCollision = false;      // --no-collision: disable the solid-geometry test
    int buyIndex = -1;             // --buy N: purchase offer N at startup
    int acceptJob = -1;            // --accept-job N: take job N at startup
    bool doDeliver = false;        // --deliver
    bool doRefuel = false;         // --refuel at the current position
    bool doRepair = false;         // --repair at the current position
    float startFuel = -1.0f;       // --fuel F: initial tank level 0..1
    float startDamage = 0.0f;      // --damage D: initial body damage 0..1        // --deliver: complete the active job at the current position
    float startHour = 9.0f;        // --time H: initial hour of the day (0..24)
    float dayLength = 1200.0f;     // --day-length S: real seconds per game day    // --event-radius R: node-event trigger radius // --traffic-radius R: spawn AI within R of the start
    float parkedRadius = 3500.0f;  // --parked-radius: only spawn parked cars this close
    int parkedMax = 24;            // --parked-max: hard cap on spawned parked cars
    // --- audio / video (src/audio/Audio.cpp, src/ui/Video.cpp) --------------
    bool noAudio = false;          // --no-audio: never touch SDL audio
    bool forceAudio = false;       // --audio: open the mixer in headless modes too
    bool noVideo = false;          // --no-video: never play movies (skip silently)
    bool noMenuMusic = false;      // --no-menu-music
    bool menuMusic = false;        // --menu-music: start the menu track even headless
    std::string musicTrack = "track01l";   // --music NAME
    bool movie = false;            // --movie: play the intro (kotr) before the menu
    std::string movieName = "kotr";// --movie-clip NAME (kotr | kotrDemo)
    float movieTime = -1.0f;       // --movie-time S: seek to S seconds (screenshots)
    int movieFrames = -1;          // --movie-frames N: seek to frame N (screenshots)
    bool makeVideo = true;         // --no-make-video: never run tools/make_video.sh
    bool movieSyncReport = false;  // --movie-sync-report: measure the A/V drift, exit
    float movieSyncSeconds = 4.0f; // --movie-sync-seconds S: real playback measured
    int texUpdate = -1;            // --tex-update N: stream frame N into a texture
    std::string loadingShot;       // --loading-shot FILE: render the loading screen
    float loadingAt = 0.35f;       // --loading-at P: progress of that screenshot
    std::string menuAct;           // --menu-act ID: activate a menu item at startup
    // --- in-game ESC screens (src/ui/EscMenu.cpp) ---------------------------
    bool esc = false;              // --esc: open the ESC main screen in the world
    std::string escScreen;         // --esc-screen NAME (main|exit|sound|control|save|load)
    std::string escShot;           // --esc-shot FILE: render one ESC frame and exit
    std::string escAct;            // --esc-act NAME[,NAME...]: scripted activations
    std::string uiKey;             // --ui-key Esc|F1..F5: normal world key path
    // --- weather and draw distance (src/world/Weather.cpp, docs/weather.md) --
    std::string weather;          // --weather NAME: clear|rain|snow|bad|N (preset)
    int weatherSet = 1;           // --weather-set N: WEATHERn preset 1..8
    bool weatherCycle = false;    // --weather-cycle: clear -> rain -> snow
    float weatherCyclePhase = 0.0f;  // --weather-cycle-phase P: 0..1 start offset
    float viewDistance = -1.0f;   // --view-distance M (<0 = the original's WEATHER.INI fogf)
    bool noWeatherFx = false;     // --no-weather-fx: keep the sky/lighting, drop the particles
    // test hook: start as if "SINGLE PLAYER" was picked
    bool vehicleGiven = false;
    bool spawnGiven = false;
    float spawnOverride[3] = {0, 0, 0};   // --spawn x,z[,yaw]
    // --- the original in-game HUD (src/ui/Hud.cpp, src/ui/Map.cpp) ----------
    bool noMap = false;          // --no-map: draw the strip only (panel tests)
    bool mapGiven = false;       // --map: force the map on in a screenshot run
    int fineShot = 0;            // --fine-shot SPEED: show the fine panel, exit
    std::string msgShot;         // --msg-shot TEXT: show the pager window, exit
    std::string jobTime;         // --job-time MM:SS: force the ОСТ.ВРЕМЯ value
    bool jobGiven = false;       // --job: take the nearest job at startup
    float speedFor = 0.0f;       // --speed-limit N: use this limit for the camera
    bool speedLimitGiven = false;
};

// Application state machine: MENU (MENV/dr.b3d scene + overlay) <-> WORLD
// (terrain + road network, the M0/M1 viewer).
class App {
public:
    int run(const Options& o);

private:
    bool loadData();
    bool infoModes();
    static void scanRes(const std::string& dir, std::vector<std::string>& out);
    void frame(float dt, bool interactive);
    // Everything that has to be released before Renderer::shutdown(): the movie
    // texture needs a live GL context and the mixer needs a live SDL.
    void finish();

    Options opt_;
    Renderer r_;
    Font font_;
    Menu menu_;
    Terrain terrain_;
    Way way_;
    B3D menuScene_, worldScene_;
    B3D trucks_;                  // COMMON/TRUCKS.B3D (vehicle models)
    std::vector<B3D::SubMesh> worldMeshes_;   // all ENV tiles merged
    GroundMap ground_;
    std::vector<std::string> worldTileNames_; // "<dir>/ENV/<tile>" for textures
    void loadWorldTiles();
    void pickGroundTexture();
    // Gameplay from the shipped scripts: roadside/parking vehicles placed on the
    // named .way nodes referenced by the mod_*.sch `parking` forms.
    void spawnParkedVehicles();
    void indexWayNodes();
    // --- script runtime / node events recovered from king.exe ---------------
    void prepareNodeEvents();
    void updateNodeEvents(float dt);
    std::unique_ptr<ScriptRuntime> scriptRuntime_;
    double scriptTime_ = 0.0;
    std::string currentRegion_;
    float msgTimer_ = 0.0f;
    std::string msgText_;
    float msgAge_ = 0.0f;          // seconds since the pager text appeared
    bool msgNew_ = false;          // blink the "new message" icon
    // One entry point for the script/node-event/ambush texts (the original's
    // 4-line message ring, drawn in the strip's pager window).
    void pushMessage(const std::string& text, const std::string& kind);
    void pushScriptMessage(const std::string& key);   // key into GameData::messages

    // --- cockpit UI --------------------------------------------------------
    void buildMinimapTexture();
    void drawDashboard();
    void drawMinimap(float x, float y, float size);
    // The original's left-hand map overlay (src/ui/Map.cpp).  Placed inside the
    // 800-px UI space of the HUD strip.  `interactivePass_` mirrors the
    // drawWorld() flag so the map can tell a play frame from a screenshot one
    // (during play it is the player's M toggle; a screenshot only draws it when
    // --map / a capture asked for it).
    void drawMapOverlay();
    bool interactivePass_ = false;
    // --- events -------------------------------------------------------------
    // Speed camera: the player is fined when passing a speed-limit sign too
    // fast (the limit comes from the road/signs, the fine from Economy).
    void initSpeedLimits();
    void updateSpeedCamera(float dt);
    int speedLimitAt(float x, float z) const;
    // --fine-shot / --msg-shot: fill the deterministic capture state
    void optsFineShot();
    void optsMsgShot();
    Hud::Fine fine_;
    float fineCooldown_ = 0.0f;      // s, no new photo while it is counting down
    float fineLog_ = -1.0f;
    int lastLimit_ = -1;             // for the log/tests
    struct LimitZone { float x = 0, z = 0, r = 60.0f; int limit = 60; };
    std::vector<LimitZone> limitZones_;
    size_t finesIssued_ = 0;
    void drawGauge(float cx, float cy, float r, float frac, int ticks, float maxVal,
                   const char* label, float redlineFrom);
    // game clock: 1 in-game day = `dayLength_` seconds of real time
    float timeOfDay_ = 9.0f;
    float dayLength_ = 1200.0f;
    float viewDistance_ = 900.0f;     // metres; the effective draw distance
    bool dayTime() const { return timeOfDay_ >= 6.0f && timeOfDay_ < 20.0f; }
    void updateClock(float dt);
    void applyLighting();
    unsigned minimapTex_ = 0;
    float mapX0_ = 0, mapZ0_ = 0, mapW_ = 1, mapH_ = 1;
    float uiTime_ = 0.0f;

    // --- AI traffic (src/game/Traffic.cpp) ---------------------------------
    void initEconomy();
    void applyEconomyActions();
    bool switchPlayerVehicle(const std::string& model);
    float nearJobDist() const;

    // day/night + weather gates from the `people` forms
    void updateGates();
    // --- weather (src/world/Weather.cpp) -----------------------------------
    Weather weather_;                 // the original's WEATHER.INI presets
    float weatherTime_ = 0.0f;        // --weather-cycle clock (simulated seconds)
    float weatherAnim_ = 0.0f;        // particle animation clock (always advances)
    int weatherKind_ = 0;             // 0 clear, 1 rain, 2 snow
    float weatherAmount_ = 0.0f;      // 0..1, ramps in/out with the cycle
    bool weatherBad_ = false;         // the engine's good/bad boolean
    int weatherPhase_ = 0;            // Weather::Phase of the current hour
    void applyWeather(float dt);      // picks the preset/phase, drives the cycle
    void drawWeatherOverlay();        // rain streaks / snow flakes (2D)
    void drawWeatherStreaks(float amount, bool snow);
    unsigned weatherSeed_ = 0x9E3779B9u;
    // one log line per weather/draw-distance change (tests read it)
    int lastWeatherLog_ = -1;
    void rebuildWorldScene();
    uint32_t rng_ = 0x51ED2701u;   // deterministic gameplay randomness
    std::set<std::string> hiddenGroups_;
    std::vector<B3D::SubMesh> worldVisible_;
    bool gatesBuilt_ = false;

    // ambushes: police/mafia roadblocks triggered when the player drives close
    struct AmbushState {
        float x = 0, z = 0, y = 0;    // y = the road height of the way node
        std::string klass;
        int cars = 1;
        size_t scriptIndex = 0;
        bool fired = false;
    };
    std::vector<AmbushState> ambushes_;
    void initAmbushes();
    void updateAmbushes();
    size_t ambushesFired_ = 0;

    Hud hud_;                     // the original top LED strip
    Map map_;                     // the original left-hand road map overlay
    Input input_;                 // keyboard + game controller

    // --- in-game ESC screens (src/ui/EscMenu.cpp, docs/esc_impl.md) --------
    EscMenu esc_;                 // the ESC main / save / load / sound / control panels
    bool escBack_ = false;        // Esc / pad B seen this frame (resolved in drawWorld)
    // The Esc key and the pad buttons are consumed from the SDL event queue while
    // the panel is up; the Input module's `menu` edge for the very same press
    // must then be ignored for one frame, otherwise Esc would close the panel
    // and immediately reopen it.
    bool escSwallowMenu_ = false;
    void escAction(const std::string& act);   // terminal action -> world state
    void runEscScript(const std::string& list);   // --esc-act NAME[,NAME...]
    bool handleWorldPanelKey(int key);
    void syncEscContext();          // current world/economy values -> open panel
    void leaveWorldToMenu();      // ESC main "ГЛАВНОЕ МЕНЮ" / exit popup
    void applyEscSliders();       // push the sound sliders into Audio
    // --- the small save game (a text file under the SDL user data dir) -----
    // HT2_SAVE_FILE overrides the location (tests / sandboxes); otherwise
    // SDL_GetPrefPath("ht2re","ht2re")/savegame.txt, with the working directory
    // as a last-resort fallback when that is not writable.
    std::vector<std::string> savePaths() const;
    std::string savePath() const;      // the existing file, else the primary path
    std::string saveSummary() const;   // one line shown on the save/load panels
    bool saveGame();                   // money, position, fuel, damage, job
    bool loadGame();
    bool deleteSave();
    void recoverVehicle();             // the "911" button: tow to the nearest stop

    // --- audio (src/audio/Audio.cpp) ---------------------------------------
    Audio audio_;
    bool audioWanted_ = false;    // decided in run(): headless modes stay silent
    bool videoAudioWanted_ = false;   // movie soundtrack (own SDL device)
    bool wasBlocked_ = false;     // Vehicle::blocked() edge -> crash one-shot
    float prevSpeedKmh_ = 0.0f;   // speed before the last update(): the impact speed
    bool menuMusicOn_ = false;    // the menu track is playing
    float crashSpeed_ = 25.0f;    // km/h: below this a scrape is not a crash
    void startAudio();            // guarded init + describe()
    void updateSceneAudio();      // listener + engine loop + collision one-shot
    void startMenuMusic();        // no-op unless the menu is up and audio is on
    void stopMenuMusic();

    // service stations (fuel / repair) from the `service` forms
    struct ServiceStop { float x = 0, z = 0; bool fuel = false, repair = false; };
    std::vector<ServiceStop> serviceStops_;
    void initServices();
    int nearestService(bool wantFuel) const;
    bool refuel(long* paidOut);
    bool repair(long* paidOut);
    long refuelPrice() const;
    long repairPrice() const;
    std::string hiredDriver_;       // the driver selected through BackJob
    std::vector<float> trafficYOff_;   // ground offset per AI model
    // Renderer batches of the intact and the dented skin, one entry per AI model
    // (equal when the container has no damaged material at all).
    std::vector<int> trafficIntact_, trafficDamaged_;
    // Car::damage at which the dented skin is drawn.
    static constexpr float kDamageSkin = 0.35f;
    Traffic traffic_;
    Collision collision_;
    void drawTraffic(float cx, float cy, float cz, float yaw, float pitch);
    void spawnTraffic();
    // The player's body for the car contacts (from the model bbox).
    float playerRadius_ = 1.45f, playerLength_ = 9.5f;
    float crashCooldown_ = 0.0f;  // s, throttle for repeated car-impact crash sounds
    size_t carCrashes_ = 0;       // AI-car impacts that played the crash one-shot
    void refreshPlayerBody();
    // Push / damage / crash sound from the contacts Traffic resolved this frame.
    void applyTrafficHits(float dt);
    // --traffic-report SECONDS: run the AI headlessly and print the statistics.
    int trafficReport();
    // --road-probe: measure, over the whole road network, the vertical error
    // between what a traffic vehicle is drawn at (the graph height `Car::h`,
    // exactly as drawTraffic()/Traffic computes it) and the real road surface
    // under each wheel, plus the vertical gap between that surface and the
    // generated ground mesh (the z-fighting / "road shakes" metric).
    int roadProbeReport();
    // --ride-probe x,z: dump every drivable-surface layer at one site.
    int rideProbeReport();
    // --traffic-wheel-probe SECONDS: run the real AI at several densities and
    // report the vertical error between each car's wheel contact points and the
    // asphalt under them (max / median / p95, and the worst site).
    int trafficWheelProbe();
    void collectWaySegments();          // every .way of every world tile -> waySegs_

    std::map<std::string, Way::Node> wayNodes_;   // every named node of every tile
    std::vector<Way::Seg> waySegs_;               // every road segment (for headings)
    // --overlay: read once in loadData(), applied to every tile's .way in
    // collectWaySegments()/indexWayNodes() and to the player vehicle.
    Overlay overlay_;
    SceneTextures menuTex_, worldTex_, vehicleTex_;
    Vehicle vehicle3d_;
    Cockpit cockpit_;           // 3D instruments from COMMON/CABINES.B3D
    Mirror mirror_;             // rear-view mirror (COMMON/esc16.RES + FBO)
    GameData gameData_;
    B3D parked_;               // COMMON/TRUCKS.B3D, reused to place parked cars
    B3D trafficB3d_;           // COMMON/TRUCKS.B3D, reused for the AI models
    Economy economy_;           // money, vehicle market, cargo jobs
    bool haveMenuScene_ = false, haveWorldScene_ = false, haveTrucks_ = false;
    enum class State { Menu, World, Vehicle, Movie } state_ = State::Menu;
    float worldCam_[5] = {0, 900, 6200, 0, -12};
    float spawn_[3] = {-3020.0f, -4625.0f, 0.0f};   // x, z, yaw (ENV/GAME.CNF start)
    void enterWorld();          // MENU -> WORLD transition (also used by tests)
    // --- movies + loading screen (src/ui/Video.cpp, src/ui/Loading.cpp) ----
    Video video_;
    bool makeVideoTried_ = false; // tools/make_video.sh runs at most once
    bool quitRequested_ = false;  // SDL_QUIT seen while the loading screen pumped
    bool loadingDumped_ = false;  // HT2_LOADING_DUMP: one mid-transition frame
    bool openClip(Video& v, const std::string& clip);   // + auto make_video.sh
    bool ensureVideoPack(const std::string& clip);
    static std::string findMakeVideoScript();
    bool enterMovie(const std::string& clip);   // MENU/WORLD -> MOVIE
    void leaveMovie();                          // MOVIE -> MENU
    bool movieFrame();                          // one Movie frame; false = ended
    bool movieShotRun();                        // --movie + --screenshot seek
    bool movieSyncReportRun();                  // --movie-sync-report: A/V drift
    bool texUpdateRun();                        // --tex-update N
    bool loadingShotRun();                      // --loading-shot FILE
    void pumpLoading(Loading& lg, float dt);    // draw + swap + pump events
    void drawWorld(float dt, bool interactive);
    // camera pose for the current view mode; returns eye position and yaw/pitch
    void viewPose(float& ex, float& ey, float& ez, float& yaw, float& pitch, bool& drawTruck);
    int viewMode_ = 1;           // 0 chase, 1 cockpit, 2 bonnet
    int driveTest();
};

} // namespace ht2

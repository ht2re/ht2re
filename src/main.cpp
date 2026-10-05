#include "app/App.h"
#include "util/Log.h"
#include <cstring>
#include <cstdio>

using namespace ht2;

static const char* USAGE =
    "HT2:RE - Hard Truck 2: King of the Road engine reimplementation\n"
    "usage: ht2re [options]\n"
    "  --data DIR        game data root (default ./original)\n"
    "  --overlay FILE    editor overlay to apply (tools/editor, docs/editor.md):\n"
    "                    .way node/segment edits and TRUCKS.B3D part/anchor edits\n"
    "  --scene menu|world|vehicle  start scene (default menu)\n"
    "  --screenshot      render one frame and save a BMP, then exit\n"
    "  --out FILE        screenshot output (default shot.bmp)\n"
    "  --time SEC        fixed menu animation time (screenshots)\n"
    "  --camera x,y,z,yaw,pitch  camera for the world scene\n"
    "  --above N         place the world camera N units above the terrain\n"
    "  --size WxH        window size (default 1280x720)\n"
    "  --b3dinfo         parse the .b3d scenes, print stats, exit\n"
    "  --resinfo         parse the .res containers, print stats, exit\n"
    "  --resdump         decode every mask/texture blob and verify byte budgets\n"
    "  --world-b3d       also draw ENV/aa.b3d geometry in the world scene\n"
    "  --msg TEXT        show TEXT on the start menu (debug/placeholder)\n"
    "  --vehicle NAME    vehicle container to show in the vehicle scene (default ZilR)\n"
    "  --drive SECONDS   headless drive test: full throttle for N seconds, print telemetry\n"
    "  --tile NAME       load only this ENV tile (default: all tiles merged)\n"
    "  --terrain         draw the M0 terrain*.raw mesh (wrong heights, debug only)\n"
    "  --autostart       start in the world as if SINGLE PLAYER was chosen\n"
    "  --scripts         decrypt+parse every .sch/.CNF and print a report, exit\n"
  "  --script-report   print the script-rule coverage (active/satisfied/fired), exit\n"
  "  --script-coverage with --script-report: also dump the (form, field) table\n"
    "  --script-dump F   write the decrypted text of F to stdout, exit\n"
    "  --vehicles        list the vehicle catalogue read from evehicles.sch\n"
    "  --probe x,z[,r]   list the world submeshes near a world position\n"
    "  --hide-mat PREFIX skip world submeshes whose material starts with PREFIX\n"
    "  --view MODE       chase | cockpit (default) | bonnet - camera for the world\n"
    "  --parked-radius R only place parked cars within R units of the spawn\n"
    "  --parked-max N    cap the number of parked cars placed\n"
    "  --traffic N       number of AI cars driving the road graph (0 = none)\n"
    "  --traffic-report S  run the world AI headlessly for S seconds with the\n"
    "                    player parked in its lane; print the AI statistics\n"
    "  --traffic-report-oncoming  park it against the flow (impact scenario)\n"
    "  --road-probe [STEP]  measure the traffic's vertical error over the whole road\n"
    "                    network and the asphalt/ground-mesh gap (default STEP 8 m)\n"
    "  --road-probe-site x,z[,r]  with --road-probe: dump the worst samples nearby\n"
    "  --ride-probe x,z  print every layer of the drivable surface at one site\n"
    "  --traffic-wheel-probe [S]  run the AI at several densities for S seconds and\n"
    "                    report each car's wheel height error against the asphalt\n"
    "  --event-radius R  .way-node association radius (default 20)\n"
    "  --hour H          starting hour of the in-game day (0..24, default 9)\n"
    "  --weather NAME    weather: clear|rain|snow|bad|fog or a preset 1..8\n"
    "                    (the presets come from the original WEATHER.INI)\n"
    "  --weather-set N   WEATHERn preset to use (default 1; 7 = the mountain\n"
    "                    highway, 5/8 = the tunnels)\n"
    "  --weather-cycle   cycle clear -> rain -> snow every 6 simulated seconds\n"
    "  --no-weather-fx   keep the weather's sky/lighting, drop the particles\n"
    "  --weather-cycle-phase P  0..1 start offset of that cycle (screenshots)\n"
    "  --view-distance M draw distance in metres: fog end, far plane and the\n"
    "                    scene cull (default: the preset's WEATHER.INI fogf_*,\n"
    "                    900 in fair weather - the original's value)\n"
    "  --buy N           buy vehicle offer N at startup\n"
    "  --collision-probe x,z[,r]  report whether a spot is blocked, exit\n"
    "  --fullscreen      borderless fullscreen (handhelds)\n"
    "  --accept-job N    accept cargo job N at startup (0 = nearest)\n"
    "  --deliver         complete the active cargo job at the current position\n"
    "  --day-length S    real seconds per in-game day (default 1200)\n"
    "  --traffic-radius R spawn AI traffic within R of the start\n"
    "  --warmup SECONDS  drive the truck N seconds before the first frame\n"
    "  --spawn x,z[,yaw]  spawn the truck here instead of the GAME.CNF start\n"
    "  --movie           play the intro movie (kotr) before the menu\n"
    "  --movie-clip NAME movie clip to play (kotr | kotrDemo, default kotr)\n"
    "  --movie-time SEC  start/screenshot the movie at SEC seconds\n"
    "  --movie-frames N  start/screenshot the movie at frame N\n"
    "  --movie-shot FILE  one deterministic movie frame -> FILE, then exit\n"
    "                    (same as --movie --screenshot --out FILE)\n"
    "  --movie-sync-report  measure video-vs-soundtrack drift, print it and exit\n"
    "  --movie-sync-seconds S  real playback measured by --movie-sync-report (4)\n"
    "  --no-video        never play a movie (skip silently)\n"
    "  --no-make-video   never run tools/make_video.sh when the pack is missing\n"
    "  --menu-music      start MUSIC/track01l on the menu even headless\n"
    "  --no-menu-music   do not start the menu track\n"
    "  --music NAME      menu track (default track01l)\n"
    "  --audio           initialise the mixer in headless modes too\n"
    "  --no-audio        never touch SDL audio (also skips the movie sound)\n"
    "  --loading-shot F  render the loading screen to the BMP F and exit\n"
    "  --loading-at P    progress 0..1 shown by --loading-shot (default 0.35)\n"
    "  --tex-update N    stream movie frame N through Renderer::updateTexture,\n"
    "                    draw it full screen to --out and exit\n"
    "  --menu-act LIST   activate menu item(s) at startup (graph,back | singl | ...)\n"
    "  --esc             start in the world with the in-game ESC menu open\n"
    "  --esc-screen NAME ESC screen to open: main|exit|sound|control|save|load\n"
    "  --esc-shot FILE   open the ESC screen, render one frame to FILE, exit\n"
    "  --esc-act NAME[,NAME...]  activate ESC buttons at startup (name or id),\n"
    "                    e.g. --esc-act exitsys,exnsave | main | restart | savg\n"
    "  --ui-panel NAME   open an in-game panel (alias of --esc-screen):\n"
    "                    fuel|service|order|sale|job|info|mytruck|f1|f2|f2tball|f3|\n"
    "                    win|win1|win2|lose1..5|popup5|cbpol|cbmaf|cbopp|cbnei|\n"
    "                    cbradio|cbtalk (docs/ui_sort.md)\n"
    "  --ui-shot NAME F  open the panel NAME, render one 1280x720 frame to F, exit\n"
    "  --ui-act LIST     activate panel buttons at startup (alias of --esc-act)\n"
    "  --ui-key KEY      exercise a world hotkey: Esc|F1|F2|F3|F4|F5\n"
    "  --map             force the original's road-map overlay on in a screenshot\n"
    "  --no-map          never draw the map overlay (panel/UI tests)\n"
    "  --job             take the nearest cargo job at startup (HUD strip cells)\n"
    "  --job-time MM:SS  force the strip's ОСТ.ВРЕМЯ value (tests)\n"
    "  --fine-shot SPEED  render one frame with the speed-camera fine panel up\n"
    "                    (the speed limit comes from --speed-limit) and exit\n"
    "  --speed-limit N   override the road's speed limit (speed camera / HUD)\n"
    "  --msg-shot TEXT   render one frame with the script message window showing\n"
    "                    TEXT and the blinking new-message icon, then exit\n";

static bool argIs(const char* a, const char* name) { return strcmp(a, name) == 0; }

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        if (argIs(argv[i], "--data") && i + 1 < argc) o.dataDir = argv[++i];
        else if (argIs(argv[i], "--overlay") && i + 1 < argc) o.overlay = argv[++i];
        else if (argIs(argv[i], "--screenshot")) o.screenshot = true;
        else if (argIs(argv[i], "--out") && i + 1 < argc) o.out = argv[++i];
        else if (argIs(argv[i], "--scene") && i + 1 < argc) o.scene = argv[++i];
        else if (argIs(argv[i], "--time") && i + 1 < argc) { if (sscanf(argv[++i], "%f", &o.time) != 1) return 1; }
        else if (argIs(argv[i], "--above") && i + 1 < argc) { if (sscanf(argv[++i], "%f", &o.above) != 1) return 1; }
        else if (argIs(argv[i], "--camera") && i + 1 < argc) {
            if (sscanf(argv[++i], "%f,%f,%f,%f,%f", &o.cam[0], &o.cam[1], &o.cam[2],
                       &o.cam[3], &o.cam[4]) != 5) { LOG("bad --camera"); return 1; }
            o.camGiven = true;
        }
        else if (argIs(argv[i], "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &o.w, &o.h) != 2) return 1;
        }
        else if (argIs(argv[i], "--b3dinfo")) o.b3dinfo = true;
        else if (argIs(argv[i], "--resinfo")) o.resinfo = true;
        else if (argIs(argv[i], "--resdump")) o.resdump = true;
        else if (argIs(argv[i], "--world-b3d")) o.worldB3d = true;
        else if (argIs(argv[i], "--autostart")) o.autostart = true;
        else if (argIs(argv[i], "--scripts")) o.scripts = true;
        else if (argIs(argv[i], "--script-report")) o.scriptReport = true;
        else if (argIs(argv[i], "--script-coverage")) { o.scriptReport = true; o.scriptCoverage = true; }
        else if (argIs(argv[i], "--script-dump") && i + 1 < argc) o.scriptDump = argv[++i];
        else if (argIs(argv[i], "--vehicles")) o.listVehicles = true;
        else if (argIs(argv[i], "--view") && i + 1 < argc) o.view = argv[++i];
        else if (argIs(argv[i], "--parked-radius") && i + 1 < argc) o.parkedRadius = (float)atof(argv[++i]);
        else if (argIs(argv[i], "--traffic") && i + 1 < argc) o.traffic = atoi(argv[++i]);
        else if (argIs(argv[i], "--event-radius") && i + 1 < argc) o.eventRadius = (float)atof(argv[++i]);
        else if (argIs(argv[i], "--day-length") && i + 1 < argc) o.dayLength = (float)atof(argv[++i]);
        else if (argIs(argv[i], "--hour") && i + 1 < argc) o.startHour = (float)atof(argv[++i]);
        else if (argIs(argv[i], "--weather") && i + 1 < argc) o.weather = argv[++i];
        else if (argIs(argv[i], "--weather-set") && i + 1 < argc) o.weatherSet = atoi(argv[++i]);
        else if (argIs(argv[i], "--weather-cycle")) o.weatherCycle = true;
        else if (argIs(argv[i], "--no-weather-fx")) o.noWeatherFx = true;
        else if (argIs(argv[i], "--weather-cycle-phase") && i + 1 < argc)
            o.weatherCyclePhase = (float)atof(argv[++i]);
        else if (argIs(argv[i], "--view-distance") && i + 1 < argc)
            o.viewDistance = (float)atof(argv[++i]);
        else if (argIs(argv[i], "--no-collision")) o.noCollision = true;
        else if (argIs(argv[i], "--fullscreen")) o.fullscreen = true;
        else if (argIs(argv[i], "--obstacles") && i + 1 < argc) o.obstaclesOpen = (strcmp(argv[++i], "open") == 0);
        else if (argIs(argv[i], "--collision-probe") && i + 1 < argc) {
            int n = sscanf(argv[++i], "%f,%f,%f", &o.probeX, &o.probeZ, &o.probeR);
            if (n < 2) return 1;
            if (n < 3) o.probeR = 1.5f;
            o.collisionProbe = true;
        }
        else if (argIs(argv[i], "--buy") && i + 1 < argc) o.buyIndex = atoi(argv[++i]);
        else if (argIs(argv[i], "--accept-job") && i + 1 < argc) o.acceptJob = atoi(argv[++i]);
        else if (argIs(argv[i], "--deliver")) o.doDeliver = true;
        else if (argIs(argv[i], "--refuel")) o.doRefuel = true;
        else if (argIs(argv[i], "--repair")) o.doRepair = true;
        else if (argIs(argv[i], "--fuel") && i + 1 < argc) o.startFuel = (float)atof(argv[++i]);
        else if (argIs(argv[i], "--damage") && i + 1 < argc) o.startDamage = (float)atof(argv[++i]);
        else if (argIs(argv[i], "--traffic-models") && i + 1 < argc) o.trafficModels = atoi(argv[++i]);
        else if (argIs(argv[i], "--traffic-radius") && i + 1 < argc) o.trafficRadius = (float)atof(argv[++i]);
        else if (argIs(argv[i], "--traffic-report") && i + 1 < argc) {
            if (sscanf(argv[++i], "%f", &o.trafficReport) != 1) return 1;
        }
        else if (argIs(argv[i], "--traffic-report-oncoming")) o.trafficReportOncoming = true;
        else if (argIs(argv[i], "--road-probe")) {
            o.roadProbe = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.roadProbeStep = (float)atof(argv[++i]);
            if (o.roadProbeStep < 1.0f) o.roadProbeStep = 8.0f;
        }
        else if (argIs(argv[i], "--traffic-wheel-probe")) {
            o.trafficWheelProbe = 20.0f;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.trafficWheelProbe = (float)atof(argv[++i]);
            if (o.trafficWheelProbe < 1.0f) o.trafficWheelProbe = 20.0f;
        }
        else if (argIs(argv[i], "--ride-probe") && i + 1 < argc) {
            int n = sscanf(argv[++i], "%f,%f", &o.probeX, &o.probeZ);
            if (n < 2) return 1;
            o.rideProbe = true;
        }
        else if (argIs(argv[i], "--road-probe-site") && i + 1 < argc) {
            int n = sscanf(argv[++i], "%f,%f,%f", &o.roadProbeX, &o.roadProbeZ, &o.roadProbeR);
            if (n < 2) return 1;
            if (n < 3) o.roadProbeR = 200.0f;
            o.roadProbe = true;
            o.roadProbeSite = true;
        }
        else if (argIs(argv[i], "--parked-max") && i + 1 < argc) o.parkedMax = atoi(argv[++i]);
        else if (argIs(argv[i], "--hide-mat") && i + 1 < argc) o.hideMat = argv[++i];
        else if (argIs(argv[i], "--only-mat") && i + 1 < argc) o.onlyMat = argv[++i];
        else if (argIs(argv[i], "--only-name") && i + 1 < argc) o.onlyName = argv[++i];
        else if (argIs(argv[i], "--no-ground")) o.noGround = true;
        else if (argIs(argv[i], "--probe") && i + 1 < argc) {
            int n = sscanf(argv[++i], "%f,%f,%f", &o.probeX, &o.probeZ, &o.probeR);
            if (n < 2) return 1;
            if (n < 3) o.probeR = 120.0f;
            o.probe = true;
        }
        else if (argIs(argv[i], "--terrain")) o.drawTerrain = true;
        else if (argIs(argv[i], "--no-cull")) o.noCull = true;
        else if (argIs(argv[i], "--msg") && i + 1 < argc) o.msg = argv[++i];
        else if (argIs(argv[i], "--vehicle") && i + 1 < argc) { o.vehicle = argv[++i]; o.vehicleGiven = true; }
        else if (argIs(argv[i], "--tile") && i + 1 < argc) { o.allTiles = false; o.onlyTile = argv[++i]; }
        else if (argIs(argv[i], "--drive") && i + 1 < argc) { if (sscanf(argv[++i], "%f", &o.drive) != 1) return 1; }
        else if (argIs(argv[i], "--warmup") && i + 1 < argc) { if (sscanf(argv[++i], "%f", &o.warmup) != 1) return 1; }
        else if (argIs(argv[i], "--spawn") && i + 1 < argc) {
            int n = sscanf(argv[++i], "%f,%f,%f", &o.spawnOverride[0], &o.spawnOverride[1], &o.spawnOverride[2]);
            if (n < 2) return 1;
            o.spawnGiven = true;
        }
        else if (argIs(argv[i], "--movie")) o.movie = true;
        else if (argIs(argv[i], "--movie-clip") && i + 1 < argc) { o.movieName = argv[++i]; o.movie = true; }
        else if (argIs(argv[i], "--movie-time") && i + 1 < argc) { if (sscanf(argv[++i], "%f", &o.movieTime) != 1) return 1; }
        else if (argIs(argv[i], "--movie-frames") && i + 1 < argc) o.movieFrames = atoi(argv[++i]);
        else if (argIs(argv[i], "--movie-shot") && i + 1 < argc) {
            o.out = argv[++i]; o.screenshot = true; o.movie = true;
        }
        else if (argIs(argv[i], "--movie-sync-report")) { o.movieSyncReport = true; o.movie = true; }
        else if (argIs(argv[i], "--movie-sync-seconds") && i + 1 < argc) {
            if (sscanf(argv[++i], "%f", &o.movieSyncSeconds) != 1) return 1;
            o.movie = true;
        }
        else if (argIs(argv[i], "--no-video")) o.noVideo = true;
        else if (argIs(argv[i], "--no-make-video")) o.makeVideo = false;
        else if (argIs(argv[i], "--menu-music")) o.menuMusic = true;
        else if (argIs(argv[i], "--no-menu-music")) o.noMenuMusic = true;
        else if (argIs(argv[i], "--music") && i + 1 < argc) o.musicTrack = argv[++i];
        else if (argIs(argv[i], "--audio")) o.forceAudio = true;
        else if (argIs(argv[i], "--no-audio")) o.noAudio = true;
        else if (argIs(argv[i], "--loading-shot") && i + 1 < argc) o.loadingShot = argv[++i];
        else if (argIs(argv[i], "--loading-at") && i + 1 < argc) { if (sscanf(argv[++i], "%f", &o.loadingAt) != 1) return 1; }
        else if (argIs(argv[i], "--tex-update") && i + 1 < argc) o.texUpdate = atoi(argv[++i]);
        else if (argIs(argv[i], "--menu-act") && i + 1 < argc) o.menuAct = argv[++i];
        else if (argIs(argv[i], "--esc")) o.esc = true;
        else if (argIs(argv[i], "--esc-screen") && i + 1 < argc) o.escScreen = argv[++i];
        else if (argIs(argv[i], "--esc-shot") && i + 1 < argc) o.escShot = argv[++i];
        else if (argIs(argv[i], "--esc-act") && i + 1 < argc) o.escAct = argv[++i];
        else if (argIs(argv[i], "--ui-key") && i + 1 < argc) o.uiKey = argv[++i];
        // --ui-* are the in-game-panel spellings of the same three hooks
        // (docs/ui_sort.md): a panel is just an ESC screen of the Panel family.
        else if (argIs(argv[i], "--ui-panel") && i + 1 < argc) o.escScreen = argv[++i];
        else if (argIs(argv[i], "--ui-shot") && i + 2 < argc) {
            o.escScreen = argv[++i];
            o.escShot = argv[++i];
        }
        else if (argIs(argv[i], "--ui-act") && i + 1 < argc) o.escAct = argv[++i];
        else if (argIs(argv[i], "--map")) { o.mapGiven = true; o.noMap = false; }
        else if (argIs(argv[i], "--no-map")) o.noMap = true;
        else if (argIs(argv[i], "--job")) o.jobGiven = true;
        else if (argIs(argv[i], "--job-time") && i + 1 < argc) o.jobTime = argv[++i];
        else if (argIs(argv[i], "--fine-shot") && i + 1 < argc) {
            if (sscanf(argv[++i], "%d", &o.fineShot) != 1) return 1;
        }
        else if (argIs(argv[i], "--speed-limit") && i + 1 < argc) {
            if (sscanf(argv[++i], "%f", &o.speedFor) != 1) return 1;
            o.speedLimitGiven = true;
        }
        else if (argIs(argv[i], "--msg-shot") && i + 1 < argc) o.msgShot = argv[++i];
        else { fputs(USAGE, stderr); return 1; }
    }
    App app;
    return app.run(o);
}

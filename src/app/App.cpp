#include "App.h"
#include "../script/ScriptRules.h"
#include "../res/Res.h"
#include "../res/Mask.h"
#include "../script/Sdte.h"
#include "../util/Log.h"
#include <SDL.h>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdlib>

namespace ht2 {

// defined below: puts a model's lowest vertex on the ground, and the
// intact/dented material split of a vehicle container (see below).
static float modelGroundOffset(const std::vector<B3D::SubMesh>& subs);
static void dropDamagedParts(std::vector<B3D::SubMesh>& subs);

static const float PI = 3.14159265358979f;

int App::run(const Options& o) {
    opt_ = o;
    if (infoModes()) return 0;
    // The one-frame capture harnesses (--msg-shot / --fine-shot) are WORLD
    // frames: they render the strip, the map and the vehicle, so the state has
    // to be WORLD before the uploads below pick what to load (loadData() has
    // just chosen MENU from the default --scene).  Without this the HUD strip,
    // gated on State::World, was never loaded and the frame fell back to the 2D
    // dashboard over the bare terrain mesh.
    if (opt_.fineShot > 0 || !opt_.msgShot.empty()) state_ = State::World;
    // Audio is initialised for interactive runs; the offline modes (--screenshot,
    // --esc-shot, --drive, --probe, --scripts ...) stay silent unless
    // --audio/--menu-music asks for it, so nothing headless ever needs a working
    // sound device.
    const bool headless = opt_.screenshot || !opt_.escShot.empty() || opt_.drive > 0.0f ||
                          opt_.trafficReport > 0.0f;
    audioWanted_ = !opt_.noAudio && (!headless || opt_.forceAudio || opt_.menuMusic);
    videoAudioWanted_ = audioWanted_;
    if (!loadData()) return 2;
    // ... and again after loadData(), which re-picks the state from --scene.
    if (opt_.fineShot > 0 || !opt_.msgShot.empty()) state_ = State::World;
    if (!font_.initBuiltin(r_)) return 3;
    menu_.load(opt_.dataDir);
    if (!opt_.msg.empty()) menu_.setMessage(opt_.msg, 1e9f);
    menu_.update(opt_.time);   // deterministic screenshot framing

    r_.setTerrain(&terrain_);
    r_.setRoads(&way_);
    r_.setGroundMap(&ground_);
    r_.setUseRawTerrain(opt_.drawTerrain);
    r_.setCulling(!opt_.noCull);
    r_.setGroundEnabled(!opt_.noGround);
    if (opt_.autostart) state_ = State::Menu;
    if (state_ == State::Vehicle) r_.setScene(&trucks_, nullptr);
    else if (state_ == State::Menu) r_.setScene(&menuScene_, nullptr);
    else r_.setSceneMeshes(&worldMeshes_, nullptr);
    if (state_ == State::World) {
        // the drivable truck lives in COMMON/TRUCKS.B3D, assembled via its links
        haveTrucks_ = trucks_.load(opt_.dataDir + "/COMMON/TRUCKS.B3D");
        if (haveTrucks_ && !trucks_.buildNamedScene(opt_.vehicle))
            LOG("app: vehicle '%s' not found in TRUCKS.B3D", opt_.vehicle.c_str());
        // The container carries the dented skin as well; the player's truck is
        // drawn intact (its progressive damage skin is not wired to the model).
        if (haveTrucks_) dropDamagedParts(trucks_.submeshesMutable());
    }
    if (!r_.init(opt_.w, opt_.h, opt_.screenshot, opt_.fullscreen)) return 3;
    input_.init(opt_.dataDir);
    menu_.loadArt(r_);   // decode the original LOAD.RES button art into textures
    // original bitmap font (falls back to the built-in stroke font)
    if (!font_.loadOriginal(r_, opt_.dataDir))
        LOG("app: MENU.SFT/MENU.FON unavailable, using the built-in font");

    // --- audio (src/audio/Audio.cpp): guarded, headless modes stay silent ---
    if (audioWanted_) startAudio();

    // --- offline renders of the two new screens (tests / documentation) -----
    if (!opt_.loadingShot.empty()) {
        loadingShotRun();
        finish();
        r_.shutdown();
        return 0;
    }
    if (opt_.texUpdate >= 0) {
        const bool ok = texUpdateRun();
        finish();
        r_.shutdown();
        return ok ? 0 : 4;
    }
    // Textures need a live GL context, so load them after init() and rebuild the
    // scene batches with per-material texture bindings.
    if (state_ == State::Vehicle && haveTrucks_) {
        vehicleTex_.load(r_, opt_.dataDir + "/COMMON/TRUCKS");
        r_.setScene(&trucks_, &vehicleTex_);
    } else if (state_ == State::Menu && haveMenuScene_) {
        menuTex_.load(r_, opt_.dataDir + "/MENV/dr");
        r_.setScene(&menuScene_, &menuTex_);
    } else if (state_ == State::World) {
        for (const std::string& base : worldTileNames_) worldTex_.load(r_, base);
        worldTex_.load(r_, opt_.dataDir + "/COMMON/TRUCKS");   // parked car skins
        // Weather/prop materials of the original's COMMON.RES: rain, snow, drop
        // (the particles), sun_mat/moon_mat and the sky_mat* dome.  `noload`
        // entries (the nine 256x256 sky textures) are skipped.
        worldTex_.load(r_, opt_.dataDir + "/COMMON/COMMON", true);
        r_.setSceneMeshes(&worldMeshes_, &worldTex_);
        pickGroundTexture();
    hud_.load(r_, opt_.dataDir);
        if (haveTrucks_) {
            vehicleTex_.load(r_, opt_.dataDir + "/COMMON/TRUCKS");
            r_.setVehicleScene(&trucks_, &vehicleTex_);
            cockpit_.load(r_, opt_.dataDir, opt_.vehicle);   // COMMON/CABINES.B3D
            // Rear-view mirror art (COMMON/esc16.RES mirr*.msk) + reflection
            // target; a no-op (and no per-frame cost) when the art is missing.
            mirror_.load(r_, opt_.dataDir, &trucks_.anchorList, opt_.vehicle);
        }
        indexWayNodes();
        buildMinimapTexture();
        prepareNodeEvents();
        vehicle3d_.setGround(&ground_);
        vehicle3d_.reset(spawn_[0], spawn_[1], spawn_[2]);
        if (opt_.startFuel >= 0.0f) vehicle3d_.setFuel(opt_.startFuel);
        if (opt_.startDamage > 0.0f) vehicle3d_.addDamage(opt_.startDamage);
        if (opt_.traffic > 0) spawnTraffic();
        refreshPlayerBody();
        initEconomy();
        applyEconomyActions();
        // The original's left-hand road map: roads from every .way, the named
        // facilities from the `map*` node convention, the town names and the
        // petrol stations from the scripts (src/ui/Map.cpp).
        map_.load(r_, opt_.dataDir, waySegs_, wayNodes_);
        map_.addEconomy(economy_);
        map_.addGameData(gameData_);
        map_.setVisible(!opt_.noMap);
        if (opt_.mapGiven) map_.setForced(true);
        initSpeedLimits();
        LOG("app: vehicle '%s' spawned at %.0f %.0f y=%.1f (submeshes %zu)",
            opt_.vehicle.c_str(), spawn_[0], spawn_[1], vehicle3d_.y(), trucks_.submeshes().size());
    }

    // --- the intro movie, before the menu (--movie / the menu button) -------
    // (also before --autostart: the original boots into the menu, so the intro
    // and the menu track are part of that path)
    if (opt_.movie && opt_.drive > 0.0f)
        LOG("movie: --drive given, the intro is skipped (the drive test owns the run)");
    else if (opt_.movie && state_ == State::Menu) enterMovie(opt_.movieName);
    // --movie-sync-report: measure the sound-vs-picture drift headlessly (the
    // SDL dummy audio device works, so no sound card is needed).
    if (opt_.movieSyncReport) {
        const bool ok = movieSyncReportRun();
        finish();
        r_.shutdown();
        return ok ? 0 : 5;
    }
    // The menu track starts when the menu is actually up (the movie owns the
    // device while it plays); entering the world stops it again.
    if (state_ == State::Menu) startMenuMusic();

    // --menu-act ID: the same path a click/keyboard activation takes, for tests
    // (e.g. `--menu-act recor` plays the movie the ВИДЕОПЛЕЕР button plays).
    if (state_ == State::Menu && !opt_.menuAct.empty()) {
        LOG("menu: activate '%s' (--menu-act)", opt_.menuAct.c_str());
        if (audio_.ready()) audio_.playGlobal("pager", 0.6f);
        if (opt_.menuAct == "singl") enterWorld();
        else if (opt_.menuAct == "recor") {
            if (!enterMovie(opt_.movieName))
                menu_.setMessage("ФИЛЬМ НЕДОСТУПЕН (tools/make_video.sh)", 3.0f);
        } else menu_.setMessage("НЕ РЕАЛИЗОВАНО", 2.5f);
    }

    // --esc* implies the world (like --autostart, it runs the real transition);
    // the panel itself is opened after --warmup so the scripted activations act
    // on the final vehicle state.
    const bool escWanted = opt_.esc || !opt_.escScreen.empty() ||
                           !opt_.escAct.empty() || !opt_.escShot.empty();
    if (escWanted && state_ != State::World) enterWorld();

    if (opt_.autostart) enterWorld();
    if (opt_.warmup > 0.0f && state_ == State::World) {
        Vehicle::Input wi;
        wi.throttle = 1.0f;
        const float step = 1.0f / 60.0f;
        for (int i = 0; i < (int)(opt_.warmup / step); ++i) vehicle3d_.update(wi, step);
        LOG("app: warmup %.1fs -> pos (%.0f, %.0f) speed %.1f km/h",
            opt_.warmup, vehicle3d_.x(), vehicle3d_.z(), vehicle3d_.speedKmh());
        if (opt_.camGiven) {
            // keep the free camera on the truck for deterministic screenshots
            worldCam_[0] = vehicle3d_.x();
            worldCam_[2] = vehicle3d_.z();
            worldCam_[1] = std::max(worldCam_[1], vehicle3d_.y() + 10.0f);
        }
    }

    // --- in-game ESC screens (src/ui/EscMenu.cpp, docs/esc_impl.md) ---------
    // The panel art is decoded here because Renderer::init() has already run.
    if (escWanted) {
        esc_.load(opt_.dataDir);
        esc_.loadArt(r_);
        if (audio_.ready()) {          // seed the ЗВУК sliders from TRUCK.INI
            esc_.setSliderValue("voll", audio_.masterVolume());
            esc_.setSliderValue("musl", audio_.musicVolume());
            esc_.setSliderValue("engl", audio_.engineVolume());
            esc_.setSliderValue("skidl", audio_.sfxVolume());
        }
        esc_.open(opt_.escScreen.empty() ? "main" : opt_.escScreen);
        esc_.setSlotText(saveSummary());
        if (!opt_.escAct.empty()) runEscScript(opt_.escAct);
        if (!opt_.escShot.empty()) { opt_.out = opt_.escShot; opt_.screenshot = true; }
    }

    if (opt_.drive > 0.0f) {
        int rc = driveTest();
        finish();
        r_.shutdown();
        return rc;
    }
    if (opt_.trafficReport > 0.0f) {
        int rc = trafficReport();
        finish();
        r_.shutdown();
        return rc;
    }

    // --fine-shot SPEED / --msg-shot TEXT: deterministic single-frame captures
    // of the two new event windows (tests + the side-by-side shots).
    if (opt_.fineShot > 0) {
        optsFineShot();
        frame(0.0f, false);
        r_.screenshot(opt_.out);
        finish();
        r_.shutdown();
        return 0;
    }
    if (!opt_.msgShot.empty()) {
        optsMsgShot();
        if (getenv("HT2_MSG_GLYPHS")) {
            // Regression hook: the exact glyph-code string Hud::drawMessage will
            // hand to Font::draw for this pager line (see Font::glyphCodes).
            const std::string g = Font::glyphCodes(msgText_);
            std::string hx; char b[8];
            for (unsigned char c : g) { snprintf(b, sizeof b, "%02x ", c); hx += b; }
            LOG("msgglyphs: utf8 '%s' -> glyph codes [%s]", msgText_.c_str(), hx.c_str());
        }
        frame(0.0f, false);
        r_.screenshot(opt_.out);
        finish();
        r_.shutdown();
        return 0;
    }

    if (opt_.screenshot) {
        // A movie screenshot is deterministic: seek to --movie-time/--movie-frames
        // (decoded frame by frame) and leave that frame on screen.
        if (state_ == State::Movie) movieShotRun();
        frame(0.0f, false);
        r_.screenshot(opt_.out);
        finish();
        r_.shutdown();
        return 0;
    }

    Uint32 last = SDL_GetTicks();
    bool running = true;
    int prevMx = -1, prevMy = -1;
    while (running) {
        if (quitRequested_) break;      // SDL_QUIT during a loading screen
        SDL_Event e;
        // A short grace period at the start of a movie: the key/button that
        // started it (menu activation, key repeat) must not skip it again.
        auto canSkipMovie = [&]() { return video_.elapsed() > 0.35f; };
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            // --- in-game ESC screens own the input while they are up ---------
            // (the keyboard/gamepad navigation goes through the SDL events, so
            // the Input module's one-shot edges stay untouched)
            else if (e.type == SDL_CONTROLLERBUTTONDOWN && esc_.active()) {
                escSwallowMenu_ = true;   // pad START also raises Input::State::menu
                switch (e.cbutton.button) {
                    case SDL_CONTROLLER_BUTTON_DPAD_UP:    esc_.selectNext(-1); break;
                    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  esc_.selectNext(1); break;
                    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  esc_.adjustSelected(-1); break;
                    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: esc_.adjustSelected(1); break;
                    case SDL_CONTROLLER_BUTTON_A:
                    case SDL_CONTROLLER_BUTTON_START:      esc_.activateSelected(); break;
                    case SDL_CONTROLLER_BUTTON_B:
                    case SDL_CONTROLLER_BUTTON_BACK:       escBack_ = true; break;
                    default: break;
                }
            }
            else if (e.type == SDL_KEYDOWN) {
                if (state_ == State::Movie) {
                    if (canSkipMovie()) video_.skip();  // any key skips the movie
                } else if (esc_.active()) {
                    // Esc goes "back" (sub-screen -> ESC main -> resume driving),
                    // everything else navigates the open panel
                    if (e.key.keysym.sym == SDLK_ESCAPE) { escBack_ = true; escSwallowMenu_ = true; }
                    else esc_.handleKey(e.key.keysym.sym);
                } else if (e.key.keysym.sym == SDLK_ESCAPE) {
                    // in the world the menu edge comes from Input (Esc is mapped
                    // there); at the menu Esc quits the game
                    if (state_ != State::World) running = false;
                } else if (state_ == State::Menu) {
                    menu_.handleKey(e.key.keysym.sym);
                }
            } else if (e.type == SDL_MOUSEMOTION) {
                prevMx = e.motion.x; prevMy = e.motion.y;
                if (esc_.active())
                    esc_.handleMouse((float)prevMx, (float)prevMy, false, r_.width_, r_.height_);
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                prevMx = e.button.x; prevMy = e.button.y;
                if (state_ == State::Movie) {               // any button skips too
                    if (canSkipMovie()) video_.skip();
                }
                else if (esc_.active()) esc_.handleMouse((float)prevMx, (float)prevMy, true, r_.width_, r_.height_);
                else if (state_ == State::Menu) menu_.handleMouse((float)e.button.x, (float)e.button.y, true);
            }
        }
        if (state_ == State::Menu) {
            int mx, my;
            SDL_GetMouseState(&mx, &my);
            menu_.handleMouse((float)mx, (float)my, false);
        } else if (esc_.active()) {
            int mx, my;
            SDL_GetMouseState(&mx, &my);
            esc_.handleMouse((float)mx, (float)my, false, r_.width_, r_.height_);
        }
        Uint32 now = SDL_GetTicks();
        float dt = (now - last) / 1000.0f;
        last = now;
        if (dt > 0.1f) dt = 0.1f;

        if (state_ == State::Movie) {
            // gamepad/keyboard activity through the unified input skips too
            Input::State st = input_.poll(dt);
            if (st.any && canSkipMovie()) {
                LOG("movie: skipped by input");
                video_.skip();
            }
            if (!movieFrame()) leaveMovie();
        } else {
            menu_.update(dt);

            // --- in-game ESC screens ---------------------------------------
            if (esc_.active()) {
                esc_.update(dt);
                if (esc_.screen() == "sound") applyEscSliders();
                if (escBack_) { escBack_ = false; escAction(esc_.back()); }
                const std::string ea = esc_.takeActivated();
                if (!ea.empty()) {
                    LOG("esc: activate '%s' -> '%s'", esc_.lastButton().c_str(), ea.c_str());
                    escAction(ea);
                }
            } else if (escBack_) {
                escBack_ = false;
            }

            std::string act = menu_.takeActivated();
            if (state_ == State::Menu && !act.empty()) {
                LOG("menu: activate '%s'", act.c_str());
                if (audio_.ready()) audio_.playGlobal("pager", 0.6f);   // UI blip
                if (act == "exit") { running = false; }
                else if (act == "singl") { enterWorld(); }
                else if (act == "recor") {          // ВИДЕОПЛЕЕР
                    if (!enterMovie(opt_.movieName))
                        menu_.setMessage("ФИЛЬМ НЕДОСТУПЕН (tools/make_video.sh)", 3.0f);
                }
                else {
                    menu_.setMessage("НЕ РЕАЛИЗОВАНО", 2.5f);
                }
            }
        }

        frame(dt, true);
        SDL_GL_SwapWindow(r_.getWindowHandle());
    }
    finish();                        // releases the movie texture + the mixer
    r_.shutdown();
    LOG("bye");
    return 0;
}

bool App::infoModes() {
    if (!opt_.scriptDump.empty()) {
        FILE* f = fopen(opt_.scriptDump.c_str(), "rb");
        if (!f) { LOG("script-dump: cannot open %s", opt_.scriptDump.c_str()); return true; }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> raw((size_t)(n > 0 ? n : 0));
        if (!raw.empty()) { size_t got = fread(raw.data(), 1, raw.size(), f); (void)got; }
        fclose(f);
        std::vector<uint8_t> plain = Sdte::decrypt(raw.data(), raw.size());
        fwrite(plain.data(), 1, plain.size(), stdout);
        return true;
    }
    if (opt_.collisionProbe) {
        loadWorldTiles();
        Collision c;
        c.build(worldMeshes_);
        bool b = c.blocked(opt_.probeX, opt_.probeZ, opt_.probeR);
        LOG("collision-probe: (%.0f, %.0f) r=%.1f -> %s (%zu solid cells, %.1f m grid)",
            opt_.probeX, opt_.probeZ, opt_.probeR, b ? "BLOCKED" : "free", c.cells(), c.cellSize());
        return true;
    }
    if (opt_.probe) {
        loadWorldTiles();
        LOG("probe: %zu world submeshes, looking near (%.0f, %.0f) r=%.0f",
            worldMeshes_.size(), opt_.probeX, opt_.probeZ, opt_.probeR);
        size_t n = 0;
        for (const auto& sm : worldMeshes_) {
            float dx = sm.center[0] - opt_.probeX, dz = sm.center[1] - opt_.probeZ;
            float d = sqrtf(dx * dx + dz * dz);
            if (opt_.probeR > 0 && d > opt_.probeR) continue;
            if (opt_.probeR <= 0 && sm.material.compare(0, opt_.hideMat.size(), opt_.hideMat) != 0) continue;
            ++n;
            LOG("probe: d=%6.1f at(%8.1f,%8.1f) y=%8.1f r=%6.1f %-18s mat=%-20s verts=%4zu tris=%4zu",
                d, sm.center[0], sm.center[1], sm.center[2], sm.radius,
                sm.name.c_str(), sm.material.c_str(), sm.pos.size() / 3, sm.idx.size() / 3);
            if (n > 60) break;
        }
        LOG("probe: %zu hits", n);
        return true;
    }
    if (opt_.listVehicles) {
        GameData gd;
        gd.load(opt_.dataDir);
        LOG("vehicles: %zu entries, player = %s", gd.vehicles.size(), gd.playerVehicle.c_str());
        for (const auto& v : gd.vehicles)
            LOG("vehicle %-18s model=%-14s var=%d %-8s %-8s mass=%-7ld price=%ld",
                v.name.c_str(), v.model.c_str(), v.variant, v.klass.c_str(),
                v.aiKind.c_str(), v.mass, v.price);
        return true;
    }
    if (opt_.scripts) {
        GameData gd;
        bool ok = gd.load(opt_.dataDir);
        for (const auto& d : gd.documents())
            LOG("scripts: %-40s forms=%zu", d.path.c_str(), d.forms.size());
        for (const auto& e : gd.errorList()) LOG("scripts: ERROR %s", e.c_str());
        LOG("scripts: %s", gd.summary().c_str());
        const Sexp* f = gd.findForm("vehicle");
        for (size_t i = 0; i < gd.vehicles.size() && i < 5; ++i) {
            const auto& v = gd.vehicles[i];
            LOG("scripts: vehicle %-14s model=%-12s variant=%d class=%-8s ai=%-8s mass=%ld price=%ld",
                v.name.c_str(), v.model.c_str(), v.variant, v.klass.c_str(),
                v.aiKind.c_str(), v.mass, v.price);
        }
        for (const auto& kv : gd.upgrades)
            LOG("scripts: upgrade %-26s %ld", kv.first.c_str(), kv.second);
        (void)f;
        return true;
    }
    if (opt_.scriptReport) {
        // The recovered engine rules of src/script/ScriptRules.{h,cpp}: the four
        // `people` condition kinds, the node-event direction/8 s guard, the
        // obstacle and ambush activation bands and the restable tuning table.
        GameData gd;
        gd.load(opt_.dataDir);
        ScriptRuntime rt(gd);
        std::vector<ScriptNodePos> nodes;                 // name -> (x, z) of ENV/*.way
        DIR* dp = opendir((opt_.dataDir + "/ENV").c_str());
        if (dp) {
            std::vector<std::string> ways;
            while (struct dirent* e = readdir(dp)) {
                std::string n = e->d_name;
                if (n.size() > 4 && n.compare(n.size() - 4, 4, ".way") == 0)
                    ways.push_back(opt_.dataDir + "/ENV/" + n);
            }
            closedir(dp);
            std::sort(ways.begin(), ways.end());
            for (const auto& p : ways) {
                Way w;
                if (!w.load(p)) continue;
                for (const auto& room : w.rooms)
                    for (const auto& nd : room.nodes)
                        if (!nd.name.empty()) nodes.push_back({nd.name, nd.pos.x, nd.pos.z});
            }
        }
        rt.setNodePositions(nodes);
        ScriptWorldState st;
        st.hour = opt_.startHour;                        // --hour (default 12)
        st.weather = 0.0f;                               // clear; see ScriptWorldState
        st.money = 45000;
        st.damage = 0.0f;
        st.time = 1000.0;
        rt.update(st);
        LOG("%s", rt.reportText().c_str());
        if (opt_.scriptCoverage)
            for (const auto& l : {rt.coverageTable()}) LOG("%s", l.c_str());
        return true;
    }
    if (opt_.b3dinfo) {
        const char* files[] = {"/MENV/dr.b3d", "/ENV/aa.b3d", "/MTR/tr.b3d", "/COMMON/TRUCKS.B3D"};
        for (const char* f : files) {
            B3D b;
            std::string p = opt_.dataDir + f;
            if (!b.load(p)) { LOG("b3dinfo: %s FAILED", p.c_str()); continue; }
            auto hist = b.subtagHistogram();
            std::string subs;
            char buf[64];
            for (size_t i = 0; i < hist.size(); ++i)
                if (hist[i]) { snprintf(buf, sizeof buf, "%zu:%u ", i, hist[i]); subs += buf; }
            size_t meshes = b.meshes().size();
            size_t verts = 0, polys = 0;
            for (auto& m : b.meshes()) { verts += m.vcount; }
            b.buildScene();
            polys = b.submeshes().size();
            LOG("b3dinfo: %s OK %.2f%% consumed=%zu/%zu names=%zu top=%zu meshes=%zu verts=%zu submeshes=%zu",
                p.c_str(), 100.0 * (double)b.consumed() / (double)(b.total() ? b.total() : 1),
                b.consumed(), b.total(), b.names().size(), b.tree().size(), meshes, verts, polys);
            LOG("b3dinfo:   subs=%s", subs.c_str());
        }
        return true;
    }
    if (opt_.resinfo || opt_.resdump) {
        std::vector<std::string> files;
        scanRes(opt_.dataDir, files);
        std::sort(files.begin(), files.end());
        size_t nMask = 0, nTex = 0, okMask = 0, okTex = 0, badMask = 0, badTex = 0;
        for (const auto& p : files) {
            Res res;
            if (!res.load(p)) continue;
            for (auto& e : res.entries()) {
                DecodedImage img;
                bool decoded = false;
                if (e.kind == Res::Kind::Mask) {
                    ++nMask;
                    decoded = Mask::decode(res.data(e), e.size, img);
                    if (decoded && img.consumed == img.total) ++okMask; else ++badMask;
                } else if (e.kind == Res::Kind::Texture) {
                    ++nTex;
                    decoded = Texture::decode(res.data(e), e.size, img);
                    if (decoded) ++okTex; else ++badTex;
                } else continue;
                if (opt_.resdump)
                    LOG("resdump: %-28s %-18s %4dx%-4d %-16s consumed=%zu/%zu %s",
                        e.name.c_str(), e.tag.c_str(), img.width, img.height,
                        img.format.c_str(), img.consumed, e.size,
                        decoded ? "OK" : "FAIL");
            }
        }
        LOG("resdump: files=%zu masks=%zu (%zu exact, %zu bad) textures=%zu (%zu ok, %zu undecoded)",
            files.size(), nMask, okMask, badMask, nTex, okTex, badTex);
        return true;
    }
    return false;
}

void App::scanRes(const std::string& dir, std::vector<std::string>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (struct dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        std::string p = dir + "/" + n;
        struct stat st;
        if (stat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) scanRes(p, out);
        else {
            std::string lower = n;
            for (auto& c : lower) c = (char)tolower((unsigned char)c);
            if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".res") == 0)
                out.push_back(p);
        }
    }
    closedir(d);
}

// Merge every ENV/*.b3d tile into one submesh list so the whole island is
// drivable (each tile carries its own anchors/links, already resolved by B3D).
// The generated terrain mesh fills the gaps between the tiles; give it one of
// the tiles' own ground textures instead of a flat colour.
void App::pickGroundTexture() {
    if (!worldTex_.count()) return;
    for (const char* cand : {"grass1", "grass0", "grass2", "grass_1", "ground", "dirt",
                             "ter_road", "mat_grass1", "mat_grass_1", "mat_grass2"}) {
        unsigned t = worldTex_.textureFor(cand);
        if (t) { r_.setGroundTexture(t); LOG("world: ground texture '%s'", cand); return; }
    }
}

// Every ENV tile has its own .way road graph; the mod_*.sch scripts reference the
// named nodes across all of them, so the whole set is indexed once.
void App::indexWayNodes() {
    if (!wayNodes_.empty()) return;
    for (const std::string& base : worldTileNames_) {
        Way w;
        if (!w.load(base + ".way")) continue;
        for (const auto& room : w.rooms)
            for (const auto& n : room.nodes)
                if (!n.name.empty()) wayNodes_[n.name] = n;
        for (const auto& sg : w.segments()) waySegs_.push_back(sg);
    }
    LOG("gameplay: %zu named .way nodes, %zu road segments indexed",
        wayNodes_.size(), waySegs_.size());
}

// Parked cars: the scripts name a .way node per bay and a role; the model comes
// from etrashboxes.sch (all 56 parked entries share the id PARKED_X, so the
// <model, variant> pair is what selects them).
void App::spawnParkedVehicles() {
    if (gameData_.parkings.empty() || gameData_.parked.empty()) return;
    if (!parked_.load(opt_.dataDir + "/COMMON/TRUCKS.B3D")) {
        LOG("gameplay: no COMMON/TRUCKS.B3D, no parked vehicles");
        return;
    }
    indexWayNodes();
    // The models are heavy (a parked car is ~8.5k triangles), so only the bays
    // near the player are realised; the rest are left for streaming later.
    const float radius = opt_.parkedRadius;
    size_t spawned = 0, missing = 0, far = 0;
    std::vector<B3D::SubMesh> extra;
    size_t pick = 0;
    for (const auto& pk : gameData_.parkings) {
        for (const auto& bay : pk.bays) {
            if (bay.role == 4) continue;                 // trade stall, not a car
            auto it = wayNodes_.find(bay.name);
            if (it == wayNodes_.end()) { ++missing; continue; }
            float ddx = (float)it->second.pos.x - spawn_[0];
            float ddz = (float)it->second.pos.z - spawn_[1];
            if (radius > 0.0f && ddx * ddx + ddz * ddz > radius * radius) { ++far; continue; }
            if (opt_.parkedMax > 0 && (int)spawned >= opt_.parkedMax) break;
            const GameData::ParkedDef& def = gameData_.parked[pick++ % gameData_.parked.size()];
            if (!parked_.buildNamedScene(def.model)) continue;
            { std::vector<B3D::SubMesh> subs = parked_.submeshes(); dropDamagedParts(subs);
              parked_.submeshesMutable().swap(subs); }
            const float px = (float)it->second.pos.x;
            const float pz = (float)it->second.pos.z;
            const float ph = (float)it->second.pos.y;
            // heading: follow the nearest road segment, so cars line up with the road
            float yaw = 0.0f, best = 80.0f * 80.0f;
            for (const auto& sg : waySegs_) {
                for (size_t k = 0; k + 1 < sg.pts.size(); ++k) {
                    float ax = (float)sg.pts[k].x, az = (float)sg.pts[k].z;
                    float bx = (float)sg.pts[k + 1].x, bz = (float)sg.pts[k + 1].z;
                    float mx = 0.5f * (ax + bx), mz = 0.5f * (az + bz);
                    float d = (mx - px) * (mx - px) + (mz - pz) * (mz - pz);
                    if (d < best) {
                        best = d;
                        yaw = atan2f(bx - ax, bz - az);
                    }
                }
            }
            const float cs = cosf(yaw), sn = sinf(yaw);
            const float yoff = modelGroundOffset(parked_.submeshes());
            for (auto& sm : parked_.submeshesMutable()) {
                B3D::SubMesh t = sm;
                for (size_t v = 0; v + 2 < t.pos.size(); v += 3) {
                    float lx = t.pos[v], lz = t.pos[v + 1], lh = t.pos[v + 2];
                    t.pos[v]     = px + lx * cs + lz * sn;
                    t.pos[v + 1] = pz - lx * sn + lz * cs;
                    t.pos[v + 2] = ph + yoff + lh;
                }
                // The batch bounding sphere is built from `center` + `radius`
                // and the renderer culls whole batches with it: the center has
                // to be transformed with the *original* local values (using the
                // already-rotated center[0] for center[1] put the sphere
                // kilometres away and culled the car away entirely).
                const float clx = t.center[0], clz = t.center[1];
                t.center[0] = px + clx * cs + clz * sn;
                t.center[1] = pz - clx * sn + clz * cs;
                t.center[2] = ph + yoff + t.center[2];
                extra.push_back(std::move(t));
            }
            ++spawned;
        }
    }
    size_t tris = 0;
    for (const auto& sm : extra) tris += sm.idx.size() / 3;
    LOG("gameplay: %zu parked vehicles spawned (%zu far, %zu bays without a .way node), "
        "%zu submeshes, %zu tris", spawned, far, missing, extra.size(), tris);
    for (auto& sm : extra) worldMeshes_.push_back(std::move(sm));
}

// The scripts are cp1251 while the bitmap font takes UTF-8 (it maps the
// Cyrillic block back to the game's code page), so strings coming from a .sch
// have to be converted before they are drawn.
// The game's script strings are raw cp1251 bytes; Font owns the one conversion
// (it must accept both the data and the sources' UTF-8 literals).
static std::string cp1251ToUtf8(const std::string& in) { return Font::toUtf8Text(in); }

// The models' origins are not at the wheel contact plane: the Zil's bbox starts
// at -1.0 while a car's may start at 0.  Placing a model at the ground height
// therefore makes some hover and others sink, so each model's lowest vertex is
// used as the offset.
static float modelGroundOffset(const std::vector<B3D::SubMesh>& subs) {
    float lo = 1e30f;
    for (const auto& sm : subs)
        for (size_t i = 0; i + 2 < sm.pos.size(); i += 3)
            lo = std::min(lo, sm.pos[i + 2]);
    return (lo < 1e29f) ? -lo : 0.0f;
}

// Damage in COMMON/TRUCKS.B3D is a *material* swap, not a different container:
// every body panel exists twice inside one vehicle container -- the intact skin
// ("Fiat_all1", "BMW_all1", "volga_all2", ...) and the dented one
// ("Fiat_alld1", "BMW_alld1", ...) -- plus a few damaged-only parts such as
// "common_BackLightsDmg".  B3D::buildNamedScene() includes *everything*, so an
// undamaged car drew both skins on top of each other and looked pre-wrecked.
// The materials are split here; the app uploads one batch per skin and picks it
// from Car::damage (0 = intact), and the player/parked vehicles keep the intact
// one only.
static std::string lowerMat(const std::string& s) {
    std::string r = s;
    for (char& c : r)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return r;
}
static bool damagedMaterial(const std::string& m) {
    const std::string l = lowerMat(m);
    if (l.find("alld") != std::string::npos) return true;     // Fiat_alld1, BMW_alld1
    if (l.find("damage") != std::string::npos) return true;   // Z_DAMAGE1..5
    if (l.size() >= 3 && l.compare(l.size() - 3, 3, "dmg") == 0) return true;
    return false;                                             // common_BackLightsDmg
}
// The intact body skin: the twin of the "alld" material.
static bool intactSkinMaterial(const std::string& m) {
    const std::string l = lowerMat(m);
    return l.find("_all") != std::string::npos || l.rfind("all") == 0;
}
// Drop the damaged materials from a built scene (player, parked vehicles).
static void dropDamagedParts(std::vector<B3D::SubMesh>& subs) {
    size_t before = subs.size();
    std::vector<B3D::SubMesh> keep;
    keep.reserve(before);
    for (auto& sm : subs)
        if (!damagedMaterial(sm.material)) keep.push_back(std::move(sm));
    subs.swap(keep);
    if (before != subs.size())
        LOG("traffic: dropped %zu damaged material submeshes of %zu",
            before - subs.size(), before);
}

// The car body for the contacts comes from the model's own bounding box:
// length = the bbox extent along +z (the forward axis the parked/vehicle code
// already assumes), radius = half the extent along x.  Returns the ground
// offset as well, so one walk over the vertices gives everything.
static void modelBody(const std::vector<B3D::SubMesh>& subs, float& length, float& radius,
                      float& groundOffset) {
    float lx = 1e30f, hx = -1e30f, lz = 1e30f, hz = -1e30f, lo = 1e30f;
    for (const auto& sm : subs) {
        for (size_t i = 0; i + 2 < sm.pos.size(); i += 3) {
            lx = std::min(lx, sm.pos[i]);     hx = std::max(hx, sm.pos[i]);
            lz = std::min(lz, sm.pos[i + 1]); hz = std::max(hz, sm.pos[i + 1]);
            lo = std::min(lo, sm.pos[i + 2]);
        }
    }
    const bool any = lx < 1e29f && lz < 1e29f;
    length = any ? std::max(1.0f, hz - lz) : 4.5f;
    radius = any ? std::max(0.5f, 0.5f * (hx - lx)) : 1.1f;
    groundOffset = (lo < 1e29f) ? -lo : 0.0f;
}

// ---------------------------------------------------------------------------
// AI traffic: the road graph is every RSEG polyline of every tile, welded at
// exactly coincident endpoints (verified: the shipped endpoints coincide).
// ---------------------------------------------------------------------------
// Node events from mod_*.sch: each names a .way node; when the player drives
// inside the trigger radius the message from the script is shown.  STATEBORDER
// events carry no text (they mark a border crossing).
void App::prepareNodeEvents() {
    if (!events_.empty() || gameData_.nodeEvents.empty()) return;
    indexWayNodes();
    events_.resize(gameData_.nodeEvents.size());
    size_t resolved = 0;
    for (size_t i = 0; i < gameData_.nodeEvents.size(); ++i) {
        auto it = wayNodes_.find(gameData_.nodeEvents[i].wayNode);
        if (it == wayNodes_.end()) continue;
        events_[i].x = (float)it->second.pos.x;
        events_[i].z = (float)it->second.pos.z;
        events_[i].has = true;
        ++resolved;
    }
    size_t withText = 0;
    for (const auto& ne : gameData_.nodeEvents) if (!ne.text.empty()) ++withText;
    LOG("events: %zu node events (%zu carry a message), %zu nodes resolved",
        events_.size(), withText, resolved);
}

void App::updateNodeEvents(float dt) {
    if (events_.empty() || gameData_.nodeEvents.empty()) return;
    if (msgTimer_ > 0.0f) { msgTimer_ -= dt; msgAge_ += dt; }
    const float px = vehicle3d_.x(), pz = vehicle3d_.z();
    const float r2 = opt_.eventRadius * opt_.eventRadius;
    for (size_t i = 0; i < events_.size() && i < gameData_.nodeEvents.size(); ++i) {
        if (!events_[i].has) continue;
        float dx = events_[i].x - px, dz = events_[i].z - pz;
        float d2 = dx * dx + dz * dz;
        if (events_[i].armed && d2 <= r2) {
            events_[i].armed = false;
            const std::string& t = gameData_.nodeEvents[i].text;
            if (!t.empty()) {
                // The original routes the script text through its 4-line message
                // ring and shows it in the strip's pager window with the blinking
                // "new message" icon (docs/original_ui.md 2.4).
                pushMessage(cp1251ToUtf8(t), "node event " + gameData_.nodeEvents[i].id);
                LOG("events: '%s' (%s) at %.0f %.0f", gameData_.nodeEvents[i].id.c_str(),
                    gameData_.nodeEvents[i].kind.c_str(), px, pz);
            }
        } else if (!events_[i].armed && d2 > r2 * 2.5f) {
            events_[i].armed = true;              // re-arm once well past it
        }
    }
}

// --- the pager window -------------------------------------------------------
// One entry point for every in-game message: the script node events, the
// ambush/police text of Ms.sch and the script messages (GameData::messages).
void App::pushMessage(const std::string& text, const std::string& kind) {
    if (text.empty()) return;
    msgText_ = text;
    msgTimer_ = 6.0f;
    msgAge_ = 0.0f;
    msgNew_ = true;
    LOG("message: '%s' (%s)", text.c_str(), kind.c_str());
}

// The script messages the original shows in the strip: the ambush and radio
// texts of Ms.sch, whose keys the shipped data uses for exactly these events.
void App::pushScriptMessage(const std::string& key) {
    auto it = gameData_.messages.find(key);
    if (it == gameData_.messages.end() || it->second.empty()) return;
    pushMessage(cp1251ToUtf8(it->second), "script " + key);
}


// AI traffic lives in src/game/Traffic.cpp: the graph is the RSEG network of
// every .way, the named RNOD nodes are the waypoints/destinations, cars keep to
// the right-hand lane and route with a reverse-Dijkstra field.
void App::spawnTraffic() {
    if (opt_.traffic <= 0) return;
    if (!trafficB3d_.loaded() && !trafficB3d_.load(opt_.dataDir + "/COMMON/TRUCKS.B3D")) {
        LOG("traffic: COMMON/TRUCKS.B3D unavailable");
        return;
    }
    indexWayNodes();
    if (waySegs_.empty()) return;
    traffic_.setSeed(0x51ED2701u);
    traffic_.build(waySegs_, wayNodes_);
    const char* wanted[] = {"BmwM5", "Fiat", "Volga", "Gazelle", "Zil", "Kamaz", "Scania", "Van"};
    std::vector<int> ids;
    trafficYOff_.clear();
    trafficIntact_.clear();
    trafficDamaged_.clear();
    for (const char* model : wanted) {
        if ((int)ids.size() >= opt_.trafficModels) break;
        if (!trafficB3d_.buildNamedScene(model)) continue;
        float len = 4.5f, rad = 1.1f, off = 0.0f;
        modelBody(trafficB3d_.submeshes(), len, rad, off);
        // Split the container into the intact skin, the dented skin and the
        // parts both cars share (wheels, glass, engine, lights).
        std::vector<B3D::SubMesh> common, skin, dents;
        for (auto& sm : trafficB3d_.submeshes()) {
            if (damagedMaterial(sm.material)) dents.push_back(sm);
            else if (intactSkinMaterial(sm.material)) skin.push_back(sm);
            else common.push_back(sm);
        }
        std::vector<B3D::SubMesh> intact = common;
        intact.insert(intact.end(), skin.begin(), skin.end());
        std::vector<B3D::SubMesh> wrecked = common;
        wrecked.insert(wrecked.end(), dents.begin(), dents.end());
        const int idIntact = r_.addTrafficModel(intact, &vehicleTex_);
        const int idDented = dents.empty() ? idIntact
                                           : r_.addTrafficModel(wrecked, &vehicleTex_);
        if (idIntact >= 0) {
            ids.push_back((int)ids.size());           // model *index*, not batch id
            trafficYOff_.push_back(off);
            trafficIntact_.push_back(idIntact);
            trafficDamaged_.push_back(idDented);
            traffic_.setBody((int)ids.size() - 1, len, rad);   // capsule for contacts
            LOG("traffic: model %-10s body %.2f x %.2f m, ground offset %.2f, "
                "skin %zu/%zu tris (intact/dented)",
                model, (double)len, (double)(2.0f * rad), (double)off,
                (size_t)(skin.size()), (size_t)(dents.size()));
        }
    }
    if (ids.empty()) { LOG("traffic: no models available"); return; }
    traffic_.setModels(ids);
    traffic_.spawn(vehicle3d_.x(), vehicle3d_.z(), opt_.trafficRadius, opt_.traffic);
    const Traffic::Stats& st = traffic_.stats();
    LOG("traffic: graph %zu v / %zu e, waypoints %zu/%zu, %zu cars (%zu models)",
        traffic_.vertices(), traffic_.edges(), traffic_.waypoints(), traffic_.waypointNames(),
        traffic_.cars().size(), ids.size());
    (void)st;
}

// The player's contact body: the bbox of the truck that is actually drawn, with
// the Vehicle constants as the fallback when the mesh is not available.
void App::refreshPlayerBody() {
    playerRadius_ = vehicle3d_.halfWidth();
    playerLength_ = 2.0f * vehicle3d_.halfLength();
    if (haveTrucks_ && !trucks_.submeshes().empty()) {
        float len = playerLength_, rad = playerRadius_, off = 0.0f;
        modelBody(trucks_.submeshes(), len, rad, off);
        playerLength_ = len;
        playerRadius_ = rad;
    }
    LOG("traffic: player body %.2f x %.2f m", (double)playerLength_,
        (double)(2.0f * playerRadius_));
}

// What the AI did to the player this frame: the pushback out of every car it
// touched, the damage both sides took and the crash one-shot above the same
// speed threshold the world collisions use.
void App::applyTrafficHits(float dt) {
    if (crashCooldown_ > 0.0f) crashCooldown_ -= dt;
    const Traffic::PlayerHit& hit = traffic_.playerHit();
    if (!hit.hit) return;
    vehicle3d_.nudge(hit.pushX, hit.pushZ);
    if (hit.damage > 0.0f) vehicle3d_.addDamage(hit.damage);
    if (hit.impact * 3.6f >= crashSpeed_ && crashCooldown_ <= 0.0f) {
        crashCooldown_ = 1.2f;
        ++carCrashes_;
        if (audio_.ready()) {
            const Audio::Vec3 at{vehicle3d_.x(), vehicle3d_.y(), vehicle3d_.z()};
            audio_.play("snd\\crash9.wav", at, 0.8f);
        }
        LOG("traffic: car impact %.0f km/h, player damage %.2f, car damage %.2f",
            hit.impact * 3.6, vehicle3d_.damage(),
            traffic_.carDamage(traffic_.contacts().empty() ? -1 : traffic_.contacts()[0].a));
    }
}

void App::drawTraffic(float cx, float cy, float cz, float yaw, float pitch) {
    const std::vector<Traffic::Car>& cars = traffic_.cars();
    if (cars.empty()) return;
    r_.beginTraffic(cx, cy, cz, yaw, pitch);
    for (const auto& c : cars) {
        // The traffic height comes from the road graph itself (the .way node
        // heights, `c.h`): the ground map takes the *lowest* vertex per cell, so
        // on a bridge it holds the water/terrain below the deck and the cars sank
        // through it.  The ground map is only a fallback when the graph has no
        // height for the car.
        float y = (c.h != 0.0f) ? c.h : (ground_.empty() ? 0.0f : ground_.heightAt(c.x, c.z, 0.0f));
        int batch = -1;
        if (c.model >= 0 && c.model < (int)trafficYOff_.size()) {
            const size_t mi = (size_t)c.model;
            y += trafficYOff_[mi];
            batch = (c.damage >= kDamageSkin && trafficDamaged_[mi] >= 0)
                        ? trafficDamaged_[mi] : trafficIntact_[mi];
        }
        if (batch < 0) continue;
        Mat4 m = Mat4::translation(c.x, y, c.z) * Mat4::rotY(c.yaw);
        r_.drawTrafficInstance(batch, m);
    }
}

void App::buildMinimapTexture() {
    if (minimapTex_ || waySegs_.empty()) return;
    float x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
    for (const auto& sg : waySegs_)
        for (const auto& p : sg.pts) {
            x0 = std::min(x0, (float)p.x); x1 = std::max(x1, (float)p.x);
            z0 = std::min(z0, (float)p.z); z1 = std::max(z1, (float)p.z);
        }
    const int N = 1024;
    mapX0_ = x0; mapZ0_ = z0;
    mapW_ = std::max(1.0f, x1 - x0);
    mapH_ = std::max(1.0f, z1 - z0);
    std::vector<uint8_t> px((size_t)N * N * 4, 0);
    auto put = [&](int ix, int iz, int r, int g, int b) {
        if (ix < 0 || iz < 0 || ix >= N || iz >= N) return;
        size_t o = ((size_t)iz * N + ix) * 4;
        px[o] = (uint8_t)r; px[o + 1] = (uint8_t)g; px[o + 2] = (uint8_t)b; px[o + 3] = 255;
    };
    // roads: 2 px wide, brighter for the wider (main) roads
    for (const auto& sg : waySegs_) {
        int shade = sg.width >= 12.0 ? 235 : 170;
        for (size_t k = 0; k + 1 < sg.pts.size(); ++k) {
            float ax = ((float)sg.pts[k].x - mapX0_) / mapW_ * (N - 1);
            float az = ((float)sg.pts[k].z - mapZ0_) / mapH_ * (N - 1);
            float bx = ((float)sg.pts[k + 1].x - mapX0_) / mapW_ * (N - 1);
            float bz = ((float)sg.pts[k + 1].z - mapZ0_) / mapH_ * (N - 1);
            int steps = (int)std::max(fabsf(bx - ax), fabsf(bz - az)) + 1;
            for (int s = 0; s <= steps; ++s) {
                float t = (float)s / steps;
                int ix = (int)(ax + (bx - ax) * t), iz = (int)(az + (bz - az) * t);
                for (int d = 0; d < 2; ++d) { put(ix + d, iz, shade, shade - 40, 90); put(ix, iz + d, shade, shade - 40, 90); }
            }
        }
    }
    // towns / named waypoints as small squares
    for (const auto& kv : wayNodes_) {
        int ix = (int)((kv.second.pos.x - mapX0_) / mapW_ * (N - 1));
        int iz = (int)((kv.second.pos.z - mapZ0_) / mapH_ * (N - 1));
        for (int dx = -1; dx <= 1; ++dx)
            for (int dz = -1; dz <= 1; ++dz) put(ix + dx, iz + dz, 250, 240, 180);
    }
    minimapTex_ = r_.createTexture(px.data(), N, N);
    LOG("ui: minimap %dx%d over x %.0f..%.0f z %.0f..%.0f", N, N, x0, x1, z0, z1);
}

// Speedometer/tachometer style gauge: an arc of ticks plus a needle.
void App::drawGauge(float cx, float cy, float rad, float frac, int ticks, float maxVal,
                    const char* label, float redlineFrom) {
    unsigned white = r_.whiteTex();
    const float a0 = -215.0f, a1 = 35.0f;             // degrees, 0 = east
    auto ang = [&](float t) { return (a0 + (a1 - a0) * t) * 3.14159265f / 180.0f; };
    for (int i = 0; i <= ticks; ++i) {
        float t = (float)i / ticks;
        float a = ang(t);
        float len = (i % 2 == 0) ? 9.0f : 5.0f;
        float sx = cx + cosf(a) * (rad - len), sy = cy + sinf(a) * (rad - len);
        float ex = cx + cosf(a) * rad, ey = cy + sinf(a) * rad;
        bool red = (float)i / ticks * maxVal >= redlineFrom;
        r_.line2D(white, sx, sy, ex, ey, 2.0f, red ? 0.95f : 0.85f, red ? 0.25f : 0.85f, 0.3f, 1.0f);
    }
    // needle
    float a = ang(std::min(1.0f, std::max(0.0f, frac)));
    r_.line2D(white, cx, cy, cx + cosf(a) * (rad - 12.0f), cy + sinf(a) * (rad - 12.0f), 3.0f,
              1.0f, 0.35f, 0.2f, 1.0f);
    r_.quad2D(white, cx - 4, cy - 4, 8, 8, 0.75f, 0.75f, 0.8f, 1.0f);
    if (label && *label)
        font_.draw(r_, label, cx - 4.0f * (float)strlen(label), cy + rad * 0.45f, 1.0f,
                   0.75f, 0.78f, 0.85f, 1.0f);
}

void App::drawMinimap(float x, float y, float size) {
    if (!minimapTex_) return;
    unsigned white = r_.whiteTex();
    const float span = 1400.0f;                        // world units shown
    float px = vehicle3d_.x(), pz = vehicle3d_.z();
    float u0 = (px - span * 0.5f - mapX0_) / mapW_;
    float v0 = (pz - span * 0.5f - mapZ0_) / mapH_;
    float du = span / mapW_, dv = span / mapH_;
    r_.quad2D(white, x - 2, y - 2, size + 4, size + 4, 0.05f, 0.07f, 0.10f, 0.85f);
    r_.quad2D(minimapTex_, x, y, size, size, 1.0f, 1.0f, 1.0f, 0.95f, u0, v0, u0 + du, v0 + dv);
    auto toPix = [&](float wx, float wz, float& ox, float& oy) {
        ox = x + (wx - (px - span * 0.5f)) / span * size;
        oy = y + (wz - (pz - span * 0.5f)) / span * size;
    };
    // traffic
    for (const auto& c : traffic_.cars()) {
        float ox, oy;
        toPix(c.x, c.z, ox, oy);
        if (ox < x || oy < y || ox > x + size || oy > y + size) continue;
        r_.quad2D(white, ox - 1.5f, oy - 1.5f, 3, 3, 0.95f, 0.85f, 0.3f, 1.0f);
    }
    // player: a triangle rotated by the heading
    {
        float cs = cosf(-vehicle3d_.yaw() * PI / 180.0f), sn = sinf(-vehicle3d_.yaw() * PI / 180.0f);
        float cxp = x + size * 0.5f, cyp = y + size * 0.5f;
        const float tri[6] = {0, -7, -5, 6, 5, 6};
        float xy[8], uv[8] = {0, 0, 1, 0, 1, 1, 0, 1};
        for (int i = 0; i < 4; ++i) {
            float tx = tri[(i % 3) * 2], ty = tri[(i % 3) * 2 + 1];
            xy[i * 2] = cxp + tx * cs - ty * sn;
            xy[i * 2 + 1] = cyp + tx * sn + ty * cs;
        }
        r_.pushQuad2D(white, xy, uv, 1.0f, 0.3f, 0.25f, 1.0f);
    }
}

// The original's road map: the panel geometry lives in src/ui/Map.cpp and is
// authored in the same 800-px UI space as the HUD strip, so it lands where the
// reference frames put it at every resolution.  The map also drives the
// «ПОЗИЦИЯ» cell's route highlight (the blue rectangle).
void App::drawMapOverlay() {
    if (!map_.loaded()) return;
    // The original shows the map while driving; a screenshot pass that is not
    // interactive still may want it (--map), and --no-map suppresses it.
    // The original shows the map while driving.  A screenshot pass is not
    // interactive, so there the panel is only drawn when it was asked for
    // (--map, the fine/message captures) - otherwise the UI tests that compare
    // the world under a panel would all move.
    if (!map_.visible()) return;
    if (!interactivePass_ && !map_.forced()) return;
    map_.update(vehicle3d_.x(), vehicle3d_.z(), vehicle3d_.yaw());
    // The panel keeps the reference aspect (268x434) and is scaled uniformly to
    // the play area under the strip (the original's map fills the frame the same
    // way at 800x600).  Its left/top edges keep the reference offsets.
    const float ui = std::max(1.0f, (float)r_.height_ / 600.0f);   // panel scale
    const float top = hud_.height(r_);
    const float ph = std::min(Map::kPanelH * ui, (float)r_.height_ - top - 8.0f);
    const float s = ph / Map::kPanelH;
    const float pw = Map::kPanelW * s;
    const float px = Map::kPanelX * s;
    const float py = top + Map::kPanelTop * s;
    map_.draw(r_, font_, px, py, pw, ph, s);
    if (!map_.report().empty()) LOG("%s", map_.report().c_str());
}

// --fine-shot SPEED: one deterministic frame with the camera panel up.
void App::optsFineShot() {
    state_ = State::World;
    const int lim = opt_.speedLimitGiven && opt_.speedFor > 0.0f ? (int)opt_.speedFor : 60;
    fine_.limit = lim;
    fine_.speed = opt_.fineShot;
    const int over = opt_.fineShot - lim;
    fine_.amount = (long)std::max(100, (over / 10) * 100);
    if (fine_.amount > 5000) fine_.amount = 5000;
    fine_.timer = 6.0f;
    fine_.age = 0.2f;                     // the flash has just fired
    map_.setForced(true);
    LOG("fine: --fine-shot %d km/h in a %d zone, panel $%ld", fine_.speed, fine_.limit,
        fine_.amount);
}

// --msg-shot TEXT: one deterministic frame with the pager window up.
void App::optsMsgShot() {
    state_ = State::World;
    const std::string t = cp1251ToUtf8(opt_.msgShot);
    pushMessage(t, "script (--msg-shot)");
    map_.setForced(true);
    LOG("message: --msg-shot '%s'", t.c_str());
}

void App::drawDashboard() {
    unsigned white = r_.whiteTex();
    const float W = (float)r_.width_, H = (float)r_.height_;
    const float ph = 108.0f;
    r_.quad2D(white, 0, H - ph, W, ph, 0.04f, 0.05f, 0.07f, 0.78f);   // panel
    r_.line2D(white, 0, H - ph, W, H - ph, 2.0f, 0.55f, 0.6f, 0.7f, 0.9f);
    const float cy = H - ph * 0.55f;
    drawGauge(96, cy, 44, vehicle3d_.speedKmh() / 120.0f, 12, 120.0f, "KM/H", 90.0f);
    char buf[64];
    snprintf(buf, sizeof buf, "%.0f", vehicle3d_.speedKmh());
    font_.draw(r_, buf, 74, cy + 12, 1.6f, 1.0f, 0.95f, 0.6f, 1.0f);
    drawGauge(232, cy, 40, vehicle3d_.rpm() / 2500.0f, 10, 2500.0f, "RPM", 2000.0f);
    int g = vehicle3d_.gear();
    snprintf(buf, sizeof buf, "%s", g == 0 ? "N" : (g < 0 ? "R" : ""));
    if (g > 0) snprintf(buf, sizeof buf, "%d", g);
    font_.draw(r_, buf, 226, cy + 10, 1.8f, 0.6f, 1.0f, 0.7f, 1.0f);
    // fuel bar
    const float fx = 320, fy = cy - 34, fw = 140, fh = 10;
    r_.quad2D(white, fx, fy, fw, fh, 0.10f, 0.12f, 0.14f, 1.0f);
    float fuel = vehicle3d_.fuel();
    r_.quad2D(white, fx + 1, fy + 1, (fw - 2) * fuel, fh - 2,
              fuel < 0.15f ? 0.95f : 0.35f, fuel < 0.15f ? 0.25f : 0.85f, 0.35f, 1.0f);
    font_.draw(r_, "FUEL", fx, fy + 14, 1.0f, 0.8f, 0.82f, 0.88f, 1.0f);
    snprintf(buf, sizeof buf, "%.1f KM   %.0f M   MONEY %ld   CARGO %s", vehicle3d_.speedKmh(),
             vehicle3d_.odometer(),
             economy_.money(), economy_.activeJob() ? economy_.activeJob()->cargoName.c_str() : "-");
    font_.draw(r_, buf, fx, fy + 30, 1.2f, 0.92f, 0.94f, 0.85f, 1.0f);
    snprintf(buf, sizeof buf, "%02d:%02d  %s", (int)timeOfDay_,
             (int)((timeOfDay_ - (int)timeOfDay_) * 60.0f), dayTime() ? "DAY" : "NIGHT");
    font_.draw(r_, buf, W - 470.0f, H - ph + 26.0f, 1.3f, 0.85f, 0.9f, 0.75f, 1.0f);
    if (economy_.activeJob()) {
        const Economy::Job* j = economy_.activeJob();
        snprintf(buf, sizeof buf, "-> %s  %.0f M", j->toTown.c_str(), j->distance);
        font_.draw(r_, buf, fx, fy + 46, 1.2f, 0.95f, 0.85f, 0.5f, 1.0f);
    }
    drawMinimap(W - 214.0f, H - ph - 210.0f, 196.0f);
}

// ---------------------------------------------------------------------------
// Economy: money, the vehicle market and cargo jobs (src/game/Economy.cpp).
// ---------------------------------------------------------------------------
void App::initEconomy() {
    indexWayNodes();
    economy_.setWayNodes(wayNodes_);
    economy_.init(gameData_);
    initServices();
    initAmbushes();
    LOG("economy: money %ld, offers %zu, jobs %zu", economy_.money(),
        economy_.offers().size(), economy_.jobs().size());
}

bool App::switchPlayerVehicle(const std::string& model) {
    if (model.empty() || !haveTrucks_) return false;
    if (!trucks_.buildNamedScene(model)) {
        LOG("economy: model '%s' not in TRUCKS.B3D", model.c_str());
        return false;
    }
    r_.setVehicleScene(&trucks_, &vehicleTex_);
    cockpit_.load(r_, opt_.dataDir, model);      // rebind the instruments (no-op if same brand)
    mirror_.load(r_, opt_.dataDir, &trucks_.anchorList, model);   // rebind the mirror mount
    vehicle3d_.reset(vehicle3d_.x(), vehicle3d_.z(), vehicle3d_.yaw());
    opt_.vehicle = model;
    LOG("economy: player vehicle is now '%s' (%zu submeshes)", model.c_str(),
        trucks_.submeshes().size());
    return true;
}

void App::applyEconomyActions() {
    if (opt_.buyIndex >= 0 && opt_.buyIndex < (int)economy_.offers().size()) {
        float mx = vehicle3d_.x(), mz = vehicle3d_.z();
        long before = economy_.money();
        if (economy_.buy((size_t)opt_.buyIndex)) {
            const Economy::Offer& o = economy_.offers()[(size_t)opt_.buyIndex];
            switchPlayerVehicle(o.model);
            LOG("economy: bought %s (%s) for %ld, money %ld -> %ld at %.0f %.0f",
                o.id.c_str(), o.model.c_str(), o.price, before, economy_.money(), mx, mz);
        } else {
            LOG("economy: purchase %d refused (price %ld, money %ld)", opt_.buyIndex,
                economy_.offers()[(size_t)opt_.buyIndex].price, economy_.money());
        }
    }
    if (opt_.acceptJob >= 0 && !economy_.jobs().empty()) {
        std::vector<size_t> near = economy_.jobsNear(vehicle3d_.x(), vehicle3d_.z(), 1.0e9f);
        if (!near.empty()) {
            size_t idx = near[(size_t)std::min<size_t>((size_t)opt_.acceptJob, near.size() - 1)];
            if (economy_.accept(idx)) {
                const Economy::Job& j = economy_.jobs()[idx];
                LOG("economy: job accepted: %s -> %s, %.0f m, reward %ld",
                    j.fromTown.c_str(), j.toTown.c_str(), j.distance, j.reward);
            }
        }
    }
    // --job: take the nearest job (tests: the strip's ПОЗИЦИЯ/ОСТ.ВРЕМЯ cells)
    if (opt_.jobGiven && !economy_.activeJob()) {
        std::vector<size_t> near = economy_.jobsNear(vehicle3d_.x(), vehicle3d_.z(), 1.0e9f);
        if (!near.empty() && economy_.accept(near[0])) {
            const Economy::Job& j = economy_.jobs()[near[0]];
            LOG("economy: job accepted (--job): %s -> %s, %.0f m, reward %ld, time %.1f s",
                j.fromTown.c_str(), j.toTown.c_str(), j.distance, j.reward, j.timeLeft);
        } else {
            LOG("economy: --job found no job to accept");
        }
    }
    if (opt_.doRefuel) {
        long paid = 0;
        if (refuel(&paid)) LOG("service: refuelled for %ld, fuel %.2f, money %ld", paid,
                               vehicle3d_.fuel(), economy_.money());
        else LOG("service: refuel refused here (fuel %.2f, money %ld)", vehicle3d_.fuel(),
                 economy_.money());
    }
    if (opt_.doRepair) {
        long paid = 0;
        if (repair(&paid)) LOG("service: repaired for %ld, damage %.2f, money %ld", paid,
                               vehicle3d_.damage(), economy_.money());
        else LOG("service: repair refused here (damage %.2f, money %ld)", vehicle3d_.damage(),
                 economy_.money());
    }
    if (opt_.doDeliver) {
        long reward = 0;
        if (economy_.deliver(vehicle3d_.x(), vehicle3d_.z(), 120.0f, &reward))
            LOG("economy: delivered, reward %ld, money %ld", reward, economy_.money());
        else
            LOG("economy: nothing to deliver here");
    }
}

// ---------------------------------------------------------------------------
// Speed camera / fine (the «ЗАРЕГИСТРИРОВАНО ПРЕВЫШЕНИЕ СКОРОСТИ» panel).
//
// The road data carries its class through the segment width and its kind through
// the `pos_road_*` / `pos_parking_*` node names (the .way ATTR dword is not a
// limit - docs/road_filtering.md).  The camera therefore sits on the road nodes
// themselves and takes its limit from the road class the player is on:
//   width >= 14  -> 90 km/h (the highways of the north)
//   width >= 7   -> 60 km/h (the reference frame's «ОГРАНИЧЕНИЕ: 60 КМ/Ч»)
//   otherwise    -> 40 km/h (town streets / service roads)
// A zone fires once, then re-arms after driving away and a 6 s cool-down; the
// fine is deducted from Economy exactly once per photo.
// ---------------------------------------------------------------------------
void App::initSpeedLimits() {
    limitZones_.clear();
    if (wayNodes_.empty()) indexWayNodes();
    // One camera per fifth `pos_road_*` node, rounded down, plus every parking
    // entrance (the urban 40 zones) - deterministic and dense enough that a
    // normal drive crosses one.
    size_t road = 0;
    for (const auto& kv : wayNodes_) {
        const std::string& n = kv.first;
        if (n.rfind("pos_road_", 0) == 0) {
            if ((road++ % 5) != 0) continue;
        } else if (n.rfind("pos_parking_", 0) != 0) {
            continue;
        }
        LimitZone z;
        z.x = (float)kv.second.pos.x;
        z.z = (float)kv.second.pos.z;
        z.r = 60.0f;
        // the class comes from the widest segment within the zone
        float best = 0.0f;
        for (const auto& sg : waySegs_) {
            for (size_t k = 0; k < sg.pts.size(); ++k) {
                float dx = (float)sg.pts[k].x - z.x, dz = (float)sg.pts[k].z - z.z;
                if (dx * dx + dz * dz < 90.0f * 90.0f) { best = std::max(best, (float)sg.width); break; }
            }
        }
        const bool parking = n.rfind("pos_parking_", 0) == 0;
        z.limit = parking ? 40 : (best >= 14.0f ? 90 : (best >= 7.0f ? 60 : 40));
        if (opt_.speedLimitGiven) z.limit = opt_.speedFor > 0.0f ? (int)opt_.speedFor : z.limit;
        limitZones_.push_back(z);
    }
    LOG("speedcam: %zu speed-limit zones (%s), player starts at %.0f %.0f",
        limitZones_.size(), opt_.speedLimitGiven ? "limit overridden" : "road class",
        vehicle3d_.x(), vehicle3d_.z());
}

int App::speedLimitAt(float x, float z) const {
    // The camera only exists inside one of the zones, so an open-road position
    // reports "no limit" (0) and never fires.  `--speed-limit N` overrides the
    // limit OF a zone, it does not put one everywhere (the tests use it to make
    // the trigger deterministic).
    int best = 0;
    float bestD = 1e18f;
    for (const auto& zn : limitZones_) {
        float dx = zn.x - x, dz = zn.z - z;
        float d2 = dx * dx + dz * dz;
        if (d2 < zn.r * zn.r && d2 < bestD) {
            bestD = d2;
            best = zn.limit;
        }
    }
    if (best > 0 && opt_.speedLimitGiven && opt_.speedFor > 0.0f)
        best = (int)opt_.speedFor;
    return best;
}

void App::updateSpeedCamera(float dt) {
    if (fine_.timer > 0.0f) { fine_.timer -= dt; fine_.age += dt; }
    if (fineCooldown_ > 0.0f) fineCooldown_ -= dt;
    // The limit the player is under right now (0 = open road, no camera).
    lastLimit_ = speedLimitAt(vehicle3d_.x(), vehicle3d_.z());
    if (lastLimit_ <= 0 || fineCooldown_ > 0.0f) return;
    const float kmh = vehicle3d_.speedKmh();
    // The camera triggers above the limit (the reference frame: 82 in a 60);
    // a small tolerance keeps the trigger from firing on a 1 km/h overshoot.
    if (kmh <= (float)lastLimit_ + 5.0f) return;
    if (fine_.timer > 0.0f) return;                 // one panel at a time
    // The original's scale (reference frame: 82 in a 60 -> $500).
    const int over = (int)(kmh + 0.5f) - lastLimit_;
    long amount = (long)std::max(100, (over / 10) * 100);
    if (amount > 5000) amount = 5000;
    fine_.limit = lastLimit_;
    fine_.speed = (int)(kmh + 0.5f);
    fine_.amount = amount;
    fine_.timer = 6.0f;
    fine_.age = 0.0f;
    fineCooldown_ = 20.0f;                          // s before another photo
    ++finesIssued_;
    economy_.addMoney(-amount);
    if (audio_.ready()) audio_.playGlobal("photo", 0.8f);
    LOG("fine: %.0f km/h in a %d zone, $%ld deducted, money %ld",
        vehicle3d_.speedKmh(), fine_.limit, amount, economy_.money());
}// ---------------------------------------------------------------------------
// Service stations: the `service` forms name a .way node and a type (fuel /
// repair); the player pays to refuel or to repair the damage taken.
// ---------------------------------------------------------------------------
void App::initServices() {
    if (serviceStops_.empty()) indexWayNodes();
    serviceStops_.clear();
    size_t resolved = 0, fuel = 0, rep = 0;
    for (const auto& sv : gameData_.services) {
        auto it = wayNodes_.find(sv.wayNode);
        if (it == wayNodes_.end()) continue;
        ServiceStop s;
        s.x = (float)it->second.pos.x;
        s.z = (float)it->second.pos.z;
        // The type field is the numeric kind (fuel stations are 10); the shipped
        // ids are self-describing: BENZO_* = petrol, STO_* = repair workshop.
        std::string id = sv.id;
        for (auto& c : id) c = (char)toupper((unsigned char)c);
        int kind = atoi(sv.type.c_str());
        s.fuel = (kind == 10) || id.rfind("BENZO", 0) == 0;
        s.repair = !s.fuel;
        fuel += s.fuel ? 1 : 0;
        rep += s.repair ? 1 : 0;
        serviceStops_.push_back(s);
        ++resolved;
    }
    LOG("service: %zu stations resolved from %zu forms (%zu fuel, %zu repair)",
        resolved, gameData_.services.size(), fuel, rep);
    if (getenv("HT2_SERVICE_DEBUG")) {
        size_t n = 0;
        for (const auto& s : serviceStops_) {
            LOG("service: %s at %.0f %.0f", s.fuel ? "fuel " : "repair", s.x, s.z);
            if (++n >= 6) break;
        }
    }
}

int App::nearestService(bool wantFuel) const {
    const float R2 = 40.0f * 40.0f;
    int best = -1;
    float bestD = R2;
    for (size_t i = 0; i < serviceStops_.size(); ++i) {
        const ServiceStop& s = serviceStops_[i];
        if (wantFuel ? !s.fuel : !s.repair) continue;
        float dx = s.x - vehicle3d_.x(), dz = s.z - vehicle3d_.z();
        float d = dx * dx + dz * dz;
        if (d < bestD) { bestD = d; best = (int)i; }
    }
    return best;
}

bool App::refuel(long* paidOut) {
    if (nearestService(true) < 0) return false;
    float need = 1.0f - vehicle3d_.fuel();
    if (need < 0.03f) return false;
    long cost = (long)(need * 1800.0f);
    if (economy_.money() < cost) return false;
    economy_.addMoney(-cost);
    vehicle3d_.setFuel(1.0f);
    if (paidOut) *paidOut = cost;
    return true;
}

bool App::repair(long* paidOut) {
    if (nearestService(false) < 0) return false;
    float dmg = vehicle3d_.damage();
    if (dmg < 0.02f) return false;
    long cost = (long)(dmg * 6000.0f);
    if (economy_.money() < cost) return false;
    economy_.addMoney(-cost);
    vehicle3d_.repair();
    if (paidOut) *paidOut = cost;
    return true;
}

// ---------------------------------------------------------------------------
// Ambushes: `( ambush ID POLICE|MAFIA … "waynode" DIR (slot…) … )`.  The slot
// list is one id per car (police 44 / mafia 43 in the shipped data); the cars
// themselves are spawned as a standing roadblock when the player gets close.
// ---------------------------------------------------------------------------
void App::initAmbushes() {
    if (ambushes_.empty()) indexWayNodes();
    ambushes_.clear();
    size_t resolved = 0;
    for (const auto& am : gameData_.ambushes) {
        auto it = wayNodes_.find(am.wayNode);
        if (it == wayNodes_.end()) continue;
        AmbushState st;
        st.x = (float)it->second.pos.x;
        st.z = (float)it->second.pos.z;
        st.y = (float)it->second.pos.y;      // the road height at the ambush node
        st.klass = am.klass;
        st.cars = (int)std::min<size_t>(am.slots.size(), 5);
        if (st.cars < 1) st.cars = 1;
        st.radius = 120.0f;
        ambushes_.push_back(st);
        ++resolved;
    }
    LOG("ambush: %zu ambushes resolved from %zu forms", resolved, gameData_.ambushes.size());
}

void App::updateAmbushes() {
    if (ambushes_.empty()) return;
    const float px = vehicle3d_.x(), pz = vehicle3d_.z();
    for (auto& a : ambushes_) {
        if (a.fired) continue;
        float dx = a.x - px, dz = a.z - pz;
        if (dx * dx + dz * dz > a.radius * a.radius) continue;
        a.fired = true;
        ++ambushesFired_;
        // spawn the roadblock: cars lined up across the road, facing the player
        const std::string model = (a.klass == "MAFIA") ? "Cayman" : "BmwM5police";
        if (!parked_.loaded()) parked_.load(opt_.dataDir + "/COMMON/TRUCKS.B3D");
        float baseYaw = atan2f(px - a.x, pz - a.z);
        std::vector<B3D::SubMesh> extra;
        int spawned = 0;
        for (int i = 0; i < a.cars; ++i) {
            if (!parked_.buildNamedScene(model)) break;
            float side = ((float)i - (a.cars - 1) * 0.5f) * 3.6f;      // spread across
            float yaw = baseYaw + 1.5707963f;                           // across the road
            float cs = cosf(yaw), sn = sinf(yaw);
            float ox = a.x + side * cs, oz = a.z - side * sn;
            // `a.y` is the road height of the ambush's way node (the .way POSN
            // triple); the ground map holds the terrain/water under a bridge, so
            // it must not be used for a roadblock.
            float oh = a.y;
            if (oh == 0.0f && !ground_.empty()) oh = ground_.heightAt(ox, oz, vehicle3d_.y());
            const float yoff = modelGroundOffset(parked_.submeshes());
            for (auto& sm : parked_.submeshesMutable()) {
                B3D::SubMesh t = sm;
                for (size_t v = 0; v + 2 < t.pos.size(); v += 3) {
                    float lx = t.pos[v], lz = t.pos[v + 1], lh = t.pos[v + 2];
                    t.pos[v] = ox + lx * cs + lz * sn;
                    t.pos[v + 1] = oz - lx * sn + lz * cs;
                    t.pos[v + 2] = oh + yoff + lh;
                }
                // The renderer culls whole batches by the submesh bounding
                // sphere: `center` has to travel with the geometry (the local
                // center of a vehicle part is ~0, so leaving it alone puts the
                // roadblock's sphere at the world origin and culls it).
                const float clx = t.center[0], clz = t.center[1];
                t.center[0] = ox + clx * cs + clz * sn;
                t.center[1] = oz - clx * sn + clz * cs;
                t.center[2] = oh + yoff + t.center[2];
                extra.push_back(std::move(t));
            }
            ++spawned;
        }
        for (auto& sm : extra) worldMeshes_.push_back(std::move(sm));
        LOG("ambush: '%s' (%s) fired at %.0f %.0f: %d %s cars, %zu submeshes",
            a.klass.c_str(), model.c_str(), a.x, a.z, spawned, model.c_str(), extra.size());
        msgText_ = (a.klass == "MAFIA") ? "ЗАСАДА: МАФИЯ!" : "ЗАСАДА: ПОЛИЦИЯ!";
        msgTimer_ = 6.0f;
        msgAge_ = 0.0f;
        msgNew_ = true;
        LOG("message: '%s' (ambush %s)", msgText_.c_str(), a.klass.c_str());
        hiddenGroups_.clear();          // force a rebuild so the cars appear
        rebuildWorldScene();
        if (input_.rumbleSupported()) input_.rumble(0.9f, 0.6f, 400);
        break;
    }
}

float App::nearJobDist() const {
    std::vector<size_t> near = economy_.jobsNear(vehicle3d_.x(), vehicle3d_.z(), 60.0f);
    if (near.empty()) return -1.0f;
    const Economy::Job& j = economy_.jobs()[near[0]];
    float dx = j.fromX - vehicle3d_.x(), dz = j.fromZ - vehicle3d_.z();
    return sqrtf(dx * dx + dz * dz);
}

// Deterministic LCG shared by the gameplay systems (gates, spawn choices).
static float trand(uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return (float)((s >> 8) & 0xFFFFFF) / 16777216.0f;
}

// ---------------------------------------------------------------------------
// Scripted world logic: the `people` forms toggle `subtag 21` key groups by day
// (and weather); `obstacle` keys use the same mechanism for road closures.
// ---------------------------------------------------------------------------
void App::updateGates() {
    if (gameData_.gates.empty()) return;
    if (!gatesBuilt_) {
        size_t gated = 0;
        for (const auto& sm : worldMeshes_) if (!sm.group.empty()) ++gated;
        LOG("gates: %zu gates, %zu submeshes belong to key groups", gameData_.gates.size(), gated);
        gatesBuilt_ = true;
    }
    std::set<std::string> hidden;
    const bool day = dayTime();
    // Street lighting: the engine's own clock drives the three illumination
    // levels (no script mentions them).  13 = off/day, 14 = dusk/dawn,
    // 15 = night; only the current level's group is drawn.
    {
        const float h = timeOfDay_;
        int level = 13;
        if (h < 6.5f || h >= 19.5f) level = 15;
        else if (h < 7.5f || h >= 18.5f) level = 14;
        for (int lv = 13; lv <= 15; ++lv) {
            if (lv == level) continue;
            char name[32];
            snprintf(name, sizeof name, "refer_ILLUM_LEVEL_%d", lv);
            hidden.insert(name);
        }
    }
    // Road works / barriers: the obstacle keys carry the closed state.  With no
    // event simulation yet every obstacle starts closed (its geometry is shown);
    // --obstacles open hides them so the toggle can be verified.
    for (const auto& ob : gameData_.obstacles)
        if (opt_.obstaclesOpen) hidden.insert(ob.key);
    for (const auto& g : gameData_.gates) {
        bool on = (g.kind == "UNCONDITIONAL") || (g.kind == "DAYONLY" && day) ||
                  (g.kind == "DAYGW" && day);              // weather is always good for now
        if (on && g.prob < 0.999f) on = (trand(rng_) < g.prob);   // density field
        if (!on) hidden.insert(g.key);
    }
    if (getenv("HT2_GATE_DEBUG")) {
        size_t n = 0;
        for (const auto& g : hidden) { LOG("gates: hidden '%s'", g.c_str()); if (++n >= 5) break; }
        std::map<std::string, size_t> hist;
        for (const auto& sm : worldMeshes_)
            if (!sm.group.empty()) hist[sm.group]++;
        LOG("gates: %zu distinct mesh groups", hist.size());
        n = 0;
        for (const auto& kv : hist) {
            LOG("gates: group '%s' x%zu", kv.first.c_str(), kv.second);
            if (++n >= 8) break;
        }
    }
    if (hidden == hiddenGroups_) return;
    hiddenGroups_ = hidden;
    rebuildWorldScene();
}

void App::rebuildWorldScene() {
    worldVisible_.clear();
    worldVisible_.reserve(worldMeshes_.size());
    for (const auto& sm : worldMeshes_)
        if (sm.group.empty() || hiddenGroups_.count(sm.group) == 0)
            worldVisible_.push_back(sm);
    r_.setSceneMeshes(&worldVisible_, &worldTex_);
    LOG("gates: %zu hidden key groups, %zu of %zu submeshes drawn",
        hiddenGroups_.size(), worldVisible_.size(), worldMeshes_.size());
}

void App::loadWorldTiles() {
    worldMeshes_.clear();
    worldTileNames_.clear();
    std::vector<std::string> files;
    if (!opt_.onlyTile.empty()) {
        files.push_back(opt_.dataDir + "/ENV/" + opt_.onlyTile + ".b3d");
    } else {
        DIR* d = opendir((opt_.dataDir + "/ENV").c_str());
        if (!d) { LOG("app: no ENV directory"); return; }
        while (struct dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".b3d") == 0)
                files.push_back(opt_.dataDir + "/ENV/" + n);
        }
        closedir(d);
        std::sort(files.begin(), files.end());
    }
    size_t total = 0;
    for (const std::string& p : files) {
        B3D t;
        if (!t.load(p)) continue;
        t.buildScene();
        total += t.submeshes().size();
        for (auto& sm : t.submeshesMutable()) worldMeshes_.push_back(std::move(sm));
        // texture base: <dir>/ENV/<tile>
        std::string base = p.substr(0, p.size() - 4);
        worldTileNames_.push_back(base);
    }
    {
        std::vector<float> roadPts;
        for (const auto& sg : way_.segments())
            for (const auto& p : sg.pts) {
                roadPts.push_back((float)p.x);
                roadPts.push_back((float)p.z);
                roadPts.push_back((float)p.y);
            }
        ground_.build(worldMeshes_, roadPts, 12.0f, &terrain_);
    }
    LOG("app: world = %zu tiles, %zu submeshes", files.size(), total);
}

bool App::loadData() {
    timeOfDay_ = opt_.startHour;
    dayLength_ = opt_.dayLength;
    if (opt_.view == "cockpit") viewMode_ = 1;
    else if (opt_.view == "bonnet") viewMode_ = 2;
    // The original configuration lives in the game's own scripts; decrypt and
    // parse them first so the spawn point and the player's truck come from the
    // shipped data instead of hard-coded constants.
    gameData_.load(opt_.dataDir);
    if (!opt_.vehicleGiven && !gameData_.playerVehicle.empty())
        opt_.vehicle = gameData_.playerVehicle;
    // calibration is built into Terrain (see Terrain.h / NOTES.md)
    if (!terrain_.load(opt_.dataDir + "/MTR/terrain0.raw")) return false;
    // The original's weather presets and draw distance: WEATHER.INI next to the
    // executable (king.exe reads ".\\weather.ini", see src/world/Weather.h).
    weather_.load(opt_.dataDir + "/WEATHER.INI");
    if (!opt_.weather.empty()) {
        Weather::Selection sel;
        if (Weather::parseSelection(opt_.weather, weather_.setCount(), sel)) {
            weatherBad_ = sel.bad;
            if (sel.set > 0) opt_.weatherSet = sel.set;
            weatherKind_ = sel.snow ? 2 : (sel.rain ? 1 : 0);
            weatherAmount_ = (weatherKind_ || weatherBad_) ? 1.0f : 0.0f;
        } else {
            LOG("weather: unknown --weather '%s' (clear|rain|snow|bad|fog|1..%d)",
                opt_.weather.c_str(), weather_.setCount());
        }
    }
    if (opt_.weatherSet < 1 || opt_.weatherSet > weather_.setCount()) opt_.weatherSet = 1;
    weatherTime_ = opt_.weatherCyclePhase * 18.0f;
    if (!way_.load(opt_.dataDir + "/ENV/aa.way"))
        LOG("app: no road network (ENV/aa.way)");

    SceneTextures::loadPalette(opt_.dataDir + "/COMMON/common1.res");
    haveMenuScene_ = menuScene_.load(opt_.dataDir + "/MENV/dr.b3d");
    if (haveMenuScene_) menuScene_.buildScene();
    loadWorldTiles();
    spawnParkedVehicles();
    if (opt_.noCollision) LOG("collision: disabled by --no-collision");
    else {
        collision_.build(worldMeshes_);
        vehicle3d_.setCollision(&collision_);
    }
    if (!opt_.onlyName.empty()) {
        std::vector<B3D::SubMesh> keep;
        for (auto& sm : worldMeshes_)
            if (sm.name.compare(0, opt_.onlyName.size(), opt_.onlyName) == 0)
                keep.push_back(std::move(sm));
        worldMeshes_.swap(keep);
        LOG("app: kept %zu submeshes with name prefix '%s'", worldMeshes_.size(), opt_.onlyName.c_str());
    }
    if (!opt_.onlyMat.empty()) {
        size_t before = worldMeshes_.size();
        std::vector<B3D::SubMesh> keep;
        for (auto& sm : worldMeshes_)
            if (sm.material.compare(0, opt_.onlyMat.size(), opt_.onlyMat) == 0)
                keep.push_back(std::move(sm));
        worldMeshes_.swap(keep);
        LOG("app: kept %zu/%zu submeshes with material prefix '%s'",
            worldMeshes_.size(), before, opt_.onlyMat.c_str());
    }
    if (!opt_.hideMat.empty()) {
        size_t before = worldMeshes_.size();
        std::vector<B3D::SubMesh> keep;
        keep.reserve(before);
        for (auto& sm : worldMeshes_)
            if (sm.material.compare(0, opt_.hideMat.size(), opt_.hideMat) != 0)
                keep.push_back(std::move(sm));
        worldMeshes_.swap(keep);
        LOG("app: hid %zu submeshes with material prefix '%s'",
            before - worldMeshes_.size(), opt_.hideMat.c_str());
    }
    haveWorldScene_ = !worldMeshes_.empty();

    if (opt_.scene == "vehicle") {
        state_ = State::Vehicle;
        haveTrucks_ = trucks_.load(opt_.dataDir + "/COMMON/TRUCKS.B3D");
        if (haveTrucks_) trucks_.buildNamedScene(opt_.vehicle);
    } else if (opt_.scene == "world" || opt_.trafficReport > 0.0f) {
        state_ = State::World;
    } else {
        state_ = State::Menu;
    }

    // start position of the world (from ENV/GAME.CNF: (startposition -3020 -4625 20))
    spawn_[0] = -3020.0f;
    spawn_[1] = -4625.0f;
    spawn_[2] = 0.0f;
    if (gameData_.haveStart && !opt_.spawnGiven) {
        spawn_[0] = gameData_.startPos[0];
        spawn_[1] = gameData_.startPos[1];
    }
    if (opt_.spawnGiven) {
        spawn_[0] = opt_.spawnOverride[0];
        spawn_[1] = opt_.spawnOverride[1];
        spawn_[2] = opt_.spawnOverride[2];
    }
    worldCam_[0] = spawn_[0];
    worldCam_[1] = 220.0f;
    worldCam_[2] = spawn_[1];
    worldCam_[3] = spawn_[2];
    worldCam_[4] = -12.0f;
    if (opt_.camGiven)
        for (int i = 0; i < 5; ++i) worldCam_[i] = opt_.cam[i];
    else {
        // sit 150 units above the (calibrated) terrain surface at the start point
        worldCam_[1] = terrain_.heightAtWorld(worldCam_[0], worldCam_[2], 200.0f) + 150.0f;
    }
    if (opt_.above > 0.0f) {
        worldCam_[1] = terrain_.heightAtWorld(worldCam_[0], worldCam_[2], 200.0f) + opt_.above;
    }
    return true;
}

void App::frame(float dt, bool interactive) {
    float pitchV = -12.0f;
    if (state_ == State::Movie) {
        // The player draws itself (frame + letterbox) and owns its clock: the
        // interactive loop advances it through movieFrame()/Video::tick(), and
        // this only re-presents the frame that is up (the screenshot paths).
        video_.present(r_);
        return;
    }
    if (state_ == State::Vehicle) {
        // inspection view of a vehicle model from COMMON/TRUCKS.B3D
        r_.setSky(0.20f, 0.24f, 0.30f);
        r_.setFog(900.0f);
        r_.setSun(0.4f, 0.8f, 0.3f);
        r_.setAmbient(0.6f);
        r_.clear();
        const B3D& b = trucks_;
        float cx = b.bboxCenter(0), cy = b.bboxCenter(2), cz = b.bboxCenter(1);
        float ext = 0;
        for (int i = 0; i < 3; ++i) ext = std::max(ext, b.bboxMax()[i] - b.bboxMin()[i]);
        float t = interactive ? (float)SDL_GetTicks() / 1000.0f : opt_.time;
        float a = t * 0.7f;
        float R = ext * 1.6f + 4.0f;
        float px = cx + cosf(a) * R, pz = cz + sinf(a) * R, py = cy + ext * 0.7f;
        float yaw = atan2f(cx - px, -(cz - pz)) * 180.0f / PI;
        if (opt_.camGiven) {
            px = opt_.cam[0]; py = opt_.cam[1]; pz = opt_.cam[2];
            yaw = opt_.cam[3]; pitchV = opt_.cam[4];
        }
        r_.drawScene(px, py, pz, yaw, opt_.camGiven ? opt_.cam[4] : -12.0f);
        r_.begin2D();
        font_.draw(r_, "VEHICLE: " + b.sceneName(), 16, 16, 1.4f, 1, 1, 1, 0.9f);
        char buf[160];
        snprintf(buf, sizeof buf, "SUBMESHES %zu   BBOX %.1f %.1f %.1f / %.1f %.1f %.1f",
                 b.submeshes().size(), b.bboxMin()[0], b.bboxMin()[1], b.bboxMin()[2],
                 b.bboxMax()[0], b.bboxMax()[1], b.bboxMax()[2]);
        font_.draw(r_, buf, 16, 40, 1.2f, 0.9f, 0.9f, 0.6f, 0.9f);
        r_.end2D();
        return;
    }
    if (state_ == State::Menu) {
        // menu scene look: the city is seen through the truck's "windshield"
        // circle, so it gets a brighter daylight sky than the world view.
        r_.setSky(0.44f, 0.60f, 0.82f);
        // The original is a short-visibility game: a 2.6 km fog with a 5.2 km far
        // plane drew the far terrain (and its untextured slabs) for kilometres.
        r_.setFog(1200.0f);
        r_.setSun(0.40f, 0.74f, 0.34f);
        r_.setAmbient(0.78f);
        r_.clear();
        if (haveMenuScene_) {
            float x, y, z, yaw, pitch;
            menu_.camera(x, y, z, yaw, pitch);
            if (opt_.camGiven) {           // debug: fixed camera for the menu scene
                x = opt_.cam[0]; y = opt_.cam[1]; z = opt_.cam[2];
                yaw = opt_.cam[3]; pitch = opt_.cam[4];
            }
            r_.drawScene(x, y, z, yaw, pitch);
        }
        menu_.draw(r_, font_);
        return;
    }

    drawWorld(dt, interactive);
}

// ---------------------------------------------------------------------------
// Audio (src/audio/Audio.cpp).  Everything here is a no-op when the module was
// not initialised, so the offline modes (--screenshot/--drive) never need a
// sound device; SDL_AUDIODRIVER=dummy is a supported (and tested) setup.
// ---------------------------------------------------------------------------
void App::startAudio() {
    if (audio_.ready()) return;
    if (!audio_.init(opt_.dataDir)) {
        LOG("app: audio unavailable (no SDL2_mixer or no device) - running silent");
        return;
    }
    LOG("%s", audio_.describe().c_str());
}

void App::updateSceneAudio() {
    if (!audio_.ready()) return;
    // The listener takes the forward *vector*: Audio::setListener(pos, yaw) uses
    // the camera convention, and Vehicle::yaw() is 180 deg off from it.
    float fx, fz;
    vehicle3d_.forward(fx, fz);
    const Audio::Vec3 at{vehicle3d_.x(), vehicle3d_.y(), vehicle3d_.z()};
    audio_.setListenerDir(at, fx, fz);
    audio_.engine(vehicle3d_.rpm(), vehicle3d_.engineLoad(), at);   // lazy start

    // A hard contact is the blocked() edge with speed: a sustained scrape along
    // a wall is blocked() on every frame and must not retrigger the crash.
    // Vehicle::update() has already shed the speed in the frame it blocks (x0.25
    // / x0.3), so the impact speed is the one from before that update.
    const bool blocked = vehicle3d_.blocked();
    const float kmh = fabsf(vehicle3d_.speedKmh());
    const float impact = std::max(prevSpeedKmh_, kmh);
    if (blocked && !wasBlocked_ && impact >= crashSpeed_) {
        audio_.play("snd\\crash9.wav", at, 0.8f);
        LOG("audio: crash 'snd\\crash9.wav' at %.0f km/h (damage %.2f)", impact,
            vehicle3d_.damage());
        if (input_.rumbleSupported()) input_.rumble(0.9f, 0.6f, 350);
    }
    wasBlocked_ = blocked;
    prevSpeedKmh_ = kmh;
    audio_.update();                 // fades, channel parking, freed music
}

void App::startMenuMusic() {
    if (!audio_.ready() || opt_.noMenuMusic || menuMusicOn_) return;
    audio_.music(opt_.musicTrack, true);      // 600 ms fade-in
    menuMusicOn_ = audio_.musicPlaying();
    LOG("audio: menu music '%s' %s", opt_.musicTrack.c_str(),
        menuMusicOn_ ? "started" : "not playing");
}

void App::stopMenuMusic() {
    if (!menuMusicOn_) return;
    audio_.stopMusic();                       // 900 ms fade-out
    menuMusicOn_ = false;
    LOG("audio: menu music '%s' stopped for the world", opt_.musicTrack.c_str());
}

// ---------------------------------------------------------------------------
// In-game ESC screens (src/ui/EscMenu.cpp, docs/esc_impl.md)
//
// EscMenu turns the panel art + the button tables into an *action string*; the
// world state stays here.  The save game is a small documented text file under
// the SDL user data dir (SDL_GetPrefPath).
// ---------------------------------------------------------------------------
void App::leaveWorldToMenu() {
    esc_.close();
    state_ = State::Menu;
    if (haveMenuScene_) r_.setScene(&menuScene_, &menuTex_);
    if (audio_.ready()) audio_.engineStop();
    wasBlocked_ = false;
    startMenuMusic();
    LOG("esc: left the world for the main menu");
}

void App::applyEscSliders() {
    if (!audio_.ready()) return;
    // The four painted rows of BackEscSnd are master / music / engine / brake
    // sound.  The mixer has no separate brake bus, so the fourth row drives the
    // effect channel (see docs/esc_impl.md).
    audio_.setMasterVolume(esc_.sliderValue("voll"));
    audio_.setChannelVolumes(esc_.sliderValue("skidl"), esc_.sliderValue("musl"),
                             esc_.sliderValue("engl"));
}

void App::escAction(const std::string& act) {
    if (act.empty() || act == "back") return;      // "back" is handled in EscMenu
    if (act == "resume") {
        if (audio_.ready()) audio_.engineStart();
        LOG("esc: closed, driving resumed");
        return;
    }
    if (act == "restart") {                        // НАЧАТЬ СНАЧАЛА
        vehicle3d_.reset(spawn_[0], spawn_[1], spawn_[2]);
        LOG("esc: restart - respawned at the start (%.0f, %.0f)", spawn_[0], spawn_[1]);
        return;
    }
    if (act == "recover") { recoverVehicle(); return; }        // 911
    if (act == "menu") { leaveWorldToMenu(); return; }         // ГЛАВНОЕ МЕНЮ
    if (act == "exitsave") { saveGame(); leaveWorldToMenu(); return; }
    if (act == "exitnosave") { leaveWorldToMenu(); return; }
    if (act == "writesave") { saveGame(); return; }
    if (act == "deletesave") { deleteSave(); return; }
    if (act == "loadfile") {
        if (loadGame()) {
            esc_.close();
            if (audio_.ready()) audio_.engineStart();
            LOG("esc: closed after loading, driving resumed");
        }
        return;
    }
    if (act.rfind("open:", 0) == 0) return;        // handled inside EscMenu
    LOG("esc: unhandled action '%s'", act.c_str());
}

// --esc-act NAME[,NAME...]: the same path a click/keyboard activation takes, so
// the state machine can be driven headlessly (used by tools/shot_test.py).
void App::runEscScript(const std::string& list) {
    size_t pos = 0;
    while (pos <= list.size()) {
        const size_t comma = list.find(',', pos);
        std::string tok = list.substr(pos, comma == std::string::npos
                                              ? std::string::npos : comma - pos);
        pos = (comma == std::string::npos) ? list.size() + 1 : comma + 1;
        while (!tok.empty() && tok.front() == ' ') tok.erase(tok.begin());
        while (!tok.empty() && tok.back() == ' ') tok.pop_back();
        if (tok.empty()) continue;
        esc_.takeActivated();                      // drop anything pending
        const std::string from = esc_.screen();
        std::string act = esc_.activateName(tok);
        if (act.empty() && isdigit((unsigned char)tok[0]))
            act = esc_.activateId((int)strtol(tok.c_str(), nullptr, 0));
        if (act.empty()) {
            LOG("esc: --esc-act '%s': no such button on screen '%s'",
                tok.c_str(), esc_.screen().c_str());
            continue;
        }
        LOG("esc: activate '%s' (id 0x%x, screen '%s') -> action '%s'",
            esc_.lastButton().c_str(), esc_.buttonId(esc_.lastButton()),
            from.c_str(), act.c_str());
        esc_.takeActivated();
        escAction(act);
    }
}

void App::recoverVehicle() {
    // 911: the tow truck takes the player to the nearest service station and
    // the truck comes back fuelled and repaired.
    int best = -1;
    float bestD = 0.0f;
    for (size_t i = 0; i < serviceStops_.size(); ++i) {
        const float dx = serviceStops_[i].x - vehicle3d_.x();
        const float dz = serviceStops_[i].z - vehicle3d_.z();
        const float d = dx * dx + dz * dz;
        if (best < 0 || d < bestD) { best = (int)i; bestD = d; }
    }
    if (best < 0) {
        vehicle3d_.reset(spawn_[0], spawn_[1], spawn_[2]);
        LOG("esc: 911 - no service station known, respawned at the start (%.0f, %.0f)",
            spawn_[0], spawn_[1]);
    } else {
        const ServiceStop& s = serviceStops_[(size_t)best];
        vehicle3d_.reset(s.x, s.z, vehicle3d_.yaw());
        vehicle3d_.setFuel(1.0f);
        vehicle3d_.repair();
        vehicle3d_.addDamage(0.0f);
        LOG("esc: 911 - towed to the nearest %s station at %.0f, %.0f "
            "(fuel 1.00, damage 0.00)", s.fuel ? "fuel" : "repair", s.x, s.z);
    }
    if (input_.rumbleSupported()) input_.rumble(0.4f, 0.2f, 250);
}

std::vector<std::string> App::savePaths() const {
    std::vector<std::string> v;
    const char* env = getenv("HT2_SAVE_FILE");
    if (env && *env) { v.push_back(env); return v; }     // tests / sandboxes
    char* pref = SDL_GetPrefPath("ht2re", "ht2re");
    if (pref) {
        v.push_back(std::string(pref) + "savegame.txt");
        SDL_free(pref);
    }
    v.push_back("ht2re_savegame.txt");   // the working directory as a fallback
    return v;
}

std::string App::savePath() const {
    const std::vector<std::string> c = savePaths();
    for (const std::string& p : c) {
        FILE* f = fopen(p.c_str(), "rb");
        if (f) { fclose(f); return p; }
    }
    return c.empty() ? std::string() : c[0];
}

std::string App::saveSummary() const {
    const std::string path = savePath();
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "SLOT 1: EMPTY";
    long money = 0;
    float x = 0, z = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        if (sscanf(line, "money %ld", &money) == 1) continue;
        if (sscanf(line, "pos %f %f", &x, &z) == 2) continue;
    }
    fclose(f);
    char buf[160];
    snprintf(buf, sizeof buf, "SLOT 1: %ld RUB   X %.0f   Z %.0f", money, x, z);
    return buf;
}

bool App::saveGame() {
    const std::vector<std::string> candidates = savePaths();
    std::string path;
    FILE* f = nullptr;
    for (const std::string& p : candidates) {
        f = fopen(p.c_str(), "wb");
        if (f) { path = p; break; }
    }
    if (!f) {
        LOG("esc: save failed, none of the %zu candidate paths is writable", candidates.size());
        return false;
    }
    int jobIndex = -1;
    const Economy::Job* job = economy_.activeJob();
    const std::vector<Economy::Job>& jobs = economy_.jobs();
    if (job)
        for (size_t i = 0; i < jobs.size(); ++i)
            if (&jobs[i] == job) { jobIndex = (int)i; break; }
    fprintf(f, "HT2RE-SAVE 1\n");
    fprintf(f, "# Hard Truck 2: King of the Road engine reimplementation save\n");
    fprintf(f, "vehicle %s\n", opt_.vehicle.c_str());
    fprintf(f, "money %ld\n", economy_.money());
    fprintf(f, "pos %.3f %.3f %.3f\n", vehicle3d_.x(), vehicle3d_.z(), vehicle3d_.yaw());
    fprintf(f, "fuel %.4f\n", vehicle3d_.fuel());
    fprintf(f, "damage %.4f\n", vehicle3d_.damage());
    fprintf(f, "job %d\n", jobIndex);
    fprintf(f, "delivered %zu\n", economy_.delivered());
    fclose(f);
    LOG("esc: savegame written to %s "
        "(money %ld, pos %.0f %.0f yaw %.0f, fuel %.2f, damage %.2f, job %d)",
        path.c_str(), economy_.money(), vehicle3d_.x(), vehicle3d_.z(), vehicle3d_.yaw(),
        vehicle3d_.fuel(), vehicle3d_.damage(), jobIndex);
    return true;
}

bool App::loadGame() {
    const std::string path = savePath();
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        LOG("esc: no save game at %s", path.c_str());
        return false;
    }
    char magic[32] = {0};
    int version = 0;
    if (fscanf(f, "%31s %d", magic, &version) != 2 || strcmp(magic, "HT2RE-SAVE") != 0) {
        fclose(f);
        LOG("esc: %s is not a HT2RE save game", path.c_str());
        return false;
    }
    fclose(f);
    f = fopen(path.c_str(), "rb");
    long money = economy_.money();
    int delivered = -1;
    float x = vehicle3d_.x(), z = vehicle3d_.z(), yaw = vehicle3d_.yaw();
    float fuel = vehicle3d_.fuel(), damage = vehicle3d_.damage();
    int jobIndex = -1;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        if (sscanf(line, "pos %f %f %f", &x, &z, &yaw) == 3) continue;
        if (sscanf(line, "fuel %f", &fuel) == 1) continue;
        if (sscanf(line, "damage %f", &damage) == 1) continue;
        if (sscanf(line, "money %ld", &money) == 1) continue;
        if (sscanf(line, "job %d", &jobIndex) == 1) continue;
        if (sscanf(line, "delivered %d", &delivered) == 1) continue;
    }
    fclose(f);
    vehicle3d_.reset(x, z, yaw);
    vehicle3d_.setFuel(std::max(0.0f, std::min(1.0f, fuel)));
    vehicle3d_.repair();
    if (damage > 0.0f) vehicle3d_.addDamage(damage);
    economy_.setMoney(money);
    if (jobIndex >= 0 && jobIndex < (int)economy_.jobs().size())
        economy_.accept((size_t)jobIndex);
    LOG("esc: savegame loaded from %s (v%d) "
        "(money %ld, pos %.0f %.0f yaw %.0f, fuel %.2f, damage %.2f, job %d)",
        path.c_str(), version, money, x, z, yaw, fuel, damage, jobIndex);
    return true;
}

bool App::deleteSave() {
    bool any = false;
    for (const std::string& p : savePaths()) {
        if (remove(p.c_str()) == 0) {
            LOG("esc: savegame deleted (%s)", p.c_str());
            any = true;
        }
    }
    if (!any) LOG("esc: no save game to delete");
    return any;
}

// Everything that must go before Renderer::shutdown(): the movie's texture needs
// a live GL context and the mixer needs a live SDL.
void App::finish() {
    if (audio_.ready()) {
        LOG("audio: summary effects=%zu missing=%zu crash9=%d music=%d engineRestarts=%zu "
            "pitch=%.2f gain=%.2f",
            audio_.effects(), audio_.missingRes(), (int)audio_.has("crash9"),
            (int)audio_.musicPlaying(), audio_.engineRestarts(),
            audio_.enginePitchRatio(), audio_.engineGain());
    }
    video_.releaseTexture(r_);
    video_.close();
    audio_.shutdown();
}

// ---------------------------------------------------------------------------
// Movies (src/ui/Video.cpp).  The Indeo 5 sources are transcoded once by
// tools/make_video.sh into build/media/<clip>.rvid (+ .wav); when the pack is
// missing it is produced on first use, and when ffmpeg or the AVI is missing the
// movie is skipped instead of failing the run.  The pack is streamed (one frame
// per update), never loaded whole.
// ---------------------------------------------------------------------------
std::string App::findMakeVideoScript() {
    std::vector<std::string> cands;
    if (const char* env = getenv("HT2_MAKE_VIDEO")) cands.push_back(env);
    cands.push_back("tools/make_video.sh");
    if (char* base = SDL_GetBasePath()) {
        std::string b(base);
        SDL_free(base);
        cands.push_back(b + "tools/make_video.sh");
        cands.push_back(b + "../tools/make_video.sh");
        cands.push_back(b + "../../tools/make_video.sh");
    }
    for (const std::string& p : cands) {
        struct stat st;
        if (!p.empty() && stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return p;
    }
    return std::string();
}

bool App::ensureVideoPack(const std::string& clip) {
    if (!opt_.makeVideo || opt_.noVideo || makeVideoTried_) return false;
    makeVideoTried_ = true;                  // ~3 s, idempotent; once per process
    static const char* kNoFfmpeg =
        "movie: tools/make_video.sh unavailable - movies stay disabled";
    std::string script = findMakeVideoScript();
    if (script.empty()) { LOG("%s", kNoFfmpeg); return false; }
    const char* tmp = getenv("TMPDIR");
    std::string log = std::string(tmp && *tmp ? tmp : "/tmp") + "ht2_make_video.log";
    std::string cmd = "\"" + script + "\" " + clip + " >\"" + log + "\" 2>&1";
    LOG("movie: pack '%s' missing - running %s %s (log %s)", clip.c_str(),
        script.c_str(), clip.c_str(), log.c_str());
    const int rc = system(cmd.c_str());
    if (rc != 0) {
        LOG("movie: %s failed (exit %d) - ffmpeg missing or the source AVI is "
            "not there; skipping the movie", script.c_str(), rc);
        return false;
    }
    return true;
}

bool App::openClip(Video& v, const std::string& clip) {
    if (opt_.noVideo) { LOG("movie: --no-video, '%s' not played", clip.c_str()); return false; }
    if (v.open(opt_.dataDir, clip)) return true;
    if (!ensureVideoPack(clip)) return false;
    return v.open(opt_.dataDir, clip);       // the pack may have just been built
}

bool App::enterMovie(const std::string& clip) {
    // the movie carries its own soundtrack: stop the menu track first, otherwise
    // both play at once (the world restarts the menu music on the way back)
    stopMenuMusic();
    if (!openClip(video_, clip)) {
        LOG("movie: '%s' unavailable - continuing without it", clip.c_str());
        return false;
    }
    video_.setAudioEnabled(videoAudioWanted_);
    video_.play(videoAudioWanted_);
    state_ = State::Movie;
    LOG("movie: playing '%s' %dx%d @%dfps, %d frames (%.1f s, %.1f MB packed), "
        "soundtrack %s",
        clip.c_str(), video_.clip().w, video_.clip().h, video_.clip().fps,
        video_.clip().frames, video_.duration(), video_.clip().packedBytes / 1e6,
        video_.audioReady() ? "on" : "off");
    return true;
}

void App::leaveMovie() {
    if (video_.ready())
        LOG("movie: '%s' finished/skipped at frame %zu/%d", opt_.movieName.c_str(),
            video_.frame(), video_.clip().frames);
    video_.stop();
    state_ = State::Menu;
    if (haveMenuScene_) r_.setScene(&menuScene_, &menuTex_);
    startMenuMusic();
}

bool App::movieFrame() {
    if (!video_.playing()) return false;
    // The player owns its clock (Video::tick): the frame index comes from
    // SDL_GetTicks, so a slow frame skips picture instead of lagging the sound.
    if (video_.tick(r_)) return true;
    LOG("movie: '%s' reached its end at frame %zu/%d", opt_.movieName.c_str(),
        video_.frame(), video_.clip().frames);
    return false;
}

// Deterministic single-frame render of a movie (--movie --screenshot): seek
// straight to the wanted frame and leave it on screen.  No wall clock and no
// frame-by-frame stepping are involved, so the screenshot cannot depend on
// timing; a soundtrack that is already playing follows the seek.
bool App::movieShotRun() {
    if (!video_.ready()) return false;
    const float fps = (float)std::max(1, video_.clip().fps);
    size_t target = 0;
    if (opt_.movieFrames >= 0) target = (size_t)opt_.movieFrames;
    else if (opt_.movieTime > 0.0f) target = (size_t)(opt_.movieTime * fps + 0.5f);
    if (target >= (size_t)video_.clip().frames) target = (size_t)video_.clip().frames - 1;
    const bool on = video_.seekFrame(r_, target);
    LOG("movie: screenshot at t=%.2fs frame %zu/%d (asked for %zu) of '%s' (%s)",
        video_.elapsed(), video_.frame(), video_.clip().frames, target,
        opt_.movieName.c_str(), on ? "on screen" : "ended");
    return on;
}

// --movie-sync-report: measure the drift the bug report is about ("the sound
// runs ahead of the picture") without a display or a sound card.  Phase 1 feeds
// the player a synthetic clock - no wall clock, no device - at the 0/30/60/100 s
// checkpoints and through a deliberately stuttering loop, and checks that the
// frame index is exactly floor(clock*fps): the picture follows the clock and
// skips frames instead of accumulating lag.  Phase 2 plays for real for
// --movie-sync-seconds (the SDL dummy device keeps time), prints video time vs
// mixer position every second, and then seeks to 30/60/100 s so the same drift
// is measurable at the far end of the clip without waiting 100 s.
bool App::movieSyncReportRun() {
    if (!video_.ready()) { LOG("movie-sync: no clip loaded"); return false; }
    const int fps = std::max(1, video_.clip().fps);
    const int frames = video_.clip().frames;
    const double checkpoints[] = {0.0, 30.0, 60.0, 100.0};
    bool ok = true, mixerRan = false;
    auto line = [&](const char* tag) {
        const double v = video_.videoTime(), a = video_.audioTime();
        const bool run = video_.audioRunning();
        mixerRan = mixerRan || run;
        LOG("%s: video=%.3fs audio=%.3fs drift=%+.0fms frame=%zu/%d mixer=%s", tag, v, a,
            (v - a) * 1000.0, video_.frame(), frames, run ? "running" : "silent");
    };

    // --- 1) the frame index is a pure function of the clock -----------------
    video_.setAudioEnabled(false);
    video_.play(false);
    for (size_t i = 0; i < sizeof(checkpoints) / sizeof(checkpoints[0]); ++i) {
        const double cp = checkpoints[i];
        double clock = (i == 0) ? 0.0 : checkpoints[i - 1];
        double since = 0.0;
        while (clock < cp - 1e-9) {
            double step = 1.0 / 60.0;
            if (since >= 1.5) { step = 0.75; since = 0.0; }   // a stalled loop
            since += step;
            clock += step;
            video_.setClockOverride(clock);
            if (!video_.tick(r_)) break;
        }
        video_.setClockOverride(cp);
        if (!video_.tick(r_)) { /* the clip ended early */ }
        const size_t want = (size_t)(cp * fps + 1e-6);
        const size_t got = video_.frame();
        const bool hit = got == want;
        ok = ok && hit;
        LOG("movie-sync[clock]: clock=%.3fs frame=%zu expected=%zu (%+ld frames) %s",
            cp, got, want, (long)got - (long)want,
            hit ? "ok: derived from the clock" : "LAG: the frame index fell behind");
    }
    video_.setClockOverride(-1.0);

    // --- 2) real playback: video time vs the mixer position ------------------
    const double secs = std::max(0.0f, opt_.movieSyncSeconds);
    if (secs > 0.0) {
        video_.setAudioEnabled(true);
        video_.play(true);
        if (!video_.audioReady()) {                  // no device at all: stay silent
            video_.setAudioEnabled(false);
            video_.play(false);
        }
        LOG("movie-sync: real playback for %.1fs, soundtrack %s", secs,
            video_.audioReady() ? "on" : "off (wall clock only)");
        const Uint32 t0 = SDL_GetTicks();
        double next = 0.0;
        while (true) {
            const double wall = (double)(SDL_GetTicks() - t0) / 1000.0;
            if (wall > secs) break;
            if (!video_.tick(r_)) break;             // reached the end
            if (wall + 1e-9 >= next) {
                next = wall + 1.0;
                line("movie-sync");
            }
            if (SDL_Window* w = r_.getWindowHandle()) SDL_GL_SwapWindow(w);
            SDL_Delay(1);
        }
        for (size_t i = 1; i < sizeof(checkpoints) / sizeof(checkpoints[0]); ++i) {
            const double cp = checkpoints[i];
            if (!video_.seek(r_, cp)) { ok = false; break; }
            const Uint32 s0 = SDL_GetTicks();
            while (SDL_GetTicks() - s0 < 250) {      // let the mixer move
                video_.tick(r_);
                if (SDL_Window* w = r_.getWindowHandle()) SDL_GL_SwapWindow(w);
                SDL_Delay(1);
            }
            char tag[32];
            snprintf(tag, sizeof tag, "movie-sync[seek@%.0fs]", cp);
            line(tag);
        }
    }
    video_.stop();
    if (secs > 0.0 && !mixerRan) {
        LOG("movie-sync: no mixer ran - the A/V drift could not be measured "
            "(set SDL_AUDIODRIVER=dummy or plug in a device)");
        ok = false;
    }
    LOG("movie-sync: %s", ok ? "frame index follows the clock at every checkpoint"
                             : "FAILED: the picture does not follow its clock");
    return ok;
}

// --tex-update N: decode frame N of the pack and push it into a texture that was
// created from frame 0 with Renderer::updateTexture() - the streaming path the
// video player uses.  Meant to be verified pixel-wise by tools/shot_test.py.
bool App::texUpdateRun() {
    Video v;
    if (!openClip(v, opt_.movieName)) return false;
    const int w = v.clip().w, h = v.clip().h;
    const int frames = v.clip().frames;
    int idx = opt_.texUpdate;
    if (frames > 0) idx %= frames;
    if (idx < 0) idx += frames;
    if (!v.loadFrame(0)) { LOG("texture: frame 0 failed to decode"); return false; }
    const unsigned tex = r_.createTexture(v.rgba().data(), w, h);
    if (!tex) { LOG("texture: createTexture(%dx%d) failed", w, h); return false; }
    if (!v.loadFrame((size_t)idx)) { LOG("texture: frame %d failed to decode", idx); return false; }
    double lum = 0.0;
    const std::vector<uint8_t>& rgba = v.rgba();
    for (size_t i = 0; i + 3 < rgba.size(); i += 4)
        lum += 0.299 * rgba[i] + 0.587 * rgba[i + 1] + 0.114 * rgba[i + 2];
    lum /= (double)(rgba.size() / 4) * 255.0;
    const bool ok = r_.updateTexture(tex, rgba.data(), w, h);
    LOG("texture: updateTexture(tex=%u, %dx%d, frame %d/%d) -> %s (mean luminance %.3f)",
        tex, w, h, idx, frames, ok ? "ok" : "FAILED", lum);
    if (!ok) { v.releaseTexture(r_); return false; }
    r_.setSky(0.0f, 0.0f, 0.0f);
    r_.clear();
    r_.begin2D();
    r_.quad2D(tex, 0, 0, (float)r_.width_, (float)r_.height_, 1, 1, 1, 1);
    r_.end2D();
    LOG("texture: frame %d drawn full screen -> %s (%u updates, last tex %u)",
        idx, opt_.out.c_str(), r_.textureUpdates(), r_.lastUpdatedTexture());
    r_.screenshot(opt_.out);
    v.releaseTexture(r_);
    return true;
}

// --loading-shot FILE: the loading screen stand-alone, for tests/docs.  The
// animation phase is fixed so the picture is deterministic.
bool App::loadingShotRun() {
    Loading lg;
    lg.begin("ЗАГРУЗКА МИРА");
    const float p = std::min(1.0f, std::max(0.0f, opt_.loadingAt));
    lg.step((int)(p * 100.0f + 0.5f), 100, "ENV/aa.b3d");
    lg.setNote("HT2:RE");
    lg.update(0.55f);
    r_.setSky(0.02f, 0.025f, 0.035f);
    r_.clear();
    lg.draw(r_, font_);
    LOG("loading: screen '%s' at %d%% (item 'ENV/aa.b3d', version '%s', %dx%d)",
        "ЗАГРУЗКА МИРА", (int)(p * 100.0f + 0.5f), Loading::versionString(),
        r_.width_, r_.height_);
    r_.screenshot(opt_.loadingShot);
    return true;
}

// One frame of the loading screen: animate, pump the event queue (so the window
// is responsive while the world loads) and swap, otherwise nothing would ever be
// presented before the world is ready.
void App::pumpLoading(Loading& lg, float dt) {
    lg.update(dt);
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) quitRequested_ = true;
    }
    r_.setSky(0.02f, 0.025f, 0.035f);
    r_.clear();
    lg.draw(r_, font_);
    // Test hook: capture one frame of the *real* menu -> world transition.
    if (const char* dump = getenv("HT2_LOADING_DUMP")) {
        if (!loadingDumped_ && *dump) {
            loadingDumped_ = true;
            r_.screenshot(dump);
            LOG("loading: mid-transition frame -> %s", dump);
        }
    }
    if (SDL_Window* w = r_.getWindowHandle()) SDL_GL_SwapWindow(w);
}

// Everything the MENU -> WORLD transition has to do.  Also reachable through
// --autostart so the transition itself can be verified headlessly.
// The tile textures (and the truck skins) are the expensive part and need a live
// GL context, so this is where the original's loading screen is shown: each item
// is reported through Loading::step() and presented with a real swap.
void App::enterWorld() {
    stopMenuMusic();                 // the menu track does not follow into the world
    if (audio_.ready()) audio_.engineStart();
    state_ = State::World;

    Loading lg;
    const bool showLoading = r_.getWindowHandle() != nullptr;
    int done = 0, total = 0;
    if (showLoading) {
        lg.begin("ЗАГРУЗКА МИРА");
        total = (int)worldTileNames_.size() + 3;
        lg.step(0, total, "ENV");
        pumpLoading(lg, 0.0f);       // first frame: draw immediately
    }
    // A fixed 1/60 s per step: the screen animates a little between items
    // without any wall-clock dependency (the transition stays deterministic).
    auto step = [&](const std::string& item) {
        if (!showLoading) return;
        lg.step(++done, total, item);
        pumpLoading(lg, 1.0f / 60.0f);
    };

    if (worldTex_.count() == 0) {
        for (const std::string& base : worldTileNames_) {
            worldTex_.load(r_, base);
            step(base);
        }
        step(opt_.dataDir + "/COMMON/TRUCKS");
        worldTex_.load(r_, opt_.dataDir + "/COMMON/TRUCKS");   // parked car skins
        // Weather/prop materials of the original's COMMON.RES: rain, snow, drop
        // (the particles), sun_mat/moon_mat and the sky_mat* dome.  `noload`
        // entries (the nine 256x256 sky textures) are skipped.
        worldTex_.load(r_, opt_.dataDir + "/COMMON/COMMON", true);
    }
    r_.setSceneMeshes(&worldMeshes_, &worldTex_);
    // Give the generated terrain mesh the tiles' own ground texture (grass or
    // dirt) so the ground between the tiles is not a flat colour.
    pickGroundTexture();
    // The original top LED strip + minimap (src/ui/Hud.cpp).  The `--scene
    // world` path loads it in run(); the menu -> world transition did not, so
    // --autostart drew the fallback dashboard instead and the two paths never
    // matched (the "autostart reaches the same world" check).
    if (!hud_.loaded()) hud_.load(r_, opt_.dataDir);
    if (!haveTrucks_) {
        haveTrucks_ = trucks_.load(opt_.dataDir + "/COMMON/TRUCKS.B3D");
        if (haveTrucks_) trucks_.buildNamedScene(opt_.vehicle);
    }
    if (haveTrucks_) {
        if (vehicleTex_.count() == 0)
            vehicleTex_.load(r_, opt_.dataDir + "/COMMON/TRUCKS");
        r_.setVehicleScene(&trucks_, &vehicleTex_);
        // The 3D cockpit instruments live in COMMON/CABINES.B3D (docs/cockpit.md);
        // Cockpit is a no-op when the file or the model's cab is missing.
        cockpit_.load(r_, opt_.dataDir, opt_.vehicle);
        // Rear-view mirror: the original's <Brand>SLRSpace mount defines the
        // reflection camera (docs/mirror.md).
        mirror_.load(r_, opt_.dataDir, &trucks_.anchorList, opt_.vehicle);
    }
    vehicle3d_.setGround(&ground_);
    vehicle3d_.reset(spawn_[0], spawn_[1], spawn_[2]);
    step("COMMON/TRUCKS.B3D");
    indexWayNodes();
    buildMinimapTexture();
    step("road network / minimap");
    prepareNodeEvents();
    if (opt_.traffic > 0 && traffic_.empty()) spawnTraffic();
    initEconomy();
    applyEconomyActions();
    step("scripts");
    if (showLoading) {
        lg.step(total, total, "готово");
        pumpLoading(lg, 1.0f / 60.0f);
        lg.end();
        LOG("loading: world ready after %d steps (version '%s')", done,
            Loading::versionString());
    }
    LOG("app: entered world (tiles %zu, vehicle '%s' %zu submeshes, ground %s)",
        worldTileNames_.size(), opt_.vehicle.c_str(), trucks_.submeshes().size(),
        ground_.empty() ? "none" : "ready");
}

// Camera for the world.  Chase = behind the truck; cockpit = the driver's eye
// inside the cab (the shipped models are solid shells, so the truck is hidden -
// with backface culling on we would otherwise look straight through the cab);
// bonnet = just above the windscreen looking forward.
void App::viewPose(float& ex, float& ey, float& ez, float& yaw, float& pitch, bool& drawTruck) {
    float fx, fz;
    vehicle3d_.forward(fx, fz);
    const float vx = vehicle3d_.x(), vy = vehicle3d_.y(), vz = vehicle3d_.z();
    if (viewMode_ == 0) {
        const float dist = 30.0f, height = 14.0f;
        ex = vx - fx * dist; ez = vz - fz * dist; ey = vy + height;
        yaw = atan2f(fx, -fz) * 180.0f / PI; pitch = -7.0f;
        drawTruck = true;
        return;
    }
    // local truck frame: +x right, +y up, +z forward.
    float lx = -0.75f, ly = viewMode_ == 1 ? 2.35f : 3.55f;
    float lz = viewMode_ == 1 ? 2.10f : 3.55f;
    float cockpitPitch = 0.0f;      // extra downward pitch for the instruments
    if (viewMode_ == 1) {
        // Preferred: the eye derived from the cockpit model itself
        // (src/game/Cockpit.cpp), which sits on the seat in front of the dash and
        // makes the 3D instruments visible (docs/cockpit.md).
        if (cockpit_.eyePose(lx, ly, lz, cockpitPitch)) {
            // lx/ly/lz are already in the vehicle-local (x, height, z_long) form
        } else {
        // Fallback (no CABINES.B3D / no cab for this model): the original looks
        // up the B3D node <Brand>BoothSpace (docs/original_ui.md 3.2).
        const B3D::Anchor* a = nullptr;
        std::string base = opt_.vehicle;
        while (!base.empty() && !isalpha((unsigned char)base.back())) base.pop_back();
        for (const auto& kv : trucks_.anchorList) {
            if (kv.first.size() < 10) continue;
            if (kv.first.compare(kv.first.size() - 10, 10, "BoothSpace") != 0) continue;
            if (!a || kv.first.rfind(base, 0) == 0) a = &kv.second;
            if (kv.first.rfind(base, 0) == 0) break;
        }
        if (a) {
            // The anchor's triple is (x, z_long, height) like every b3d record;
            // only its x/height map onto the local eye frame directly, so the
            // translation is used as a rough mount point (the cockpit module
            // above supersedes it whenever CABINES.B3D is present).
            lx = a->t[0]; ly = a->t[2]; lz = a->t[1];
            LOG("cockpit: camera on the BoothSpace node (%.2f %.2f %.2f)", lx, ly, lz);
        }
        }
    }
    // keep the matrix alive: Vehicle::modelMatrix() returns by value and `m`
    // would dangle if it pointed into the temporary
    const Mat4 vm = vehicle3d_.modelMatrix();
    const float* m = vm.m;
    ex = m[0] * lx + m[4] * ly + m[8] * lz + m[12];
    ey = m[1] * lx + m[5] * ly + m[9] * lz + m[13];
    ez = m[2] * lx + m[6] * ly + m[10] * lz + m[14];
    // look along the truck's forward axis
    const float fwdX = m[8], fwdY = m[9], fwdZ = m[10];
    yaw = atan2f(fwdX, -fwdZ) * 180.0f / PI;
    float len = sqrtf(fwdX * fwdX + fwdY * fwdY + fwdZ * fwdZ);
    pitch = (len > 1e-6f) ? asinf(fwdY / len) * 180.0f / PI : 0.0f;
    pitch += cockpitPitch;
    // The truck stays visible: with backface culling on the cab shell is
    // invisible from the inside, while the dashboard/wheel/mirrors (if the model
    // has them) show up as a cockpit.
    drawTruck = true;
}

// Time of day drives the sun, the sky and the ambient term (and later the
// DAYONLY/DAYGW gates of the scripts).
void App::updateClock(float dt) {
    if (opt_.dayLength > 1.0f) {
        timeOfDay_ += dt * 24.0f / opt_.dayLength;
        while (timeOfDay_ >= 24.0f) timeOfDay_ -= 24.0f;
    }
}

// Rain and snow: the original builds them from the COMMON.RES particle
// materials `rain` (16x16 txr), `snow` (16x16) and `drop` (32x32, the glass
// droplets), driven by the per-preset `frpl_*` density.  The engine's own
// class pair CRain/CSnow spawns them as 3D sprites around the camera and
// `%%s_1.msk`/`%%s_2.msk` are the wiper/glass masks.
//
// Here they are a deterministic 2D field over the frame: the positions come
// from a fixed hash of the particle index and the motion from the simulated
// clock, so a screenshot at a given time is byte-identical between runs.
void App::drawWeatherOverlay() {
    if (opt_.noWeatherFx || weatherAmount_ <= 0.001f) return;
    drawWeatherStreaks(weatherAmount_, weatherKind_ == 2);
}

void App::drawWeatherStreaks(float amount, bool snow) {
    const float W = (float)r_.width_, H = (float)r_.height_;
    const unsigned tex = worldTex_.count() ? worldTex_.textureFor(snow ? "snow" : "rain") : 0u;
    const float a = std::min(1.0f, amount);
    const int N = snow ? 300 : 460;
    const float t = weatherAnim_;
    // Additive: the textures are bright shapes on black, so the black part of
    // each quad must not darken the frame.
    r_.setBlendAdditive(true);
    uint32_t seed = weatherSeed_;
    for (int i = 0; i < N; ++i) {
        // xorshift per particle: reproducible without touching the game RNG
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        float r0 = (float)(seed & 0xFFFFu) / 65535.0f;
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        float r1 = (float)(seed & 0xFFFFu) / 65535.0f;
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        float r2 = (float)(seed & 0xFFFFu) / 65535.0f;
        if (snow) {
            const float size = 3.0f + 8.0f * r2;
            const float sp = 26.0f + 34.0f * r1;                  // px/s, slow
            float y = fmodf(r0 * (H + 80.0f) + sp * t, H + 80.0f) - 40.0f;
            float x = fmodf(r1 * (W + 80.0f) + sinf((t + r2 * 9.0f) * 0.7f) * 18.0f + 40.0f,
                            W + 80.0f) - 40.0f;
            r_.quad2D(tex, x, y, size, size, 1.05f * a, 1.05f * a, 1.10f * a, 1.0f);
        } else {
            const float len = 44.0f + 60.0f * r2;
            const float sp = 900.0f + 700.0f * r1;                // px/s, fast
            float y = fmodf(r0 * (H + 120.0f) + sp * t, H + 120.0f) - 60.0f;
            float x = fmodf(r1 * (W + 160.0f) + 0.30f * (y + 60.0f) + 80.0f, W + 160.0f) - 80.0f;
            r_.line2D(tex, x, y, x - len * 0.30f, y + len, 3.0f,
                      1.70f * a, 1.85f * a, 2.10f * a, 1.0f);
        }
    }
    r_.setBlendAdditive(false);
}

void App::applyLighting() {
    // sun elevation: 0 at 06:00, peaks at 13:00, sets at 20:00
    float t = (timeOfDay_ - 6.0f) / 14.0f;                  // 0..1 over the day
    float elev = sinf(std::min(1.0f, std::max(0.0f, t)) * 3.14159265f);
    // full daylight shortly after sunrise / before sunset (the sky is already
    // bright at 09:00, like the original), dusk only near the horizon
    float day = std::min(1.0f, std::max(0.0f, (elev - 0.05f) / 0.35f));
    float dawn = std::exp(-powf((timeOfDay_ - 6.5f) / 1.2f, 2.0f));
    float dusk = std::exp(-powf((timeOfDay_ - 19.5f) / 1.2f, 2.0f));
    float warm = std::min(1.0f, dawn + dusk);

    // --- weather: sky / fog / lighting from the original's WEATHER.INI -----
    // The preset supplies the fog colour and both fog distances, the sky and
    // ambient colours and the particle density for the current good/bad state
    // and time-of-day phase.  When the file is missing the old analytic sky is
    // used so the engine still runs without the shipped data.
    Weather::Params wp;
    const bool haveWeather = weather_.params(opt_.weatherSet, weatherBad_, weatherPhase_, wp);
    float horizon[3], zenith[3], ambient;
    if (haveWeather) {
        horizon[0] = wp.fog[0]; horizon[1] = wp.fog[1]; horizon[2] = wp.fog[2];
        // Zenith: the palette's "bright sky" colour (COMMON.RES col_sky_b =
        // 108,136,169) lit by the preset's sky ambient and sun colour, which is
        // how the original tints its sky dome (sky_mat1..5 are textured quads -
        // a flat clear colour cannot reproduce the clouds, see docs/weather.md).
        const float b[3] = {0.42f, 0.53f, 0.66f};
        for (int k = 0; k < 3; ++k)
            zenith[k] = b[k] * (wp.skyamb[k] + 0.5f * wp.sunsky[k]);
        float la = (wp.skyamb[0] + wp.skyamb[1] + wp.skyamb[2]) / 3.0f;
        ambient = 0.22f + 0.55f * la;
    } else {
        horizon[0] = std::min(1.0f, 0.05f + 0.45f * day + 0.55f * warm);
        horizon[1] = std::min(1.0f, 0.07f + 0.62f * day + 0.25f * warm);
        horizon[2] = std::min(1.0f, 0.12f + 0.80f * day - 0.10f * warm);
        for (int k = 0; k < 3; ++k) zenith[k] = horizon[k];
        ambient = 0.26f + 0.32f * day;
    }

    // Draw distance.  The original's fog end (WEATHER.INI fogf_*) is the number
    // the far plane, the fog and the scene cull are all derived from; 900 units
    // in fair weather, 1300 on the mountain highway, 150..350 in the tunnels.
    // --view-distance overrides it and keeps the near/far ratio of the preset.
    float far = haveWeather ? wp.fogFar : (day > 0.15f ? 1400.0f : 900.0f);
    float near = haveWeather ? wp.fogNear : far * 0.3f;
    if (opt_.viewDistance > 1.0f) {
        float k = (far > 1.0f) ? (opt_.viewDistance / far) : 1.0f;
        far = opt_.viewDistance;
        near = near * k;
    }
    if (far < 60.0f) far = 60.0f;
    if (near < 1.0f) near = 1.0f;
    if (near > far * 0.9f) near = far * 0.9f;
    viewDistance_ = far;
    // Renderer::setFog() is the fog END; the shader's smoothstep-style curve is
    // normalised on it, so the near value only biases how early the fog starts.
    r_.setFog(far);
    r_.setFogColor(horizon[0], horizon[1], horizon[2]);
    r_.setSkyZenith(zenith[0], zenith[1], zenith[2]);
    r_.setSky(horizon[0], horizon[1], horizon[2]);
    r_.setAmbient(ambient);
    // Precipitation is overcast: the directional term is damped.
    r_.setSunAmount(weatherAmount_ > 0.0f ? 0.35f : (weatherBad_ ? 0.6f : 1.0f));
    // sun direction: rises in the east (+x), sets in the west (-x)
    float az = (timeOfDay_ - 6.0f) / 14.0f * 3.14159265f;
    r_.setSun(cosf(az) * 0.9f, std::max(0.12f, elev), 0.25f);
    // One line per change: the draw-distance numbers the tests and the docs
    // quote, plus which WEATHER.INI preset/phase is in force.
    int sig = (int)(far * 0.5f) * 128 + opt_.weatherSet * 8 + (weatherBad_ ? 4 : 0) +
              weatherPhase_ + (weatherKind_ + 1) * 1024;
    if (sig != lastWeatherLog_) {
        lastWeatherLog_ = sig;
        const char* kind = weatherKind_ == 1 ? "rain" : (weatherKind_ == 2 ? "snow" : "clear");
        LOG("view: draw distance %.0f m (fog near %.0f m from WEATHER%d %s %s)%s, far plane %.0f m, "
            "scene cull %.0f m, weather %s",
            far, near, opt_.weatherSet, weatherBad_ ? "bad" : "good",
            Weather::phaseName(weatherPhase_),
            opt_.viewDistance > 1.0f ? " [--view-distance]" : "", r_.farPlane(), r_.farPlane(), kind);
    }
    (void)near;
    (void)haveWeather;
}

// Weather state for this frame: the good/bad boolean, the time-of-day phase and
// the `--weather-cycle` ramp.  Deterministic: it only advances with the
// simulated clock, so a screenshot of a fixed hour and phase always matches.
void App::applyWeather(float dt) {
    weatherPhase_ = Weather::phaseForHour(timeOfDay_);
    if (opt_.weatherCycle) {
        weatherTime_ += dt;
        float period = 18.0f;                      // 6 s clear, 6 s rain, 6 s snow
        float u = fmodf(weatherTime_, period) / period;
        int slot = (int)(u * 3.0f);
        float local = u * 3.0f - (float)slot;
        // ramp in over the first fifth of each slot
        float ramp = std::min(1.0f, local * 5.0f);
        if (slot == 0) { weatherKind_ = 0; weatherBad_ = false; weatherAmount_ = 0.0f; }
        else if (slot == 1) { weatherKind_ = 1; weatherBad_ = true; weatherAmount_ = ramp; }
        else { weatherKind_ = 2; weatherBad_ = true; weatherAmount_ = ramp; }
    }
    // Cycle while keeping the amount consistent for the explicit --weather flag.
    if (weatherAmount_ > 0.0f && weatherKind_ == 0 && !opt_.weatherCycle) weatherAmount_ = 1.0f;
}

void App::drawWorld(float dt, bool interactive) {
    interactivePass_ = interactive;
    // While an ESC screen is up the world is *paused*: no clock, no vehicle, no
    // traffic, no events - only the frozen frame behind the panel.
    const bool paused = esc_.active();
    if (!paused) updateClock(dt);
    applyWeather(dt);
    weatherAnim_ += paused ? 0.0f : (dt > 0.25f ? 0.25f : dt);
    applyLighting();
    r_.clear();

    // --- input (keyboard + game controller) --------------------------------
    Vehicle::Input in;
    if (interactive) {
        Input::State st = input_.poll(dt);           // exactly once per frame
        const bool swallowMenu = escSwallowMenu_;    // see App.h: one-frame guard
        escSwallowMenu_ = false;
        if (paused) {
            // The panel owns the input; the SDL event path above already turned
            // Esc / pad B into escBack_ and the pad buttons into nav/activate.
        } else {
            in.throttle = std::max(-1.0f, std::min(1.0f, st.throttle - st.brake));
            in.steer = st.steer;
            in.handbrake = st.handbrake;
            if (st.viewToggle) viewMode_ = (viewMode_ + 1) % 3;
            if (st.buy) {
                const auto& offs = economy_.offers();
                for (size_t i = 0; i < offs.size(); ++i)
                    if (!offs[i].owned && offs[i].price <= economy_.money() && economy_.buy(i)) {
                        switchPlayerVehicle(offs[i].model);
                        break;
                    }
            }
            if (st.job) {
                std::vector<size_t> near = economy_.jobsNear(vehicle3d_.x(), vehicle3d_.z(), 120.0f);
                if (!near.empty()) economy_.accept(near[0]);
            }
            if (st.deliver) {
                long reward = 0;
                economy_.deliver(vehicle3d_.x(), vehicle3d_.z(), 120.0f, &reward);
            }
            if (st.map) {                            // fuel / repair (Tab / Back)
                long paid = 0;
                if (refuel(&paid)) {
                    LOG("service: refuelled for %ld, money %ld", paid, economy_.money());
                    input_.rumble(0.3f, 0.15f, 120);
                } else if (repair(&paid)) {
                    LOG("service: repaired for %ld, money %ld", paid, economy_.money());
                }
            }
            if (st.mapToggle) {                      // M / Select: the road map
                map_.toggle();                       // (the original's toggle)
                LOG("map: %s (M)", map_.visible() ? "shown" : "hidden");
            }
            if (st.menu && !swallowMenu) {           // open the in-game ESC menu
                esc_.open("main");
                esc_.setSlotText(saveSummary());
                if (audio_.ready()) audio_.engineStop();
                LOG("esc: world paused (Esc / Start)");
            }
        }
    }
    if (!esc_.active()) {
        vehicle3d_.update(in, dt);

        // The AI sees the player as a solid, moving leader: it queues behind a
        // stopped truck and shunts it when it cannot stop in time.
        {
            Traffic::Player p;
            p.valid = true;
            p.x = vehicle3d_.x();
            p.z = vehicle3d_.z();
            p.yaw = vehicle3d_.yaw();
            vehicle3d_.forward(p.vx, p.vz);
            p.vx *= vehicle3d_.speed();
            p.vz *= vehicle3d_.speed();
            p.radius = playerRadius_;
            p.length = playerLength_;
            traffic_.setPlayer(p);
        }
        traffic_.update(dt, vehicle3d_.x(), vehicle3d_.z(), opt_.trafficRadius);
        if (!opt_.noCollision) applyTrafficHits(dt);
        updateNodeEvents(dt);
        updateGates();
        updateAmbushes();
        updateSpeedCamera(dt);       // speed camera / fine panel
        economy_.tick(dt);           // the job's «ОСТ.ВРЕМЯ» contract clock
    }
    // Sound follows the world state: listener + engine loop + crash one-shots.
    // Skipped on the frame that leaves the world, so engineStop() is not undone.
    if (state_ == State::World && !esc_.active()) updateSceneAudio();

    // AI cars are solid for the player: Traffic::update() already pushed the
    // truck out of every car it touched (applyTrafficHits above); the old
    // single-circle nudge is gone with it.

    // --- world geometry ----------------------------------------------------
    float vx = vehicle3d_.x(), vy = vehicle3d_.y(), vz = vehicle3d_.z();
    float fx, fz;
    vehicle3d_.forward(fx, fz);

    if (opt_.camGiven) {
        // free camera (debug / deterministic screenshots)
        r_.drawSky(worldCam_[4]);
        r_.draw(worldCam_[0], worldCam_[1], worldCam_[2], worldCam_[3], worldCam_[4]);
        if (haveWorldScene_) r_.drawScene(worldCam_[0], worldCam_[1], worldCam_[2], worldCam_[3], worldCam_[4]);
        drawTraffic(worldCam_[0], worldCam_[1], worldCam_[2], worldCam_[3], worldCam_[4]);
    } else {
        float cx, cy, cz, camYaw, camPitch;
        bool drawTruck = true;
        viewPose(cx, cy, cz, camYaw, camPitch, drawTruck);
        // The cockpit camera sits inside the cab, well within the default 2.0
        // near plane, so it uses a closer one (Renderer keeps 2.0 everywhere
        // else; the value is restored below).
        r_.setNearPlane(viewMode_ == 1 ? 0.4f : 2.0f);
        // Cockpit with a working instrument cluster: cull the truck's exterior
        // shell (from inside the cab only its back faces would be visible, and
        // they would cover the whole screen) - the dashboard comes from the
        // Cockpit module instead.  Without a cockpit (no CABINES.B3D data for
        // this model) and in the bonnet view the previous two-sided behaviour
        // is kept.
        const bool cockpitInstruments = cockpit_.loaded() && viewMode_ == 1;
        r_.setVehicleTwoSided(!cockpitInstruments && viewMode_ != 0);
        r_.drawSky(camPitch);
        r_.draw(cx, cy, cz, camYaw, camPitch);
        if (haveWorldScene_) r_.drawScene(cx, cy, cz, camYaw, camPitch);
        drawTraffic(cx, cy, cz, camYaw, camPitch);
        if (haveTrucks_ && drawTruck) {
            r_.setModel(vehicle3d_.modelMatrix());
            r_.drawVehicleScene(cx, cy, cz, camYaw, camPitch);
            r_.clearModel();
        }
        // Cockpit instruments (src/game/Cockpit.cpp): only in the cockpit view,
        // and only from the deterministic vehicle state (speed/rpm/fuel).
        cockpit_.setVisible(viewMode_ == 1);
        cockpit_.draw(r_, vehicle3d_.modelMatrix(), vehicle3d_, cx, cy, cz, camYaw, camPitch);
        // Rear-view mirror (src/gfx/Mirror.cpp): re-renders the world from the
        // mirror mount into an offscreen target and blits it under the original
        // mirr*.msk frame.  Cockpit view only; one call, before the HUD's 2D pass.
        mirror_.setVisible(viewMode_ == 1);
        mirror_.draw(r_, vehicle3d_.modelMatrix());
        r_.setNearPlane(2.0f);
    }
    if (haveTrucks_ && opt_.camGiven) {
        r_.setModel(vehicle3d_.modelMatrix());
        r_.drawVehicleScene(worldCam_[0], worldCam_[1], worldCam_[2], worldCam_[3], worldCam_[4]);
        r_.clearModel();
    }

    if (getenv("HT2_CULL_DEBUG")) {
        static size_t last = (size_t)-1;
        if (r_.rangesDrawn() + r_.rangesCulled() != last) {
            last = r_.rangesDrawn() + r_.rangesCulled();
            LOG("cull: %zu of %zu world batches submitted (%zu culled by the %.0f m view distance), "
                "traffic %zu instances drawn / %zu culled",
                r_.rangesDrawn(), last, r_.rangesCulled(), viewDistance_,
                r_.trafficInstancesDrawn(), r_.trafficInstancesCulled());
        }
    }

    // --- HUD ---------------------------------------------------------------
    r_.begin2D();
    unsigned white = r_.whiteTex();
    drawWeatherOverlay();                    // rain streaks / snow flakes
    // The original has no debug box: speed, gear, money and the clock are in the
    // top LED strip (m16\tablo.txr, docs/original_ui.md 2.2).  The old
    // "<truck> POS ... YAW ... ODO ..." overlay is gone (docs/ui_sort.md 6.2).
    char buf[160];
    (void)vx; (void)vz;
    // the original HUD is a 1600x50 LED strip at the top of the screen; the
    // bottom panel stays only as a fallback when the art is unavailable
    if (hud_.loaded()) {
        Hud::Values hv;
        hv.speedKmh = vehicle3d_.speedKmh();
        hv.gear = vehicle3d_.gear();
        hv.rpm = vehicle3d_.rpm();
        hv.money = economy_.money();
        hv.clockHours = timeOfDay_;
        hv.level = (int)(vehicle3d_.damage() * 9.0f + 0.5f);
        // «ПОЗИЦИЯ»: the destination town and its distance (the original shows
        // «2 км 3»); «ОСТ.ВРЕМЯ»: the job's remaining contract time.
        std::string destName;
        if (const Economy::Job* aj = economy_.activeJob()) {
            hv.distanceToDest = economy_.distance2D(vehicle3d_.x(), vehicle3d_.z(),
                                                    aj->toX, aj->toZ);
            hv.haveDest = true;
            destName = aj->toTown;
            hv.destName = destName.c_str();
            // the route highlight of the map (the reference frame's blue box)
            map_.setRoute(aj->toX, aj->toZ, true, destName);
        } else {
            hv.haveDest = false;
            map_.setRoute(0, 0, false, std::string());
        }
        const std::string jt = opt_.jobTime.empty() ? economy_.jobTimeText() : opt_.jobTime;
        hv.jobTime = jt.c_str();
        hv.jobOverdue = economy_.jobOverdue();
        const int near = (int)economy_.jobsNear(vehicle3d_.x(), vehicle3d_.z(), 200.0f).size();
        hv.jobCount = near;
        if (msgTimer_ > 0.0f) {
            hv.message = msgText_;
            hv.messageTimer = msgTimer_;
            hv.messageAge = msgAge_;
            hv.messageNew = msgNew_;
        }
        hud_.draw(r_, font_, hv);
        if (!hud_.report().empty()) LOG("%s", hud_.report().c_str());
        drawMapOverlay();
    } else {
        drawDashboard();
    }
    // the fine panel sits above the map, like the original (ref_fine_panel_mirror)
    if (hud_.loaded() && fine_.timer > 0.0f) hud_.drawFine(r_, fine_);
    // A node-event / ambush banner stays as a small line as well, but the
    // original's message window is the pager inside the strip (Hud::draw).
    if (msgTimer_ > 0.0f && !msgText_.empty() && !hud_.loaded()) {
        float bw = (float)msgText_.size() * 8.0f * 1.6f + 40.0f;
        float bx = ((float)r_.width_ - bw) * 0.5f;
        float by = 96.0f;
        float a = msgTimer_ < 1.0f ? msgTimer_ : 1.0f;      // fade out
        r_.quad2D(white, bx, by, bw, 42, 0.05f, 0.08f, 0.12f, 0.72f * a);
        font_.draw(r_, msgText_, bx + 20.0f, by + 12.0f, 1.6f, 1.0f, 0.97f, 0.75f, a);
    }
    if (ambushesFired_ > 0 && msgTimer_ <= 0.0f)
        font_.draw(r_, "AMBUSH!", 16, (float)r_.height_ - 30, 1.3f, 1.0f, 0.4f, 0.35f, 1.0f);
    if (nearestService(true) >= 0 && vehicle3d_.fuel() < 0.95f)
        font_.draw(r_, "PRESS TAB TO REFUEL", 16, (float)r_.height_ - 30, 1.2f, 0.5f, 1.0f, 0.6f, 1.0f);
    else if (nearestService(false) >= 0 && vehicle3d_.damage() > 0.05f)
        font_.draw(r_, "PRESS TAB TO REPAIR", 16, (float)r_.height_ - 30, 1.2f, 0.5f, 0.9f, 1.0f, 1.0f);
    if (nearJobDist() >= 0.0f)
        font_.draw(r_, "PRESS T TO TAKE THE CARGO JOB", 16, (float)r_.height_ - 30, 1.2f,
                   1.0f, 0.9f, 0.4f, 1.0f);
    font_.draw(r_, "W/S: THROTTLE  A/D: STEER  SPACE: BRAKE  C: VIEW  B: BUY  T: JOB  Y: DELIVER  ESC: MENU",
               16, (float)r_.height_ - 16, 1.0f, 0.85f, 0.87f, 0.92f, 0.9f);
    r_.end2D();

    // --- in-game ESC screens: the original 1200-px panels, 1:1, on top -------
    if (esc_.active()) {
        if (esc_.screen() == "save" || esc_.screen() == "load") esc_.setSlotText(saveSummary());
        esc_.draw(r_, font_);
    }
}

// ---------------------------------------------------------------------------
// --traffic-report SECONDS: the headless measurement harness for the world AI.
//
// It runs the *same* simulation the game runs (the road graph, the lane path,
// the junction reservation, the car/player contacts), with the player parked in
// its lane, and prints one machine-readable RESULT line plus a human summary:
// mean speed, U-turns, off-road frames, pose jumps at junctions, contacts,
// junction collisions, the minimum inter-car gap and the damage both sides took.
// ---------------------------------------------------------------------------
int App::trafficReport() {
    const float step = 1.0f / 60.0f;
    const int steps = std::max(1, (int)(opt_.trafficReport / step));
    // Park the player *in a lane* of the road graph: a truck stopped in the
    // middle of the carriageway is the scenario the AI has to deal with (queue
    // behind it, shunt it), and the raw spawn point sits beside the lane line.
    {
        float lx = 0.0f, lz = 0.0f, lh = 0.0f, lyaw = 0.0f;
        int ledge = -1;
        // Prefer a long edge: a truck parked in the middle of a 15 m block
        // leaves the queue nowhere to stand but the junction.  40 m of edge in
        // front of it lets the queue form behind it, which is the behaviour the
        // report is meant to measure.
        if (!traffic_.nearestLane(vehicle3d_.x(), vehicle3d_.z(), lx, lz, lh, lyaw, ledge,
                                  40.0f) &&
            !traffic_.nearestLane(vehicle3d_.x(), vehicle3d_.z(), lx, lz, lh, lyaw, ledge)) {
            LOG("traffic-report: no road found near %.0f %.0f", vehicle3d_.x(), vehicle3d_.z());
        } else {
            // --traffic-report-oncoming: face the other way, so the cars in this
            // lane meet the parked truck head-on (the impact scenario).
            if (opt_.trafficReportOncoming) lyaw += 180.0f;
            LOG("traffic-report: player parked %s in the lane of edge %d (%.0f m) at "
                "%.0f %.0f yaw %.0f (was %.0f %.0f)",
                opt_.trafficReportOncoming ? "against the flow" : "with the flow",
                ledge, (double)traffic_.edgeLength(ledge), (double)lx, (double)lz,
                (double)lyaw, vehicle3d_.x(), vehicle3d_.z());
            vehicle3d_.reset(lx, lz, lyaw);
        }
    }
    LOG("traffic-report: %d steps of %.4f s (%.0f s), %zu cars, radius %.0f, "
        "player parked at %.0f %.0f",
        steps, step, (double)opt_.trafficReport, traffic_.cars().size(),
        (double)opt_.trafficRadius, vehicle3d_.x(), vehicle3d_.z());
    for (int i = 0; i < steps; ++i) {
        Traffic::Player p;
        p.valid = true;
        p.x = vehicle3d_.x();
        p.z = vehicle3d_.z();
        p.yaw = vehicle3d_.yaw();
        vehicle3d_.forward(p.vx, p.vz);
        p.vx *= vehicle3d_.speed();
        p.vz *= vehicle3d_.speed();
        p.radius = playerRadius_;
        p.length = playerLength_;
        traffic_.setPlayer(p);
        traffic_.update(step, vehicle3d_.x(), vehicle3d_.z(), opt_.trafficRadius);
        const Traffic::PlayerHit& h = traffic_.playerHit();
        if (h.hit) {
            vehicle3d_.nudge(h.pushX, h.pushZ);
            vehicle3d_.addDamage(h.damage);
        }
    }
    // Cars held up by the parked player: in front of it, in its lane, stopped.
    float pfx = 0.0f, pfz = 0.0f;
    vehicle3d_.forward(pfx, pfz);
    const float prx = -pfz, prz = pfx;
    size_t queued = 0;
    float nearestQueue = 1e9f;
    for (const auto& c : traffic_.cars()) {
        const float dx = c.x - vehicle3d_.x(), dz = c.z - vehicle3d_.z();
        const float along = dx * pfx + dz * pfz;
        const float lat = dx * prx + dz * prz;
        if (along < 0.0f || along > 30.0f || std::fabs(lat) > 4.0f) continue;
        if (c.speed < 1.0f) {
            ++queued;
            nearestQueue = std::min(nearestQueue, along);
        }
    }
    const Traffic::Stats& s = traffic_.stats();
    const double offPct = s.frames ? 100.0 * (double)s.offRoadFrames / (double)s.frames : 0.0;
    LOG("traffic-report: mean speed %.2f m/s (%.1f km/h) over %.0f m driven",
        s.meanSpeed(), s.meanSpeed() * 3.6, s.distance);
    LOG("traffic-report: U-turns %zu (invariant), dead-end turn-arounds %zu, "
        "arrivals %zu, reroutes %zu, stalls %zu, respawns %zu, recycles %zu",
        s.uturns, s.deadEndTurns, s.arrivals, s.reroutes, s.stalls, s.respawns, s.recycles);
    LOG("traffic-report: off-road frames %zu of %zu (%.3f%%) > %.1f m off the lane, "
        "pose jumps %zu (max %.2f m), height steps >0.5 m %zu (max %.2f m)",
        s.offRoadFrames, s.frames, offPct, (double)traffic_.offRoadLimit(), s.jumps, s.maxJump,
        s.heightJumps, s.maxHeightStep);
    LOG("traffic-report: contacts %zu (car-vs-car %zu, car-vs-player %zu), junction hits %zu, "
        "overlap pair-frames %zu, min inter-car gap %.2f m",
        s.contacts, s.carContacts, s.playerContacts, s.junctionHits, s.overlapFrames,
        s.minGap > 1e8 ? 0.0 : s.minGap);
    LOG("traffic-report: damage dealt %.3f (to the player %.3f), player damage now %.3f, "
        "cars queued behind the player %zu (nearest %.1f m)",
        s.damageDealt, s.damageToPlayer, (double)vehicle3d_.damage(), queued,
        nearestQueue > 1e8 ? 0.0 : (double)nearestQueue);
    LOG("traffic-report: RESULT cars=%zu mean=%.2f uturns=%zu deadend=%zu offroad=%zu "
        "frames=%zu jumps=%zu maxjump=%.2f hjumps=%zu maxhstep=%.2f contacts=%zu "
        "carcontacts=%zu playercontacts=%zu junctionhits=%zu overlaps=%zu mingap=%.2f "
        "damage=%.3f playerdamage=%.3f queued=%zu arrivals=%zu respawns=%zu recycles=%zu",
        s.cars, s.meanSpeed(), s.uturns, s.deadEndTurns, s.offRoadFrames, s.frames,
        s.jumps, s.maxJump, s.heightJumps, s.maxHeightStep, s.contacts, s.carContacts,
        s.playerContacts, s.junctionHits, s.overlapFrames, s.minGap > 1e8 ? 0.0 : s.minGap,
        s.damageDealt, s.damageToPlayer, queued, s.arrivals, s.respawns, s.recycles);
    // Together with --screenshot this renders the *simulated* frame: a
    // deterministic picture of the traffic after N seconds of world AI.
    if (opt_.screenshot) {
        frame(0.0f, false);
        r_.screenshot(opt_.out);
        LOG("traffic-report: screenshot written to %s", opt_.out.c_str());
    }
    return 0;
}

int App::driveTest() {
    // deterministic headless drive: full throttle, no steering
    const float step = 1.0f / 60.0f;
    int steps = (int)(opt_.drive / step);
    Vehicle::Input in;
    in.throttle = 1.0f;
    float minY = 1e9f, maxY = -1e9f;
    if (audio_.ready()) audio_.engineStart();
    for (int i = 0; i < steps; ++i) {
        vehicle3d_.update(in, step);
        {
            Traffic::Player p;
            p.valid = true;
            p.x = vehicle3d_.x();
            p.z = vehicle3d_.z();
            p.yaw = vehicle3d_.yaw();
            vehicle3d_.forward(p.vx, p.vz);
            p.vx *= vehicle3d_.speed();
            p.vz *= vehicle3d_.speed();
            p.radius = playerRadius_;
            p.length = playerLength_;
            traffic_.setPlayer(p);
        }
        traffic_.update(step, vehicle3d_.x(), vehicle3d_.z(), opt_.trafficRadius);
        if (!opt_.noCollision) applyTrafficHits(step);
        updateNodeEvents(step);
        updateSpeedCamera(step);     // the speed camera / fine of the drive test
        economy_.tick(step);         // the job's contract clock
        updateSceneAudio();          // engine loop + crash one-shots, same calls
        minY = std::min(minY, vehicle3d_.y());
        maxY = std::max(maxY, vehicle3d_.y());
        if ((i % 60) == 0 || i == steps - 1)
            LOG("drive: t=%5.1fs pos=(%8.1f,%8.1f,%8.1f) yaw=%6.1f speed=%6.1f km/h odo=%7.1f m",
                (i + 1) * step, vehicle3d_.x(), vehicle3d_.y(), vehicle3d_.z(),
                vehicle3d_.yaw(), vehicle3d_.speedKmh(), vehicle3d_.odometer());
    }
    LOG("drive: done, %d steps, odometer %.1f m, height range %.1f..%.1f",
        steps, vehicle3d_.odometer(), minY, maxY);
    if (audio_.ready())
        LOG("audio: engine loop after drive: %zu frames, %zu starts, pitch %.2f, gain %.2f",
            audio_.engineLoopFrames(), audio_.engineRestarts(),
            audio_.enginePitchRatio(), audio_.engineGain());
    const Traffic::Stats& st = traffic_.stats();
    LOG("drive: traffic cars=%zu mean=%.1f m/s dist=%.0f m uturns=%zu arrivals=%zu "
        "recycles=%zu respawns=%zu reroutes=%zu",
        st.cars, st.meanSpeed(), st.distance, st.uturns, st.arrivals, st.recycles,
        st.respawns, st.reroutes);
    LOG("drive: contacts %zu (car-vs-car %zu, car-vs-player %zu), junction hits %zu, "
        "overlaps %zu, min gap %.2f m, damage dealt %.3f (player %.3f)",
        st.contacts, st.carContacts, st.playerContacts, st.junctionHits, st.overlapFrames,
        st.minGap > 1e8 ? 0.0 : st.minGap, st.damageDealt, st.damageToPlayer);
    return 0;
}

} // namespace ht2

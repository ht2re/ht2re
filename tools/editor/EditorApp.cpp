#include "EditorApp.h"
#include "util/Log.h"

#include <SDL.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace ht2 {

namespace {

std::vector<std::string> tokens(const std::string& line) {
    std::vector<std::string> t;
    std::istringstream in(line);
    std::string w;
    while (in >> w) t.push_back(w);
    return t;
}

double num(const std::string& s, double dflt = 0.0) {
    char* end = nullptr;
    double v = strtod(s.c_str(), &end);
    return (end == s.c_str()) ? dflt : v;
}

int inum(const std::string& s, int dflt = 0) {
    char* end = nullptr;
    long v = strtol(s.c_str(), &end, 0);
    return (end == s.c_str()) ? dflt : (int)v;
}

} // namespace

void EditorApp::usage() const {
    fputs(
        "ht2editor - HT2:RE map + car-model editor (docs/editor.md)\n"
        "usage: ht2editor --map TILE | --vehicle NAME [options]\n"
        "  --data DIR          game data root (default ./original)\n"
        "  --map TILE          edit ENV/TILE.way (e.g. --map aa)\n"
        "  --vehicle NAME      edit COMMON/TRUCKS.B3D model NAME (e.g. ZilR)\n"
        "  --overlay FILE      overlay to read/write (default ht2edit.ovl)\n"
        "                      the shipped data is never modified\n"
        "  --view 2d|3d        map view mode (default 2d)\n"
        "  --select NAME       select this node (map) / part (model) at startup\n"
        "  --angle YAW[,PITCH] model camera angles (default 205,-18)\n"
        "  --cam x,y,z,yaw,pitch  explicit camera (map 3D view / model view)\n"
        "  --cmd 'CMD'         run one script command (repeatable)\n"
        "  --script FILE       run a command script\n"
        "  --list              print the loaded nodes/segments or parts/anchors, exit\n"
        "  --shot FILE         render one frame to FILE (BMP) and exit\n"
        "  --blank             render the background only (harness control frame)\n"
        "  --no-textures       do not decode .res textures\n"
        "  --size WxH          window/offscreen size (default 1280x720)\n"
        "\n"
        "commands: view 2d|3d | fit | select NAME | selectseg I | selectanchor NAME\n"
        "          moveto X Y Z | nudge DX DY DZ | setflags 0xN | addnode NAME X Y Z\n"
        "          delnode NAME | connect A B [W] | setseg I width|width1|attr V\n"
        "          setseg I del | part NAME hide|show | partmat NAME MAT\n"
        "          anchormove NAME DX DY DZ | anchorpos NAME X Y Z\n"
        "          anchorrot NAME DYAW DPITCH DROLL | angle YAW [PITCH] | zoom D\n"
        "          cam X Y Z YAW PITCH | save | shot FILE | list | report | echo TEXT\n",
        stderr);
}

// ---------------------------------------------------------------------------
int EditorApp::run(const EditorOptions& o) {
    opt_ = o;
    if (opt_.mapTile.empty() && opt_.vehicle.empty()) {
        fputs("ht2editor: give --map TILE or --vehicle NAME\n", stderr);
        usage();
        return 1;
    }
    // The overlay is the only file the editor writes; a missing one is normal.
    if (!ov_.load(opt_.overlay)) {
        LOG("editor: %s has errors, refusing to start (fix or delete it)", opt_.overlay.c_str());
        return 2;
    }
    // Colour keys of the shared palette, exactly like the game's loadData().
    SceneTextures::loadPalette(opt_.dataDir + "/COMMON/common1.res");

    const bool headless = !opt_.shot.empty() || !opt_.cmds.empty() || !opt_.script.empty() ||
                          opt_.list || opt_.blank;

    // --- load the model / map (no GL context needed) -----------------------
    if (!opt_.mapTile.empty()) {
        modeMap_ = true;
        if (!map_.load(r_, opt_.dataDir, opt_.mapTile, ov_)) return 4;
        if (!opt_.view.empty()) map_.setView3D(opt_.view == "3d");
        if (!opt_.select.empty()) map_.selectNode(opt_.select);
        // The 3D map context has to be registered with the renderer BEFORE
        // init(): Renderer builds the terrain/ground mesh once, from the
        // pointers it holds at that moment (exactly like App::loadData() does
        // before r_.init()).  Prepared up front when the run may show 3D.
        const bool want3d = map_.view3D() || !headless;
        if (!opt_.blank && want3d && opt_.textures) map_.prepare3D(r_);
    } else {
        modeModel_ = true;
    }

    if (!r_.init(opt_.w, opt_.h, headless)) {
        LOG("editor: renderer init failed");
        return 3;
    }
    if (!font_.initBuiltin(r_)) return 3;

    if (modeMap_) {
        map_.fitView(opt_.w, opt_.h);           // fit first, then honour --cam
        if (opt_.camGiven) map_.setCamera(opt_.cam);
        if (!opt_.blank && map_.view3D() && opt_.textures) {
            map_.prepare3D(r_);
            map_.texture3D(r_);
        }
    } else {
        if (!model_.load(r_, opt_.dataDir, opt_.vehicle, ov_)) return 4;
        if (opt_.select.empty()) {
            // default: the first part, so a bare --shot already shows a selection
            model_.selectPartIndex(0);
        } else {
            model_.selectPart(opt_.select);
        }
        model_.fitCamera(opt_.w, opt_.h);
        if (opt_.angleGiven) model_.setAngles(opt_.angle[0], opt_.angle[1]);
    }

    if (opt_.list) {
        if (modeMap_) map_.printList(); else model_.printList();
        reportState();
        r_.shutdown();
        return 0;
    }

    // --- script ------------------------------------------------------------
    for (const std::string& c : opt_.cmds)
        if (!exec(c)) LOG("editor: command failed: %s", c.c_str());
    if (!opt_.script.empty() && !runScript(opt_.script)) {
        r_.shutdown();
        return 5;
    }

    if (!opt_.shot.empty()) takeShot(opt_.shot);

    if (headless) {
        reportState();
        r_.shutdown();
        return 0;
    }

    // --- interactive -------------------------------------------------------
    SDL_Window* win = r_.getWindowHandle();
    SDL_StartTextInput();
    LOG("editor: interactive %s editor (S saves to %s, ESC quits)",
        modeMap_ ? "map" : "model", opt_.overlay.c_str());
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { running = false; continue; }
            if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_ESCAPE) { running = false; continue; }
                if (e.key.keysym.sym == SDLK_s) {
                    ov_.save(opt_.overlay);
                    LOG("editor: overlay saved to %s (%zu records)", opt_.overlay.c_str(),
                        ov_.records());
                    continue;
                }
            }
            if (modeMap_) map_.event(r_, e, opt_.w, opt_.h);
            else model_.event(r_, e, opt_.w, opt_.h);
        }
        if (modeMap_ && map_.view3D() && opt_.textures) map_.texture3D(r_);
        renderFrame();
        if (win) SDL_GL_SwapWindow(win);
        SDL_Delay(16);
    }
    // A quit is not a save: the user must ask for it explicitly (or via a
    // script `save`), so an accidental key cannot rewrite the overlay.
    if (!ov_.empty())
        LOG("editor: %zu overlay record(s) NOT saved (press S to save)", ov_.records());
    r_.shutdown();
    return 0;
}

// ---------------------------------------------------------------------------
void EditorApp::renderFrame() {
    ++frames_;
    const unsigned white = r_.whiteTex();
    if (opt_.blank) {
        // The harness's control frame: the same background, none of the content.
        r_.setFogColor(0.09f, 0.10f, 0.13f);
        r_.clear();
        r_.begin2D();
        r_.quad2D(white, 0, 0, (float)opt_.w, 34, 0.05f, 0.06f, 0.08f, 0.85f);
        r_.end2D();
        return;
    }
    if (modeMap_) map_.draw(r_, font_, opt_.w, opt_.h);
    else model_.draw(r_, font_, opt_.w, opt_.h);
}

void EditorApp::takeShot(const std::string& file) {
    renderFrame();
    if (r_.screenshot(file)) {
        lastShot_ = file;
        LOG("editor: shot %s (%s)", file.c_str(), modeMap_ ? "map" : "model");
    } else {
        LOG("editor: shot %s FAILED", file.c_str());
    }
}

void EditorApp::reportState() const {
    if (modeMap_) {
        map_.printReport();
        LOG("editor: overlay path=%s records=%zu frames=%d",
            opt_.overlay.c_str(), ov_.records(), frames_);
    } else {
        model_.printReport();
        LOG("editor: overlay path=%s records=%zu frames=%d",
            opt_.overlay.c_str(), ov_.records(), frames_);
    }
}

// ---------------------------------------------------------------------------
// script commands
// ---------------------------------------------------------------------------
bool EditorApp::runScript(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        LOG("editor: cannot open script %s", path.c_str());
        return false;
    }
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        ++n;
        size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::vector<std::string> t = tokens(line);
        if (t.empty()) continue;
        if (!exec(line)) LOG("editor: %s:%d: command failed: %s", path.c_str(), n, line.c_str());
    }
    return true;
}

bool EditorApp::exec(const std::string& line) {
    std::vector<std::string> t = tokens(line);
    if (t.empty()) return true;
    const std::string& c = t[0];
    auto need = [&](size_t n) {
        if (t.size() < n) LOG("editor: '%s' needs %zu argument(s)", c.c_str(), n - 1);
        return t.size() >= n;
    };

    if (c == "echo") { LOG("editor: %s", line.substr(4).c_str()); return true; }
    if (c == "list") {
        if (modeMap_) map_.printList(); else model_.printList();
        return true;
    }
    if (c == "report") { reportState(); return true; }
    if (c == "save") {
        if (!ov_.save(opt_.overlay)) return false;
        LOG("editor: overlay saved: %s (%zu records)", opt_.overlay.c_str(), ov_.records());
        return true;
    }
    if (c == "shot") { if (!need(2)) return false; takeShot(t[1]); return true; }
    if (c == "view") {
        if (!need(2)) return false;
        if (modeMap_) {
            map_.setView3D(t[1] == "3d");
            if (map_.view3D() && opt_.textures) map_.texture3D(r_);
        }
        return true;
    }
    if (c == "fit") {
        if (modeMap_) map_.fitView(opt_.w, opt_.h); else model_.fitCamera(opt_.w, opt_.h);
        return true;
    }
    if (c == "cam") {
        if (!need(6)) return false;
        float cam[5];
        for (int i = 0; i < 5; ++i) cam[i] = (float)num(t[(size_t)i + 1]);
        if (modeMap_) map_.setCamera(cam);
        else { model_.setAngles(cam[3], cam[4]); model_.setDistance(cam[0]); }
        return true;
    }
    if (c == "angle") {
        if (!need(2)) return false;
        float y = (float)num(t[1]);
        float p = t.size() > 2 ? (float)num(t[2]) : opt_.angle[1];
        model_.setAngles(y, p);
        return true;
    }
    if (c == "zoom") { if (!need(2)) return false; model_.setDistance((float)num(t[1])); return true; }

    // --- map ---------------------------------------------------------------
    if (modeMap_) {
        if (c == "select") { if (!need(2)) return false; return map_.selectNode(t[1]); }
        if (c == "selectseg") { if (!need(2)) return false; map_.selectSegIndex(inum(t[1], -1)); return true; }
        if (c == "moveto") {
            if (!need(4)) return false;
            return map_.moveNodeAbs(map_.selectedNode(), num(t[1]), num(t[2]), num(t[3]));
        }
        if (c == "nudge") {
            if (!need(4)) return false;
            return map_.nudgeNode(map_.selectedNode(), num(t[1]), num(t[2]), num(t[3]));
        }
        if (c == "setflags") {
            if (!need(2)) return false;
            return map_.setNodeFlags(map_.selectedNode(), (uint32_t)inum(t[1]));
        }
        if (c == "addnode") {
            if (!need(5)) return false;
            return map_.addNode(t[1], num(t[2]), num(t[3]), num(t[4]),
                                t.size() > 5 ? t[5] : std::string());
        }
        if (c == "delnode") { if (!need(2)) return false; return map_.deleteNode(t[1]); }
        if (c == "connect") {
            if (!need(3)) return false;
            return map_.connectNodes(t[1], t[2], t.size() > 3 ? num(t[3]) : 8.0);
        }
        if (c == "setseg") {
            if (!need(4)) return false;
            int idx = inum(t[1], -1);
            const Way::Seg* s = map_.seg(idx);
            if (!s) { LOG("editor: setseg: no segment %d", idx); return false; }
            int attr = s->attr;
            double w = s->width, w1 = s->width1;
            if (t[2] == "width") w = num(t[3]);
            else if (t[2] == "width1") w1 = num(t[3]);
            else if (t[2] == "attr") attr = inum(t[3]);
            else if (t[2] == "del") return map_.deleteSeg(idx);
            else { LOG("editor: setseg: unknown field '%s'", t[2].c_str()); return false; }
            return map_.setSegFields(idx, attr, w, w1);
        }
        return false;
    }

    // --- model -------------------------------------------------------------
    if (c == "select" || c == "selectpart") {
        if (!need(2)) return false;
        return model_.selectPart(t[1]);
    }
    if (c == "selectanchor") { if (!need(2)) return false; return model_.selectAnchor(t[1]); }
    if (c == "part") {
        if (!need(3)) return false;
        if (t[2] == "hide") return model_.setPartHidden(t[1], true);
        if (t[2] == "show") return model_.setPartHidden(t[1], false);
        LOG("editor: part: expected hide|show");
        return false;
    }
    if (c == "partmat") { if (!need(3)) return false; return model_.setPartMaterial(t[1], t[2]); }
    if (c == "anchormove") {
        if (!need(5)) return false;
        return model_.moveAnchor(r_, t[1], num(t[2]), num(t[3]), num(t[4]));
    }
    if (c == "anchorpos") {
        if (!need(5)) return false;
        return model_.setAnchorPos(r_, t[1], num(t[2]), num(t[3]), num(t[4]));
    }
    if (c == "anchorrot") {
        if (!need(5)) return false;
        return model_.rotateAnchor(r_, t[1], num(t[2]), num(t[3]), num(t[4]));
    }
    return false;
}

} // namespace ht2

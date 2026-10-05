#pragma once
// ht2editor - the separate map + car-model editor (docs/editor.md).
//
// It is not part of the game: its own executable, its own window, its own CLI.
// It links the game's own Renderer / .way / .b3d / .res parsers and the shared
// overlay format, and it never writes to the shipped game data - every edit
// goes to one overlay text file that ht2re reads back with --overlay.
#include <signal.h>
#include <string>
#include <vector>
#include "gfx/Renderer.h"
#include "ui/Font.h"
#include "world/Overlay.h"
#include "MapEditor.h"
#include "ModelEditor.h"

namespace ht2 {

struct EditorOptions {
    std::string dataDir = "./original";
    std::string overlay = "ht2edit.ovl";
    std::string mapTile;            // --map TILE
    std::string vehicle;            // --vehicle NAME
    std::string shot;               // --shot FILE
    std::string select;             // --select NAME
    std::vector<std::string> cmds;  // --cmd "..." (repeatable)
    std::string script;             // --script FILE
    std::string view;               // --view 2d|3d
    int w = 1280, h = 720;
    float cam[5] = {0, 0, 0, 0, -45};
    bool camGiven = false;
    float angle[2] = {205.0f, -18.0f};
    bool angleGiven = false;
    bool list = false;
    bool blank = false;             // render the background only (harness control)
    bool textures = true;           // --no-textures: untextured (diagnostics)
};

class EditorApp {
public:
    int run(const EditorOptions& o);

private:
    bool exec(const std::string& line);          // one script command
    bool runScript(const std::string& path);
    void renderFrame();
    void takeShot(const std::string& file);
    void reportState() const;
    void usage() const;

    EditorOptions opt_;
    Renderer r_;
    Font font_;
    Overlay ov_;
    MapEditor map_;
    ModelEditor model_;
    bool modeMap_ = false, modeModel_ = false;
    int frames_ = 0;
    std::string lastShot_;
};

} // namespace ht2

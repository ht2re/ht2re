#pragma once
#include <string>

namespace ht2 {
class Renderer;
class Font;

// Loading screen: shown while the world tiles are parsed, in the style of the
// original - a dark background, a framed amber progress bar with the percent
// and the item being loaded, and the original's version string at the bottom.
//
// It owns no assets: LOAD.RES carries no loading-screen artwork (only the menu
// masks), so the screen is drawn procedurally from the 2D API.  It is safe to
// call any of these at any time, with or without a world, before begin() or
// after end(): draw() simply draws nothing when it is not active.
class Loading {
public:
    // Starts a new stage; `what` is the heading (e.g. "ЗАГРУЗКА МИРА").
    void begin(const std::string& what);
    // The original loading art lives in COMMON/ind16.RES: m16\loadpix.txr
    // (1600x1200 panorama with the logo and "ЗАГРУЗКА ИГРОВОГО МИРА…") blitted
    // centred plus m16\loadrunner.txr (1600x97) revealed left->right as the
    // progress bar.  Loaded lazily on the first draw (the App does not have to
    // pass the data dir); falls back to the procedural screen when absent.
    bool loadArt(Renderer& r, const std::string& dataDir);
    // Progress within the stage.  total <= 0 makes the bar indeterminate (a
    // moving segment), which is what a stage of unknown length should report.
    void step(int done, int total, const std::string& item);
    void draw(Renderer& r, Font& font);
    void end();
    bool active() const { return active_; }

    // ---- extras -----------------------------------------------------------
    void update(float dt);                 // animation clock (optional)
    float progress() const;                // 0..1
    void setNote(const std::string& n) { note_ = n; }   // extra dim line

    // the original's build string, also drawn by the menu
    static const char* versionString() { return "v 8.0 30.01.03"; }

private:
    void bar(Renderer& r, float x, float y, float w, float h,
             float pct, bool indeterminate) const;
    void frameRect(Renderer& r, unsigned tex, float x, float y, float w, float h,
                   float t, float cr, float cg, float cb, float ca) const;

    bool active_ = false;
    std::string what_, item_, note_;
    int done_ = 0, total_ = 1;
    float t_ = 0.0f;
    unsigned bg_ = 0, runner_ = 0;      // original art (0 = procedural fallback)
    bool artTried_ = false;
};

} // namespace ht2

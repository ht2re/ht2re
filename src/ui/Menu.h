#pragma once
#include <string>
#include <vector>
#include "../res/Res.h"

namespace ht2 {

class Renderer;
class Font;

// Start menu: the original MENV/dr.b3d scene as background plus the button
// definitions taken from the original LOAD.RES (mask names) / load.ovl (layers).
//
// The original screen is 800x600; the overlay is laid out on that virtual canvas
// and scaled to the window. Button *art* comes from LOAD.RES masks
// (m\<id>_a/_ap/_p.msk, 125x125 each); until the .msk pixel format is decoded we
// draw placeholder frames so the state machine is fully testable.
class Menu {
public:
    struct Item {
        std::string id;        // "singl"...
        std::string label;     // fallback label (original art has Cyrillic text)
        std::string maskBase;  // "m\\singl"
        int socketId = 0;      // internal button id recovered from king.exe (101..108)
        int x = 0, y = 0;      // top-left on the 800x600 canvas (from king.exe)
        bool available = false;   // mask found in the .res
        unsigned texA = 0, texAP = 0, texP = 0;   // decoded button art (a/ap/p)
    };

    // dataDir = game data root (contains load.ovl and LOAD.RES)
    bool load(const std::string& dataDir);
    // Decode the original button art + the common.msk frame into GL textures.
    // Must be called after Renderer::init().
    void loadArt(Renderer& r);

    void handleKey(int key);          // SDLK_* for keyboard navigation
    void handleMouse(float mx, float my, bool click);
    void update(float dt);
    void setViewport(int w, int h) { vpW_ = w; vpH_ = h; }

    // returns the id of the item activated this frame ("" if none)
    std::string takeActivated();

    void draw(Renderer& r, Font& font);
    // transient status line (used for "not implemented yet" placeholders)
    void setMessage(const std::string& text, float seconds);

    void camera(float& x, float& y, float& z, float& yaw, float& pitch) const;
    const std::vector<Item>& items() const { return items_; }
    int selected() const { return sel_; }
    // virtual canvas size (original UI resolution)
    static const int VW = 800, VH = 600;
    float mouseX() const { return mouseX_; }
    float mouseY() const { return mouseY_; }

private:
    float cellRect(int i, float& x, float& y, float& w, float& h) const;
    int hitTest(float mx, float my) const;

    std::vector<Item> items_;
    Res res_;
    int sel_ = 0;
    float time_ = 0.0f;
    std::string activated_;
    float mouseX_ = -1, mouseY_ = -1;
    std::string message_;
    float messageT_ = 0.0f;
    unsigned frameTex_ = 0;       // common.msk (800x600 truck-front overlay)
    unsigned cursorTex_ = 0;      // cursor.msk (14x16)
    int cursorW_ = 0, cursorH_ = 0;
    int frameW_ = 0, frameH_ = 0;
    bool artLoaded_ = false;
    float scale_ = 1.0f;
    float offX_ = 0.0f, offY_ = 0.0f;
    int vpW_ = 1280, vpH_ = 720;
};

} // namespace ht2

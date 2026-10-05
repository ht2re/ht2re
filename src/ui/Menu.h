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

    struct Page {
        std::string name;
        std::string backdrop;
        std::string overlay;
        int overlayX = 0, overlayY = 0;
        unsigned backdropTex = 0, overlayTex = 0;
        int backdropW = 0, backdropH = 0, overlayW = 0, overlayH = 0;
    };

    // dataDir = game data root (contains load.ovl and LOAD.RES)
    bool load(const std::string& dataDir);
    // Decode the original button art + the common.msk frame into GL textures.
    // Must be called after Renderer::init().
    void loadArt(Renderer& r);

    void handleKey(int key);          // SDLK_* for keyboard navigation
    // Gamepad parity with the ESC panels (docs/original_ui.md 5.3): the pad
    // dirs move the selection, A activates and B plays the "back" role.  The
    // App routes SDL_CONTROLLERBUTTONDOWN here; see tools/app_vehicle_wiring.diff.
    enum PadButton { PadUp = 0, PadDown, PadLeft, PadRight, PadA, PadB, PadStart };
    void handlePad(PadButton b);
    void handleMouse(float mx, float my, bool click);
    void update(float dt);
    void setViewport(int w, int h) { vpW_ = w; vpH_ = h; }

    // returns the id of the item activated this frame ("" if none)
    std::string takeActivated();
    bool activateName(const std::string& id); // same path as keyboard/mouse (CLI tests)

    void draw(Renderer& r, Font& font);
    // transient status line (used for "not implemented yet" placeholders)
    void setMessage(const std::string& text, float seconds);

    // ---- the exit confirmation (original: m\exit -> e\backExitDoit.txr) ----
    // VERIFIED entry point docs/original_ui.md 5.2 ("exit confirm ... e\ok,
    // e\canc"); the port used to quit on the first RETURN.  `exit` is only
    // reported by takeActivated() after the confirm; B/Esc/`canc` cancels.
    bool confirmOpen() const { return confirm_ >= 0; }
    const std::string& confirmText() const { return confirmText_; }
    // the two answers are drawn as yes/no; `sel_` stays on the menu button
    int confirmChoice() const { return confirmChoice_; }   // 0 = yes, 1 = no
    const std::string& page() const { return page_; }
    // the flow table: which screen a button leads to ("" = not implemented)
    static const char* screenFor(const std::string& id);

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
    void activate(int index);
    // True when the page a button opens actually has its backdrop art (pages whose
    // art is not shipped must fall back to the direct action).
    bool pageAvailable(const std::string& id) const;
    void openPage(const std::string& name);
    const Page* currentPage() const;

    std::vector<Item> items_;
    Res res_;
    int sel_ = 0;
    float time_ = 0.0f;
    std::string activated_;
    float mouseX_ = -1, mouseY_ = -1;
    std::string message_;
    float messageT_ = 0.0f;
    int confirm_ = -1;             // index of the item awaiting confirmation
    int confirmChoice_ = 1;        // 1 = the safe default ("no")
    std::string confirmText_;
    std::string page_;               // original LOAD.RES front-end sub-page
    std::vector<Page> pages_;
    unsigned frameTex_ = 0;       // common.msk (800x600 truck-front overlay)
    unsigned cursorTex_ = 0;      // cursor.msk (14x16)
    unsigned exitOkTex_ = 0, exitOkSelTex_ = 0;
    unsigned exitCancelTex_ = 0, exitCancelSelTex_ = 0;
    int cursorW_ = 0, cursorH_ = 0;
    int frameW_ = 0, frameH_ = 0;
    bool artLoaded_ = false;
    float scale_ = 1.0f;
    float offX_ = 0.0f, offY_ = 0.0f;
    int vpW_ = 1280, vpH_ = 720;
};

} // namespace ht2

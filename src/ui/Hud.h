#pragma once
#include <string>
#include <vector>
#include "Font.h"

namespace ht2 {
class Renderer;
class Font;

// The original driving HUD.
//
// Reverse-engineered in docs/original_ui.md: the game blits ONE 1600x50
// chrome/LED strip (`m16\tablo.txr` from COMMON/esc16.RES) at the very top of
// the screen and writes its readouts into the LED windows with two bitmap fonts
// (`matrix.fon` 7x12 labels, `icon.fon` 25x30 numbers).
//
// Layout, verified against the retail reference frames (shots/ref/ref_hud.png,
// ref_map_and_fine.png, ref_hud_helicopter.png, ref_fine_panel_mirror.png):
//
//   [ ПОЗИЦИЯ  ОСТ.ВРЕМЯ ]   [  speed  АККИ  n   ] [fuel] [lamps]
//   [ 2 км 3   25:06     ]   [  82 км/ч 5x100 об/м]
//
// The strip art is drawn 1:1 (not scaled) exactly as the original does, with a
// per-resolution x offset; the 800-pixel reference layout uses art x = -399, so
// the strip's first LED window lands at screen x 91 and its second at x 300 —
// the numbers the reference frames show.  All text is authored in reference
// (800-px) coordinates and mapped through layoutScale()/layoutX().
class Hud {
public:
    // The original's strip art is 1600x50 and is blitted 1:1.
    static constexpr float kArtWidth = 1600.0f;
    static constexpr float kArtHeight = 50.0f;
    // The reference layout width (the retail frames were measured at 800x600).
    static constexpr float kRefWidth = 800.0f;

    struct Values {
        float speedKmh = 0.0f;
        int gear = 0;                 // 0 = N, -1 = R, 1..6
        float rpm = 0.0f;             // engine rpm for the «АККИ» tacho cell
        long money = 0;
        float clockHours = 9.0f;
        float distanceToDest = 0.0f;  // metres, 0 = none
        int level = 0;                // the 0..9 damage/level pair of the original
        float headingDeg = 0.0f;      // destination bearing, for the nav arrow
        const char* destName = "";    // destination town («ПОЗИЦИЯ» second line)
        bool haveDest = false;
        const char* jobTime = "";     // «ОСТ.ВРЕМЯ» value, "MM:SS"; empty = hidden
        bool jobOverdue = false;
        int jobCount = -1;            // icon-cell badge
        std::string message;          // pager line inside the strip
        float messageTimer = 0.0f;
        bool messageNew = false;      // blink the "new message" icon
        float messageAge = 0.0f;
    };

    bool load(Renderer& r, const std::string& dataDir);
    bool loaded() const { return tablo_ != 0; }
    // Height of the strip in pixels; the original art is 50 px tall at 1:1.
    float height(const Renderer& r) const { return kArtHeight; }
    // The 800-px reference layout scale of the current window.
    float layoutScale(const Renderer& r) const;
    // x of the strip art origin and of the first LED window, in screen pixels.
    float layoutX(const Renderer& r) const;
    float windowX(const Renderer& r) const;

    void draw(Renderer& r, Font& font, const Values& v);

    // --- speed camera / fine panel -----------------------------------------
    // The retail data ships the *sound* of the camera (`snd\photo.wav`) and a
    // `txr\flash.txr` flash sprite, but no violation-panel mask: searched all
    // 141 .res containers for the panel/frame the reference frame shows and it
    // is not shipped (the Russian panel is baked into that release's data, the
    // English one in shot/ref_fine_panel_mirror.png is built from the same
    // `mess*`/chrome family).  So the panel is composed here out of the real
    // strip chrome (`m16\tablo.txr`), the real flash sprite and the two HUD
    // fonts, with the reference layout and wording.
    struct Fine {
        int limit = 60;          // «ОГРАНИЧЕНИЕ: NN КМ/Ч»
        int speed = 0;           // «ВАША СКОРОСТЬ: NN КМ/Ч»
        long amount = 500;       // «ШТРАФ $NNN»
        float timer = 0.0f;      // seconds left on screen
        float age = 0.0f;        // seconds since it appeared (flash decay)
    };
    bool hasFineArt() const { return flashTex_ != 0; }
    void drawFine(Renderer& r, const Fine& f);
    void drawMessage(Renderer& r, Font& font, const std::string& text, float alpha,
                     bool isNew, float age);

    // The element table (reference coordinates).  Public so the tests and the
    // map overlay can reason about the same layout.
    struct Cell { const char* name; float x, y; };
    static const Cell* cells(size_t& n);

private:
    unsigned tablo_ = 0;
    unsigned pager_ = 0;      // m16\pager_p.msk (message window)
    unsigned pagerA_ = 0;     // m16\pager_a.msk (new-message icon)
    unsigned pagerTex_ = 0, pagerATex_ = 0;
    unsigned flashTex_ = 0;   // txr\flash.txr (COMMON.RES): the camera flash
    unsigned chromeTop_ = 0, chromeSide_ = 0;   // strips cut out of the tablo art
    // the original HUD fonts (matrix.fon labels 7x12, icon.fon numbers 25x30);
    // their glyphs sit at ASCII + 0x10
    Font* label_ = nullptr;
    Font* big_ = nullptr;
    Font labelFont_, bigFont_;
    std::vector<unsigned> owned_;
    // one machine-readable line per draw (shot_test reads it)
    std::string report_;

public:
    const std::string& report() const { return report_; }
};

} // namespace ht2

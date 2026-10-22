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
// The strip art is drawn 1:1. Readouts are the fields of king.exe 0x169ab0
// (docs/original_ui.md §2.2), placed at text_base + the immediate the call
// pushes. text_base is 469 / -10 / 70 / -10 for the same width bands as the
// strip x. The y pushed into 0x1ea170 is the bottom of the glyph: an icon.fon
// cell is 30 px, and a top at y=43 would hang out of the 50 px strip, while
// the retail numbers sit inside it.
//
//   ПОЗИЦИЯ          ОСТ.ВРЕМЯ                 82 км/ч   АКПП  N
//   <cur> из <total> <MM:SS>                         <rpm>x100 об/м
//
// cur/total are the destination object's city indices (+0x210 and +0x20c,
// each stored 0-based and drawn +1). A port job is one from→to pair, so the
// strip reads 1 из 2 while that order is active and hides the block otherwise
// (the exe skips it when the index is negative).
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
        float rpm = 0.0f;             // engine rpm; the strip shows it in hundreds
        long money = 0;
        float clockHours = 9.0f;      // cab clock; the strip's second timer is separate
        float distanceToDest = 0.0f;  // metres, kept for the map route
        // «ПОЗИЦИЯ»: 1-based city index and city count. Negative hides the block.
        int posCur = -1;
        int posTotal = -1;
        int level = 0;                // the 0..9 damage/level pair of the original
        float headingDeg = 0.0f;      // destination bearing, for the nav arrow
        // Unit components of the destination in the driving view. navRight > 0
        // is the driver's right (screen right after mirrorX); navAhead > 0 is
        // straight ahead. The strip picks one of the 8 icon.fon sectors.
        float navRight = 0.0f;
        float navAhead = 0.0f;
        bool haveNav = false;
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
    // Original 1600x50 PAUSE strip drawn while an ESC page/panel owns input.
    void drawPause(Renderer& r);

    // --- speed camera / fine panel -----------------------------------------
    // king.exe 0x56b650 loads `m16\mess%d.msk`. Id 16 is the speeding panel:
    // the 220×230 mask is blitted at (screenW − 220, 50). The title, the two
    // captions and «ШТРАФ» are in the art. The speeds are `%d` at x = left+137,
    // baselines 142 and 158; the amount is `$%d`, centred, baseline 261.
    struct Fine {
        int limit = 60;          // «ОГРАНИЧЕНИЕ: NN КМ/Ч»
        int speed = 0;           // «ВАША СКОРОСТЬ: NN КМ/Ч»
        long amount = 500;       // «ШТРАФ $NNN»
        float timer = 0.0f;      // seconds left on screen
        float age = 0.0f;        // seconds since it appeared (flash decay)
    };
    bool hasFineArt() const { return fineTex_ != 0; }
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
    unsigned fineTex_ = 0;    // m16\mess16.msk, the speeding panel (220×230)
    unsigned pauseTex_ = 0;   // m16\pause.msk (COMMON/esc16.RES)
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

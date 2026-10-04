#pragma once
#include <string>
#include <cstdint>

namespace ht2 {

class Renderer;

// Text drawing.
//
// Priority order:
//   1. loadFon()  — an original .FON bitmap font (see docs/fon_format.md):
//      a 16x16 grid of character cells (code = row*16 + column, cp1251), read
//      as an 8bpp mask (plain container) or 16bpp mask (LOFF/TXR container);
//   2. initBuiltin() — a small built-in stroke font (5x7 grid) so the UI is
//      usable even when no .FON can be loaded.
class Font {
public:
    struct Glyph { int x = 0, y = 0, w = 0, h = 0, advance = 0; };

    bool initBuiltin(Renderer& r);
    // Decodes the original font and uploads the glyph atlas as a texture.
    bool loadFon(Renderer& r, const std::string& path);
    // .SFT variant: 94 Latin glyphs (codes 0x21..0x7E), 1bpp, one byte per row.
    bool loadSft(Renderer& r, const std::string& path);
    // tries <dataDir>/MENU.SFT first (the original's preference), then MENU.FON
    bool loadOriginal(Renderer& r, const std::string& dataDir);
    bool ready() const { return builtin || tex != 0; }
    bool isBitmap() const { return !builtin && tex != 0; }

    float lineHeight(float scale) const {
        return (builtin ? 7.0f : (float)cellH_) * scale * 1.25f;
    }
    float textWidth(const std::string& s, float scale) const;
    // x,y = top-left of the first glyph cell (pixel coordinates)
    void draw(Renderer& r, const std::string& s, float x, float y, float scale,
              float cr, float cg, float cb, float ca) const;
    void drawCentered(Renderer& r, const std::string& s, float cx, float y, float scale,
                      float cr, float cg, float cb, float ca) const;

    unsigned texture() const { return tex; }
    int atlasW() const { return atlasW_; }
    int atlasH() const { return atlasH_; }
    int cellW() const { return cellW_; }
    int cellH() const { return cellH_; }
    int glyphH() const { return glyphH_; }
    const Glyph* glyph(char c) const;
    // The in-game HUD fonts (matrix.fon / icon.fon) place their glyphs at a
    // different code page position than ASCII: digits live at 0x40..0x49, i.e.
    // ASCII + 0x10 (docs/original_ui.md 2.2).  The offset is added when the
    // glyph is looked up.
    void setCodeOffset(int off) { codeOffset_ = off; }
    // The game's own strings (script town/cargo names, .way node names) are raw
    // cp1251 bytes; the sources' literals are UTF-8.  This converts either form
    // to UTF-8, so callers can build one string from both (HUD text, messages).
    static std::string toUtf8Text(const std::string& in);
    // The cp1251 byte string Font::draw()/textWidth() hand to the glyph lookup
    // (code + codeOffset()).  Exposed so the regression test can verify the
    // conversion of a script message without decoding the rendered pixels.
    static std::string glyphCodes(const std::string& in);
    int codeOffset() const { return codeOffset_; }
    void unload();

private:
    bool builtin = true;
    unsigned tex = 0;
    int atlasW_ = 0, atlasH_ = 0;
    int codeOffset_ = 0;
    int cellW_ = 6, cellH_ = 9, glyphH_ = 8;
    Glyph glyphs[256];
};

} // namespace ht2

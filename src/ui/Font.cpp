#include "Font.h"
#include "../gfx/Renderer.h"
#include "../util/Log.h"
#include <cctype>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace ht2 {

// --- built-in stroke font ---------------------------------------------------
// Glyphs live on a 5 (x: 0..4) x 7 (y: 0..6) grid, y pointing down.
struct Seg { uint8_t x0, y0, x1, y1; };
struct GlyphDef { const Seg* segs; uint8_t n; };

#define G(name, ...)                                              \
    static const Seg name##_s[] = {__VA_ARGS__};                   \
    static const GlyphDef name = {name##_s, (uint8_t)(sizeof(name##_s) / sizeof(Seg))};

G(gA, {0,6,0,2}, {0,2,2,0}, {2,0,4,2}, {4,2,4,6}, {0,4,4,4})
G(gB, {0,0,0,6}, {0,0,3,0}, {3,0,4,1}, {4,1,4,2}, {4,2,3,3}, {3,3,0,3},
      {3,3,4,4}, {4,4,4,5}, {4,5,3,6}, {3,6,0,6})
G(gC, {4,1,3,0}, {3,0,1,0}, {1,0,0,1}, {0,1,0,5}, {0,5,1,6}, {1,6,3,6}, {3,6,4,5})
G(gD, {0,0,0,6}, {0,0,2,0}, {2,0,4,2}, {4,2,4,4}, {4,4,2,6}, {2,6,0,6})
G(gE, {4,0,0,0}, {0,0,0,6}, {0,6,4,6}, {0,3,3,3})
G(gF, {4,0,0,0}, {0,0,0,6}, {0,3,3,3})
G(gG, {4,1,3,0}, {3,0,1,0}, {1,0,0,1}, {0,1,0,5}, {0,5,1,6}, {1,6,3,6}, {3,6,4,5},
      {4,5,4,3}, {4,3,2,3})
G(gH, {0,0,0,6}, {4,0,4,6}, {0,3,4,3})
G(gI, {1,0,3,0}, {2,0,2,6}, {1,6,3,6})
G(gJ, {2,0,2,5}, {2,5,1,6}, {1,6,0,5})
G(gK, {0,0,0,6}, {4,0,0,3}, {0,3,4,6})
G(gL, {0,0,0,6}, {0,6,4,6})
G(gM, {0,6,0,0}, {0,0,2,2}, {2,2,4,0}, {4,0,4,6})
G(gN, {0,6,0,0}, {0,0,4,6}, {4,6,4,0})
G(gO, {1,0,3,0}, {3,0,4,1}, {4,1,4,5}, {4,5,3,6}, {3,6,1,6}, {1,6,0,5}, {0,5,0,1}, {0,1,1,0})
G(gP, {0,6,0,0}, {0,0,3,0}, {3,0,4,1}, {4,1,4,2}, {4,2,3,3}, {3,3,0,3})
G(gQ, {1,0,3,0}, {3,0,4,1}, {4,1,4,5}, {4,5,3,6}, {3,6,1,6}, {1,6,0,5}, {0,5,0,1},
      {0,1,1,0}, {2,4,4,6})
G(gR, {0,6,0,0}, {0,0,3,0}, {3,0,4,1}, {4,1,4,2}, {4,2,3,3}, {3,3,0,3}, {2,3,4,6})
G(gS, {4,1,3,0}, {3,0,1,0}, {1,0,0,1}, {0,1,0,2}, {0,2,1,3}, {1,3,3,3}, {3,3,4,4},
      {4,4,4,5}, {4,5,3,6}, {3,6,1,6}, {1,6,0,5})
G(gT, {0,0,4,0}, {2,0,2,6})
G(gU, {0,0,0,5}, {0,5,1,6}, {1,6,3,6}, {3,6,4,5}, {4,5,4,0})
G(gV, {0,0,2,6}, {2,6,4,0})
G(gW, {0,0,0,6}, {0,6,2,4}, {2,4,4,6}, {4,6,4,0})
G(gX, {0,0,4,6}, {4,0,0,6})
G(gY, {0,0,2,3}, {4,0,2,3}, {2,3,2,6})
G(gZ, {0,0,4,0}, {4,0,0,6}, {0,6,4,6})
G(g0, {1,0,3,0}, {3,0,4,1}, {4,1,4,5}, {4,5,3,6}, {3,6,1,6}, {1,6,0,5}, {0,5,0,1}, {0,1,1,0})
G(g1, {1,1,2,0}, {2,0,2,6}, {1,6,3,6})
G(g2, {0,1,1,0}, {1,0,3,0}, {3,0,4,1}, {4,1,4,2}, {4,2,0,6}, {0,6,4,6})
G(g3, {0,0,4,0}, {4,0,2,3}, {2,3,3,3}, {3,3,4,4}, {4,4,4,5}, {4,5,3,6}, {3,6,1,6}, {1,6,0,5})
G(g4, {3,6,3,0}, {3,0,0,4}, {0,4,4,4})
G(g5, {4,0,0,0}, {0,0,0,3}, {0,3,3,3}, {3,3,4,4}, {4,4,4,5}, {4,5,3,6}, {3,6,1,6}, {1,6,0,5})
G(g6, {4,1,3,0}, {3,0,1,0}, {1,0,0,1}, {0,1,0,5}, {0,5,1,6}, {1,6,3,6}, {3,6,4,5},
      {4,5,4,4}, {4,4,3,3}, {3,3,0,3})
G(g7, {0,0,4,0}, {4,0,2,6})
G(g8, {1,0,3,0}, {3,0,4,1}, {4,1,4,2}, {4,2,3,3}, {3,3,1,3}, {1,3,0,2}, {0,2,0,1}, {0,1,1,0},
      {1,3,0,4}, {0,4,0,5}, {0,5,1,6}, {1,6,3,6}, {3,6,4,5}, {4,5,4,4}, {4,4,3,3})
G(g9, {0,5,1,6}, {1,6,3,6}, {3,6,4,5}, {4,5,4,1}, {4,1,3,0}, {3,0,1,0}, {1,0,0,1},
      {0,1,0,2}, {0,2,1,3}, {1,3,4,3})
G(gDot,  {1,5,3,5}, {3,5,3,6}, {3,6,1,6}, {1,6,1,5})
G(gComma,{2,5,2,7})
G(gDash, {1,3,3,3})
G(gUs,   {0,6,4,6})
G(gCol,  {2,1,2,2}, {2,4,2,5})
G(gSlash,{4,0,0,6})
G(gBSlash,{0,0,4,6})
G(gLPar, {3,0,1,2}, {1,2,1,4}, {1,4,3,6})
G(gRPar, {1,0,3,2}, {3,2,3,4}, {3,4,1,6})
G(gGt,   {1,1,3,3}, {3,3,1,5})
G(gLt,   {3,1,1,3}, {1,3,3,5})
G(gPlus, {1,3,3,3}, {2,2,2,4})
G(gEq,   {1,2,3,2}, {1,4,3,4})
G(gStar, {2,1,2,5}, {1,2,3,4}, {3,2,1,4})
G(gBang, {2,0,2,4}, {1,6,3,6})
G(gQuest,{0,1,1,0}, {1,0,3,0}, {3,0,4,1}, {4,1,4,2}, {4,2,2,3}, {2,3,2,4}, {1,6,3,6})
G(gBox,  {0,0,4,0}, {4,0,4,6}, {4,6,0,6}, {0,6,0,0})
G(gEmpty,{0,0,0,0})
#undef G

static const GlyphDef* glyphFor(unsigned char c) {
    static const GlyphDef* tab[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) tab[i] = &gBox;
        const char* up = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        const GlyphDef* ups[] = {&gA,&gB,&gC,&gD,&gE,&gF,&gG,&gH,&gI,&gJ,&gK,&gL,&gM,
                                 &gN,&gO,&gP,&gQ,&gR,&gS,&gT,&gU,&gV,&gW,&gX,&gY,&gZ};
        for (int i = 0; i < 26; ++i) { tab[(unsigned char)up[i]] = ups[i]; tab[(unsigned char)tolower(up[i])] = ups[i]; }
        tab['0']=&g0; tab['1']=&g1; tab['2']=&g2; tab['3']=&g3; tab['4']=&g4;
        tab['5']=&g5; tab['6']=&g6; tab['7']=&g7; tab['8']=&g8; tab['9']=&g9;
        tab[' '] = &gEmpty;
        tab['.'] = &gDot;  tab[','] = &gComma; tab['-'] = &gDash; tab['_'] = &gUs;
        tab[':'] = &gCol;  tab['/'] = &gSlash; tab['\\'] = &gBSlash;
        tab['('] = &gLPar; tab[')'] = &gRPar;  tab['>'] = &gGt;   tab['<'] = &gLt;
        tab['+'] = &gPlus; tab['='] = &gEq;    tab['*'] = &gStar;
        tab['!'] = &gBang; tab['?'] = &gQuest;
        init = true;
    }
    return tab[c];
}

// UTF-8 -> cp1251 for the glyph codes of the original bitmap font (the game's
// code page is cp1251, so Cyrillic needs the 0xC0..0xFF block).
// The two string families the blitter must accept:
//   * the C++ literals in the sources and the HUD element names: valid UTF-8;
//   * the ORIGINAL data (script town/cargo names, .way node names read as raw
//     bytes, docs/script_gameplay.md): cp1251.
// They are told apart per byte: an ASCII byte is itself, and only the pair
// (0xD0|0xD1, 0x80..0xBF) is a UTF-8 Cyrillic letter (cp1251 letter bytes are
// >= 0xC0, so a genuine cp1251 pair can never look like that).  High bytes that
// are not part of such a pair are the original's cp1251 letters.
static unsigned decodeCp1251(unsigned char c) {
    if (c < 0x80) return c;
    if (c >= 0xC0) return 0x0410 + (c - 0xC0);      // А..я
    if (c == 0xA8) return 0x0401;                   // Ё
    if (c == 0xB8) return 0x0451;                   // ё
    if (c == 0xB9) return 0x2116;                   // №
    return c;                                       // punctuation as-is
}

static void appendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) out.push_back((char)cp);
    else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

// Any byte string -> UTF-8 (mixed cp1251 / UTF-8 aware).
static std::string toUtf8(const std::string& in) {
    std::string out;
    out.reserve(in.size() * 2);
    for (size_t i = 0; i < in.size();) {
        const unsigned char c = (unsigned char)in[i];
        if (c < 0x80) { out.push_back((char)c); ++i; continue; }
        if ((c == 0xD0 || c == 0xD1) && i + 1 < in.size() &&
            ((unsigned char)in[i + 1] & 0xC0) == 0x80) {
            appendUtf8(out, ((unsigned)(c & 0x1F) << 6) |
                            (unsigned)((unsigned char)in[i + 1] & 0x3F));
            i += 2;
            continue;
        }
        appendUtf8(out, decodeCp1251(c));      // the original's encoding
        ++i;
    }
    return out;
}

std::string Font::toUtf8Text(const std::string& in) { return toUtf8(in); }

// UTF-8 (or raw cp1251) -> the cp1251 byte string the .FON atlas is indexed by.
static std::string toCp1251(const std::string& in) {
    const std::string u = toUtf8(in);
    std::string out;
    out.reserve(u.size());
    for (size_t i = 0; i < u.size();) {
        const unsigned char c = (unsigned char)u[i];
        if (c < 0x80) { out.push_back((char)c); ++i; continue; }
        uint32_t cp = 0;
        int extra = 0;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { out.push_back('?'); ++i; continue; }
        if (i + (size_t)extra >= u.size()) { out.push_back('?'); break; }
        for (int k = 1; k <= extra; ++k)
            cp = (cp << 6) | ((unsigned char)u[i + k] & 0x3F);
        i += (size_t)extra + 1;
        if (cp >= 0x0410 && cp <= 0x044F) out.push_back((char)(cp - 0x0410 + 0xC0));
        else if (cp == 0x0401) out.push_back((char)0xA8);
        else if (cp == 0x0451) out.push_back((char)0xB8);
        else if (cp == 0x2116) out.push_back((char)0xB9);   // №
        else if (cp == 0x2014 || cp == 0x2013) out.push_back((char)0x97);
        else if (cp == 0x2018 || cp == 0x2019) out.push_back((char)0x92);
        else if (cp == 0x201C || cp == 0x201D) out.push_back((char)0x93);
        else if (cp == 0x00AB) out.push_back((char)0xAB);
        else if (cp == 0x00BB) out.push_back((char)0xBB);
        else out.push_back('?');
    }
    return out;
}

std::string Font::glyphCodes(const std::string& in) { return toCp1251(in); }

// ---------------------------------------------------------------------------
// the built-in stroke font (fallback when no original font is available)
// ---------------------------------------------------------------------------
const GlyphDef* builtinGlyph(unsigned char c) {
    static bool init = false;
    if (!init) { glyphFor('A'); init = true; }        // builds the table lazily
    return glyphFor(c);
}

bool Font::initBuiltin(Renderer&) {
    builtin = true;
    tex = 0;
    cellW_ = 6;
    cellH_ = 9;
    glyphH_ = 8;
    atlasW_ = atlasH_ = 0;
    codeOffset_ = 0;
    for (int i = 0; i < 256; ++i) glyphs[i] = Glyph();
    return true;
}

bool Font::loadOriginal(Renderer& r, const std::string& dataDir) {
    // The original prefers MENU.SFT (a 94-glyph Latin set), then MENU.FON.
    if (loadSft(r, dataDir + "/MENU.SFT")) return true;
    if (loadFon(r, dataDir + "/MENU.FON")) return true;
    return initBuiltin(r);
}

// .SFT: 94 glyphs (codes 0x21..0x7E), 1 bpp, one byte per row per glyph.
bool Font::loadSft(Renderer& r, const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 16) { fclose(f); return false; }
    std::vector<uint8_t> d((size_t)n);
    size_t got = fread(d.data(), 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) return false;
    // header: u16 width, u16 height, u16 first code, then the glyph rows
    const int w = (int)(d[0] | (d[1] << 8));
    const int h = (int)(d[2] | (d[3] << 8));
    const int first = (int)(d[4] | (d[5] << 8));
    if (w <= 0 || h <= 0 || w > 32 || h > 32) return false;
    const size_t rowBytes = (size_t)((w + 7) / 8);
    const size_t glyphBytes = rowBytes * (size_t)h;
    if (6 + glyphBytes * 94 > (size_t)n) return false;
    cellW_ = w;
    cellH_ = h + 1;
    glyphH_ = h;
    atlasW_ = w * 16;
    atlasH_ = glyphH_ * 16;
    std::vector<uint8_t> rgba((size_t)atlasW_ * atlasH_ * 4, 0);
    for (int c = 0; c < 94; ++c) {
        const int code = first + c;
        if (code < 0 || code > 255) continue;
        const uint8_t* g = d.data() + 6 + glyphBytes * (size_t)c;
        const int col = (code & 15) * w, row = (code >> 4) * glyphH_;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (g[(size_t)y * rowBytes + (size_t)(x / 8)] & (0x80 >> (x & 7))) {
                    const size_t o = ((size_t)(row + y) * atlasW_ + col + x) * 4;
                    rgba[o + 0] = rgba[o + 1] = rgba[o + 2] = 255;
                    rgba[o + 3] = 255;
                }
    }
    tex = r.createTexture(rgba.data(), atlasW_, atlasH_);
    if (!tex) return false;
    builtin = false;
    for (int c = 0; c < 256; ++c) {
        Glyph& g = glyphs[c];
        g.x = (c & 15) * cellW_;
        g.y = (c >> 4) * glyphH_;
        g.w = cellW_;
        g.h = glyphH_;
        g.advance = cellW_;
    }
    LOG("font: %s SFT %dx%d atlas, cell %dx%d, glyph %dx%d", path.c_str(), atlasW_,
        atlasH_, cellW_, cellH_, cellW_, glyphH_);
    return true;
}

bool Font::loadFon(Renderer& r, const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { LOG("font: cannot open %s", path.c_str()); return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0x20) { fclose(f); return false; }
    std::vector<uint8_t> d((size_t)n);
    size_t got = fread(d.data(), 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) return false;

    auto rd16 = [&](size_t p) { return (int)(d[p] | (d[p + 1] << 8)); };
    int w = rd16(0x0C), h = rd16(0x0E);
    int bits = d[0x10], flag = d[0x11];
    if (w <= 0 || h <= 0 || w % 16 || h % 16) {
        LOG("font: %s bad atlas size %dx%d", path.c_str(), w, h);
        return false;
    }
    bool loff = (memcmp(d.data() + 0x12, "LOFF", 4) == 0);
    size_t pixOff;
    if (loff) {
        if (bits != 16) { LOG("font: %s LOFF but %d bpp", path.c_str(), bits); return false; }
        pixOff = 30;
    } else {
        if (bits != 8) { LOG("font: %s plain but %d bpp", path.c_str(), bits); return false; }
        pixOff = (size_t)n - (size_t)w * h;
    }
    int bpp = bits / 8;
    if (pixOff + (size_t)w * h * bpp > (size_t)n) {
        LOG("font: %s pixel block truncated", path.c_str());
        return false;
    }
    bool topDown = (flag & 0x20) != 0;
    cellW_ = w / 16;
    cellH_ = h / 16;
    glyphH_ = cellH_ - 1;
    atlasW_ = cellW_ * 16;
    atlasH_ = glyphH_ * 16;

    std::vector<uint8_t> rgba((size_t)atlasW_ * atlasH_ * 4, 0);
    for (int c = 0; c < 256; ++c) {
        int strip = c >> 4, col = c & 15;
        int sx0 = col * cellW_, sy0 = strip * cellH_ + 1;
        for (int ry = 0; ry < glyphH_; ++ry) {
            int sy = sy0 + ry;
            int srcy = topDown ? sy : (h - 1 - sy);
            for (int rx = 0; rx < cellW_; ++rx) {
                int sx = sx0 + rx;
                size_t off = pixOff + ((size_t)srcy * w + sx) * bpp;
                uint8_t v = bpp == 1 ? d[off] : (uint8_t)(d[off] | (d[off + 1] << 8));
                size_t o = ((size_t)(strip * glyphH_ + ry) * atlasW_ + (col * cellW_ + rx)) * 4;
                rgba[o + 0] = 255; rgba[o + 1] = 255; rgba[o + 2] = 255;
                rgba[o + 3] = v ? 255 : 0;
            }
        }
    }
    tex = r.createTexture(rgba.data(), atlasW_, atlasH_);
    if (!tex) { LOG("font: %s texture upload failed", path.c_str()); return false; }
    builtin = false;
    for (int c = 0; c < 256; ++c) {
        Glyph& g = glyphs[c];
        g.x = (c & 15) * cellW_;
        g.y = (c >> 4) * glyphH_;
        g.w = cellW_;
        g.h = glyphH_;
        g.advance = cellW_;
        // icon.fon (25px cells). king.exe 0x1ea0e0 sums the per-glyph width
        // scanned from row 0 of the cell (the solid bar above the digit, not
        // drawn). That bar is 11px for the HUD digits and 9px for the colon;
        // advancing the whole 25px cell is what shoved «33:03» into the next
        // field. matrix.fon stays on the cell pitch: its row 0 is a full-width
        // marker, and the labels are authored for that pitch.
        if (cellW_ >= 16) {
            const int strip = c >> 4, col = c & 15;
            const int sy = strip * cellH_;
            const int srcy = topDown ? sy : (h - 1 - sy);
            int right = -1;
            for (int rx = 0; rx < cellW_; ++rx) {
                const size_t off = pixOff + ((size_t)srcy * w + (col * cellW_ + rx)) * bpp;
                const uint8_t v = bpp == 1 ? d[off]
                    : (uint8_t)(d[off] | (d[off + 1] << 8));
                if (v) right = rx;
            }
            if (right >= 0) g.advance = right + 1;
        }
    }
    LOG("font: %s %s %dx%d atlas, cell %dx%d, glyph %dx%d", path.c_str(),
        loff ? "LOFF/16bpp" : "plain/8bpp", w, h, cellW_, cellH_, cellW_, glyphH_);
    return true;
}

const Font::Glyph* Font::glyph(char c) const {
    const unsigned char u = (unsigned char)c;
    if (builtin) return nullptr;
    const int idx = (int)u + codeOffset_;
    if (idx < 0 || idx > 255) return nullptr;
    return &glyphs[idx];
}

void Font::unload() {
    // The GL id belongs to the Renderer and is released with it; dropping the
    // reference keeps this a safe no-op after Renderer::shutdown().
    tex = 0;
    builtin = true;
    atlasW_ = atlasH_ = 0;
    for (int i = 0; i < 256; ++i) glyphs[i] = Glyph();
}

float Font::textWidth(const std::string& s, float scale) const {
    if (builtin) return 6.0f * scale * (float)s.size();
    const std::string cs = toCp1251(s);
    float w = 0;
    for (unsigned char c : cs) {
        int idx = (int)c + codeOffset_;
        if (idx < 0 || idx > 255) { w += cellW_ * scale; continue; }
        const Glyph& g = glyphs[idx];
        w += (g.advance ? g.advance : cellW_) * scale;
    }
    return w;
}

void Font::draw(Renderer& r, const std::string& s, float x, float y, float scale,
                float cr, float cg, float cb, float ca) const {
    if (builtin) {
        float u = scale;
        float thick = scale < 1.5f ? 1.0f : scale * 0.9f;
        float cx = x;
        for (unsigned char c : s) {
            const GlyphDef* g = glyphFor(c);
            if (g) {
                for (uint8_t i = 0; i < g->n; ++i) {
                    const Seg& sg = g->segs[i];
                    if (sg.x0 == sg.x1 && sg.y0 == sg.y1) continue;
                    r.line2D(r.whiteTex(),
                             cx + (sg.x0 + 0.5f) * u, y + (sg.y0 + 0.5f) * u,
                             cx + (sg.x1 + 0.5f) * u, y + (sg.y1 + 0.5f) * u,
                             thick, cr, cg, cb, ca);
                }
            }
            cx += 6.0f * u;
        }
        return;
    }
    const std::string cs = toCp1251(s);
    float cx = x;
    for (unsigned char c : cs) {
        int idx = (int)c + codeOffset_;
        if (idx < 0 || idx > 255) { cx += cellW_ * scale; continue; }
        const Glyph& g = glyphs[idx];
        if (g.w > 0 && g.h > 0) {
            float u0 = (float)g.x / (float)atlasW_;
            float v0 = (float)g.y / (float)atlasH_;
            float u1 = (float)(g.x + g.w) / (float)atlasW_;
            float v1 = (float)(g.y + g.h) / (float)atlasH_;
            r.quad2D(tex, cx, y, g.w * scale, g.h * scale, cr, cg, cb, ca, u0, v0, u1, v1);
        }
        cx += (g.advance ? g.advance : 6) * scale;
    }
}

void Font::drawCentered(Renderer& r, const std::string& s, float cx, float y, float scale,
                        float cr, float cg, float cb, float ca) const {
    draw(r, s, cx - textWidth(s, scale) * 0.5f, y, scale, cr, cg, cb, ca);
}

} // namespace ht2

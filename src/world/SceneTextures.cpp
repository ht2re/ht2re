#include "SceneTextures.h"
#include "../res/Res.h"
#include "../res/Mask.h"
#include "../gfx/Renderer.h"
#include "../util/Log.h"
#include <cctype>
#include <cstring>

namespace ht2 {

static std::string normalize(const std::string& s) {
    std::string out;
    // keep only the file stem, lower case, drop underscores
    size_t slash = s.find_last_of("\\/");
    std::string t = slash == std::string::npos ? s : s.substr(slash + 1);
    size_t dot = t.rfind('.');
    if (dot != std::string::npos && dot > 0) t = t.substr(0, dot);
    for (char c : t) if (c != '_') out += (char)tolower((unsigned char)c);
    return out;
}

#include <array>
#include <cstdlib>

// RGB triples of the shared palette (common.plm).  The colour key of a keyed
// material is PALT[col-1] truncated to RGB565.
static std::array<uint8_t, 768> g_palette{};
static bool g_paletteLoaded = false;

void SceneTextures::loadPalette(const std::string& commonResPath) {
    if (g_paletteLoaded) return;
    Res res;
    if (!res.load(commonResPath)) { LOG("palette: cannot open %s", commonResPath.c_str()); return; }
    for (const auto& e : res.entries()) {
        if (e.kind != Res::Kind::Palette) continue;
        const uint8_t* p = res.data(e);
        size_t n = e.size;
        for (size_t i = 0; i + 8 <= n; ++i)
            if (!memcmp(p + i, "PALT", 4)) {
                // "PALT" u32 0x300 then 256 RGB triples
                if (i + 8 + 768 <= n) {
                    memcpy(g_palette.data(), p + i + 8, 768);
                    g_paletteLoaded = true;
                    LOG("palette: %s: %zu bytes", e.name.c_str(), n);
                    return;
                }
            }
    }
}

static int rgbTo565(int r, int g, int b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// The MATERIALS section is a count followed by one NUL-terminated line per
// material: "<name> ttx <N> col <C> ..." - `ttx N` (1-based) marks texture N of
// this .res as colour-keyed, `col C` selects palette entry C-1 (default 1 =
// black).  Textures with no `ttx` material stay opaque.
static void parseMaterialKeys(const std::vector<uint8_t>& raw, std::vector<int>& keyOfTex,
                              std::vector<char>& noTile) {
    static const char* kTag = "MATERIALS ";
    size_t at = std::string::npos;
    for (size_t i = 0; i + 10 <= raw.size(); ++i)
        if (!memcmp(raw.data() + i, kTag, 10)) { at = i; break; }
    if (at == std::string::npos) return;
    long count = strtol((const char*)raw.data() + at + 10, nullptr, 10);
    size_t p = at + 10;
    while (p < raw.size() && raw[p] != 0) ++p;      // end of the count token
    if (p >= raw.size()) return;
    ++p;
    for (long i = 0; i < count && p < raw.size(); ++i) {
        size_t end = p;
        while (end < raw.size() && raw[end] != 0) ++end;
        std::string line((const char*)raw.data() + p, end - p);
        p = end + 1;
        int ttx = -1, col = 1;
        bool notile = false;
        for (size_t k = 0; k < line.size();) {
            while (k < line.size() && line[k] == ' ') ++k;
            size_t s0 = k;
            while (k < line.size() && line[k] != ' ') ++k;
            std::string tok = line.substr(s0, k - s0);
            if (tok == "notile") { notile = true; continue; }
            if (tok == "ttx" || tok == "col") {
                while (k < line.size() && line[k] == ' ') ++k;
                size_t v0 = k;
                while (k < line.size() && line[k] != ' ') ++k;
                int v = atoi(line.substr(v0, k - v0).c_str());
                if (tok == "ttx") ttx = v;
                else col = v;
            }
        }
        if (ttx <= 0) continue;
        size_t idx = (size_t)(ttx - 1);
        if (idx >= keyOfTex.size()) keyOfTex.resize(idx + 1, -1);
        if (notile) {
            if (idx >= noTile.size()) noTile.resize(idx + 1, 0);
            noTile[idx] = 1;
        }
        int c = col - 1;
        if (c < 0 || c > 255) c = 0;
        keyOfTex[idx] = rgbTo565(g_palette[(size_t)c * 3], g_palette[(size_t)c * 3 + 1],
                                 g_palette[(size_t)c * 3 + 2]);
    }
}

std::string SceneTextures::tileKey(const std::string& resBase) { return normalize(resBase); }

bool SceneTextures::load(Renderer& r, const std::string& resBase, bool skipNoLoad) {
    base_ = resBase;
    white_ = r.whiteTex();
    const std::string tile = normalize(resBase);
    const char* suffix[3] = {"", "1", "2"};
    for (int k = 0; k < 3; ++k) {
        Res res;
        std::string path = resBase + suffix[k] + ".res";
        if (!res.load(path)) continue;
        std::vector<int> keyOfTex;
        std::vector<char> noTile;
        parseMaterialKeys(res.bytes(), keyOfTex, noTile);
        size_t texOrdinal = 0;
        for (auto& e : res.entries()) {
            if (e.kind != Res::Kind::Texture) continue;
            if (skipNoLoad && e.flags.find("noload") != std::string::npos) continue;
            int key565 = texOrdinal < keyOfTex.size() ? keyOfTex[texOrdinal] : -1;
            const bool clamp = texOrdinal < noTile.size() && noTile[texOrdinal];
            ++texOrdinal;
            std::string key = normalize(e.name);
            // king.exe looks the texture up in this .res.  A shared stem
            // (Zgas.txr in ac.res and eurotxr/Zgas.txr in ce.res) is a
            // different image; the first file must not hide the later one.
            const std::string qual = tile + "/" + key;
            if (map_.count(key) && map_.count(qual)) continue;
            DecodedImage img;
            if (!Texture::decode(res.data(e), e.size, img, key565)) {
                LOG("tex: decode failed %s", e.name.c_str());
                continue;
            }
            if (img.format.find("colour-keyed") != std::string::npos) {
                ++keyed_;
                if (getenv("HT2_TEX_DEBUG"))
                    LOG("tex: keyed %-28s %s", e.name.c_str(), img.format.c_str());
            }
            unsigned id = r.createTexture(img.rgba.data(), img.width, img.height, clamp);
            if (!id) continue;
            if (!map_.count(qual)) map_[qual] = id;
            if (!map_.count(key)) map_[key] = id;   // first file wins for unqualified lookups
            tex_.push_back(id);
            ++decoded_;
        }
    }
    LOG("tex: %s: %zu textures decoded from %s[1,2].res, %zu colour-keyed",
        resBase.c_str(), decoded_, resBase.c_str(), keyed_);
    // Filtering contract for everything that is drawn in 3D (road/props/ground):
    // a complete glGenerateMipmap chain, GL_LINEAR_MIPMAP_LINEAR, GL_REPEAT and
    // GL_TEXTURE_MAX_ANISOTROPY_EXT = min(8, driver max).  The counters are
    // filled by Renderer::createTexture() and cover every texture of the process
    // created up to this point (so they are cumulative).
    LOG("tex: %s: anisotropy %.0fx (driver max %.0fx) - cumulative %s",
        resBase.c_str(), r.anisotropy(), r.maxAnisotropy(),
        r.textureStats().line().c_str());
    return !map_.empty();
}

unsigned SceneTextures::textureFor(const std::string& material, const std::string& tile) const {
    if (material.empty() || map_.empty()) return 0;
    std::string key = normalize(material);
    // material names carry a "mat_" prefix that texture names drop
    if (key.compare(0, 3, "mat") == 0) key = key.substr(3);
    if (!tile.empty()) {
        auto q = map_.find(tile + "/" + key);
        if (q != map_.end()) return q->second;
    }
    auto it = map_.find(key);
    if (it != map_.end()) return it->second;
    // aliases seen in the shipped data (mat_red_d -> rd_d, ...)
    static const struct { const char* from; const char* to; } kAlias[] = {
        {"redd", "rdd"}, {"redb", "rdb"}, {"gred", "grd"}, {"greb", "grb"},
        {"reflroad", "reflr"}, {"graviroad", "gravii"},
    };
    for (auto& a : kAlias) if (key == a.from) { auto j = map_.find(a.to); if (j != map_.end()) return j->second; }
    auto lookup = [&](const std::string& name) -> unsigned {
        if (!tile.empty()) {
            auto q = map_.find(tile + "/" + name);
            if (q != map_.end()) return q->second;
        }
        auto j = map_.find(name);
        return j == map_.end() ? 0u : j->second;
    };
    // Direction boards. mat_poster_tile / mat_poster_ugol are the frame strips
    // (post_tile.txr, post_ugol.txr). mat_post_<town> is the inscription face
    // (town_<town>.txr). The 4-character prefix below bound every post* name to
    // whichever of those textures hashed first, so the frame wore the letters.
    if (key == "postertile") {
        if (unsigned t = lookup("posttile")) return t;
    } else if (key == "posterugol") {
        if (unsigned t = lookup("postugol")) return t;
    } else if (key.size() > 4 && key.compare(0, 4, "post") == 0 &&
               key.compare(0, 6, "poster") != 0) {
        if (unsigned t = lookup("town" + key.substr(4))) return t;
    }
    const bool signFamily = key.compare(0, 4, "post") == 0 || key.compare(0, 4, "town") == 0 ||
                            key.compare(0, 4, "back") == 0 || key.compare(0, 3, "zno") == 0 ||
                            key.compare(0, 4, "zone") == 0 || key.compare(0, 4, "zend") == 0;
    if (signFamily) return 0;
    // prefix fallback (e.g. house7 -> house7 absent, use house6)
    for (auto& kv : map_)
        if (kv.first.size() >= 4 && key.size() >= 4 &&
            (kv.first.compare(0, 4, key, 0, 4) == 0))
            return kv.second;
    return 0;
}

} // namespace ht2

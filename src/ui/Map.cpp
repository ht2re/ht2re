#include "Map.h"
#include "../gfx/Renderer.h"
#include "../res/Res.h"
#include "../res/Mask.h"
#include "../world/SceneTextures.h"
#include "../script/GameData.h"
#include "../game/Economy.h"
#include "../util/Log.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <array>

namespace ht2 {

// Liang-Barsky style clip of the segment (x0,y0)-(x1,y1) against the rectangle.
// Returns false when it lies fully outside.  The renderer has no scissor state
// in this path, so the map has to keep its roads inside the panel itself.
static bool clipSegment(float& x0, float& y0, float& x1, float& y1,
                        float rx0, float ry0, float rx1, float ry1) {
    float dx = x1 - x0, dy = y1 - y0;
    float t0 = 0.0f, t1 = 1.0f;
    const float p[4] = {-dx, dx, -dy, dy};
    const float q[4] = {x0 - rx0, rx1 - x0, y0 - ry0, ry1 - y0};
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0.0f) {
            if (q[i] < 0.0f) return false;         // parallel and outside
            continue;
        }
        float t = q[i] / p[i];
        if (p[i] < 0.0f) { if (t > t1) return false; if (t > t0) t0 = t; }
        else             { if (t < t0) return false; if (t < t1) t1 = t; }
    }
    const float nx0 = x0 + t0 * dx, ny0 = y0 + t0 * dy;
    const float nx1 = x0 + t1 * dx, ny1 = y0 + t1 * dy;
    x0 = nx0; y0 = ny0; x1 = nx1; y1 = ny1;
    return true;
}

// --- panel colours, sampled off the reference frames -----------------------
// The panel is a dark, semi-transparent green-grey wash over the world; the
// roads are a bright green; the labels are white on a dark plate; the route
// highlight is a blue rectangle.
static const float kPanelR = 0.11f, kPanelG = 0.20f, kPanelB = 0.15f, kPanelA = 0.78f;
static const float kBorderR = 0.35f, kBorderG = 0.72f, kBorderB = 0.62f;
static const float kRoadR = 0.42f, kRoadG = 1.00f, kRoadB = 0.47f;
static const float kRouteR = 0.25f, kRouteG = 0.55f, kRouteB = 1.00f;
// world units -> panel pixels, at the 800-px reference layout (measured off
// shots/ref/ref_cab_and_map.png: the road numbers/settlements ~1.5 km apart
// sit ~200 px apart).
static const float kWorldPerPx = 0.133f;

// Which esc16 mask backs which marker.  The names are the ones the original
// loads into its map-icon globals (docs/original_ui.md 2.6).
static const char* kIconMask[] = {
    "",                       // kNone
    "m_16\\I_city0.msk",      // kCity (settlement, class 0)
    "m_16\\I_fuel0.msk",      // kFuel «АЗС»
    "m_16\\I_park.msk",       // kPark «П»
    "m_16\\I_repair.msk",     // kRepair
    "m_16\\I_tank.msk",       // kTank (fuel depot / gas holder sign)
    "m_16\\I_debris.msk",     // kHazard
    "m_16\\I_box.msk",        // kStore (warehouse / cargo)
    "m_16\\I_gazhold.msk",    // kGasholder
    "m_16\\I_race.msk",       // kRace
};
static const int kIconCount = (int)(sizeof kIconMask / sizeof kIconMask[0]);

// ---------------------------------------------------------------------------
// loading / indexing
// ---------------------------------------------------------------------------
bool Map::load(Renderer& r, const std::string& dataDir, const std::vector<Way::Seg>& segs,
               const std::map<std::string, Way::Node>& nodes) {
    if (!indexed_) {
        index(segs, nodes);
        indexed_ = true;
    }
    // the marker masks live in the in-game UI container
    Res res;
    if (res.load(dataDir + "/COMMON/esc16.RES")) {
        for (int i = 1; i < kIconCount; ++i) {
            const Res::Entry* e = res.find(kIconMask[i]);
            if (!e || e->kind != Res::Kind::Mask) {
                LOG("map: icon mask %s missing", kIconMask[i]);
                continue;
            }
            DecodedImage img;
            if (!Mask::decode(res.data(*e), e->size, img)) continue;
            iconTex_[i] = r.createTexture(img.rgba.data(), img.width, img.height);
            iconW_[i] = (unsigned)img.width;
            iconH_[i] = (unsigned)img.height;
            if (iconTex_[i]) owned_.push_back(iconTex_[i]);
        }
    }
    int have = 0;
    for (int i = 1; i < kIconCount; ++i) if (iconTex_[i]) ++have;
    LOG("map: %zu road polylines (%zu segments), %zu markers, %d/%d icon masks, "
        "world x %.0f..%.0f z %.0f..%.0f",
        segs_.size(), segs.size(), pins_.size(), have, kIconCount - 1, x0_, x1_, z0_, z1_);
    return !segs_.empty();
}

void Map::index(const std::vector<Way::Seg>& segs, const std::map<std::string, Way::Node>& nodes) {
    nodes_ = nodes;
    bool first = true;
    for (const auto& s : segs) {
        if (s.pts.size() < 2) continue;
        Seg e;
        e.pts = s.pts.data();
        e.n = s.pts.size();
        // The original draws the carriageways of a road at the same weight; the
        // wider (>=14 u) ones are the highways, so they get the brighter ink the
        // reference frames show for the main road.
        e.main = s.width >= 14.0;
        segs_.push_back(e);
        if (!s.name.empty() &&
            std::find(roads_.begin(), roads_.end(), s.name) == roads_.end())
            roads_.push_back(s.name);
        for (const auto& p : s.pts) {
            const float fx = (float)p.x, fz = (float)p.z;
            if (first) {
                x0_ = x1_ = fx; z0_ = z1_ = fz;
                first = false;      // initialised once, on the first point
            }
            x0_ = std::min(x0_, fx); x1_ = std::max(x1_, fx);
            z0_ = std::min(z0_, fz); z1_ = std::max(z1_, fz);
        }
    }
    // --- facility markers, from the original's `map*` node convention -------
    int nTown = 0, nFuel = 0, nSto = 0, nPark = 0, nStore = 0, nOther = 0;
    for (const auto& kv : nodes) {
        const std::string& n = kv.first;
        const float x = (float)kv.second.pos.x, z = (float)kv.second.pos.z;
        if (n.rfind("map_town", 0) == 0) { ++nTown; addPin(x, z, "", kCity, 1.0f, n); }
        else if (n.rfind("mapBenzo", 0) == 0) { ++nFuel; addPin(x, z, "", kFuel, 0.8f); }
        else if (n.rfind("mapNumBenzo", 0) == 0) { ++nFuel; addPin(x, z, "", kFuel, 0.8f); }
        else if (n.rfind("mapSto", 0) == 0) { ++nSto; addPin(x, z, "", kRepair, 0.8f); }
        else if (n.rfind("mapPark", 0) == 0) { ++nPark; addPin(x, z, "", kPark, 0.8f); }
        else if (n.rfind("mapNameStore", 0) == 0) { ++nStore; addPin(x, z, "", kStore, 0.8f); }
        else if (n.rfind("map_gasholder", 0) == 0) { ++nOther; addPin(x, z, "", kGasholder, 0.8f); }
        else if (n.rfind("debris_", 0) == 0) { ++nOther; addPin(x, z, "", kHazard, 0.8f); }
        else if (n.rfind("map_", 0) == 0) { ++nOther; addPin(x, z, "", kStore, 0.8f); }
    }
    LOG("map: markers from .way - %d town, %d fuel, %d service, %d parking, "
        "%d store, %d other", nTown, nFuel, nSto, nPark, nStore, nOther);
    // Highway tags: the road names the .way data carries are translated into the
    // «А3С NN»-style shield text of the retail map, and one tag is placed every
    // ~4 km of each named road (the reference frames show them at that spacing).
    std::sort(roads_.begin(), roads_.end());
    for (size_t r = 0; r < roads_.size(); ++r) {
        const std::string& name = roads_[r];
        // W005 -> 5; the class letter comes from the road's own weight below.
        int num = 0;
        for (char c : name) if (isdigit((unsigned char)c)) num = num * 10 + (c - '0');
        if (num <= 0) num = (int)(r + 1);
        RoadTag tag;
        tag.spacing = 4000.0f;                 // 4 km between two shields
        char buf[32];
        float bestW = 0.0f, ax = 0.0f, az = 0.0f;
        float acc = 0.0f;
        bool have = false;
        for (const auto& sg : segs) {
            if (sg.name != name || sg.pts.size() < 2) continue;
            for (size_t k = 0; k < sg.pts.size(); ++k) {
                const float px2 = (float)sg.pts[k].x, pz2 = (float)sg.pts[k].z;
                if (!have) { ax = px2; az = pz2; have = true; continue; }
                acc += std::sqrt((px2 - ax) * (px2 - ax) + (pz2 - az) * (pz2 - az));
                bestW = std::max(bestW, (float)sg.width);
                ax = px2; az = pz2;
                if (acc >= (float)roadTags_.size() * 300.0f + tag.spacing) {
                    // one tag per 4 km of this road, keyed by its own name
                    acc = 0.0f;
                    RoadTag t;
                    t.x = px2; t.z = pz2;
                    t.spacing = tag.spacing;
                    snprintf(buf, sizeof buf, "А3С %d", num);
                    t.text = buf;
                    roadTags_.push_back(t);
                }
            }
        }
    }
    LOG("map: %zu road tags on %zu named roads", roadTags_.size(), roads_.size());
}

void Map::addPin(float x, float z, const std::string& label, int icon, float s,
                 const std::string& node) {
    for (auto& p : pins_) {
        if (p.icon != icon) continue;
        if (std::fabs(p.x - x) < 240.0f && std::fabs(p.z - z) < 240.0f) {
            if (p.label.empty() && !label.empty()) { p.label = label; p.scale = s; }
            if (p.node.empty()) p.node = node;
            return;                       // map* / map*_l / map*_h zoom twins
        }
    }
    Pin p;
    p.x = x; p.z = z; p.label = label; p.node = node; p.icon = icon; p.scale = s;
    pins_.push_back(p);
}

// The scripts name every settlement and station; the economy already resolved
// those names to .way positions, so take the labels from it.
void Map::addEconomy(const Economy& eco) {
    auto place = [&](const std::string& label, float x, float z, int icon, float s) {
        if (label.empty()) return;
        // A settlement already placed by the `map_town_*` node of its tile keeps
        // its position and only gains the name; otherwise add a new pin.
        float best = 1e18f;
        Pin* hit = nullptr;
        for (auto& p : pins_) {
            if (p.icon != icon && !(icon == kCity && p.icon == kCity)) continue;
            float d = (p.x - x) * (p.x - x) + (p.z - z) * (p.z - z);
            if (d < best) { best = d; hit = &p; }
        }
        if (hit && best < 900.0f * 900.0f) {
            if (hit->label.empty()) { hit->label = label; hit->scale = s; }
            return;
        }
        addPin(x, z, label, icon, s);
    };
    // The town labels the player actually sees: every job endpoint is a town.
    for (const auto& j : eco.jobs()) {
        if (j.fromX == 0.0f && j.fromZ == 0.0f) continue;
        place(j.fromTown, j.fromX, j.fromZ, kCity, 1.0f);
        place(j.toTown, j.toX, j.toZ, kCity, 1.0f);
    }
    for (const auto& d : eco.dealers())
        if (d.resolved) addPin(d.x, d.z, "", kPark, 0.8f);
    LOG("map: economy layer -> %zu markers", pins_.size());
}

// `town` field 8 carries the label node names and the petrol stations their
// numbers («АЗС 22»); field 3 is the numeric id used in that name.  The label of
// a settlement therefore comes from its own `map_town_*` node, which is exactly
// what the retail map prints next to the name.
void Map::addGameData(const GameData& gd) {
    int named = 0, stations = 0, labelled = 0;
    for (const auto& t : gd.towns) {
        const bool station = t.petrol;
        const int icon = t.petrol ? kFuel : (t.fuelDepot ? kTank : kStore);
        char buf[64];
        const char* label = t.display.c_str();
        if (t.petrol) {
            // the original shows these as «АЗС NN» (the oil-station number)
            if (t.townId > 0) snprintf(buf, sizeof buf, "АЗС %d", t.townId);
            else snprintf(buf, sizeof buf, "%s", label);
            label = buf;
        }
        // 1) the settlement marker: the town's own `map_town_*` label node.  The
        //    scripts also name it in field 8, so match on the node name first.
        bool city = false;
        std::string townNode;
        for (const auto& nm : t.mapIcons)
            if (nm.rfind("map_town", 0) == 0) { townNode = nm; break; }
        if (!townNode.empty() && nodes_.count(townNode)) {
            const Way::Node& nd = nodes_.at(townNode);
            addPin((float)nd.pos.x, (float)nd.pos.z, label, kCity, 1.0f, townNode);
            city = true;
            ++labelled;
        }
        // 2) every other marker of the icon group goes on its own node.
        for (const auto& nm : t.mapIcons) {
            if (nm == townNode) continue;
            auto it = nodes_.find(nm);
            if (it == nodes_.end()) continue;
            const int ic = (nm.rfind("map_town", 0) == 0) ? kCity
                         : (nm.rfind("mapBenzo", 0) == 0) ? kFuel
                         : (nm.rfind("mapSto", 0) == 0) ? kRepair
                         : (nm.rfind("mapPark", 0) == 0) ? kPark
                         : (nm.rfind("mapNameStore", 0) == 0) ? kStore
                         : (nm.rfind("map_gasholder", 0) == 0) ? kGasholder : kStore;
            addPin((float)it->second.pos.x, (float)it->second.pos.z,
                   station && ic == kFuel ? label : std::string(), ic,
                   station ? 0.8f : 1.0f, nm);
            if (!city && station && ic == kFuel) ++stations;
        }
        ++named;
    }
    LOG("map: GameData layer -> %d towns (%d settlement labels, %d stations) / "
        "%zu markers", named, labelled, stations, pins_.size());
}

// ---------------------------------------------------------------------------
// per-frame
// ---------------------------------------------------------------------------
void Map::update(float px, float pz, float yawDeg) {
    px_ = px; pz_ = pz; yaw_ = yawDeg;
}

void Map::setRoute(float x, float z, bool has, const std::string& label) {
    rx_ = x; rz_ = z; haveRoute_ = has; routeLabel_ = label;
}

float Map::iconSizePx(int icon, float scale) const {
    if (icon <= 0 || icon >= kIconCount || !iconTex_[icon]) return 7.0f * scale;
    // The 13x13 masks the original draws; scale 1 = the 800-px UI layout.
    return (float)std::max(iconW_[icon], iconH_[icon]) * scale;
}

// World -> panel pixel.  The panel shows a fixed window of the world centred on
// the player, exactly like the retail map (the reference frames put the truck
// just below the middle of the panel).
void Map::draw(Renderer& r, Font& labelFont, float x, float y, float w, float h, float scale) {
    if (!wanted() || segs_.empty()) { lastLabels_ = lastSegs_ = 0; report_.clear(); return; }
    unsigned white = r.whiteTex();
    // The retail map shows roughly 2 km across the panel (the reference frames
    // cover ~6.6 km over the whole panel height).  `scale` is the panel's
    // uniform art scale, so the world-to-pixel factor stays proportional:
    // 0.133 px/unit at the 800-px reference = about 1 km per 133 panel pixels.
    const float ws = kWorldPerPx * scale;
    auto wr = [&](float wx) { return x + w * 0.45f + (wx - px_) * ws; };
    auto wz = [&](float wz) { return y + h * 0.60f + verticalOffset(wz,pz_,ws); };

    // --- panel wash + explored border --------------------------------------
    r.quad2D(white, x, y, w, h, kPanelR, kPanelG, kPanelB, kPanelA);
    const float bw = std::max(1.0f, 1.0f * scale);
    r.quad2D(white, x, y, w, bw, kBorderR, kBorderG, kBorderB, 0.9f);
    r.quad2D(white, x, y + h - bw, w, bw, kBorderR, kBorderG, kBorderB, 0.9f);
    r.quad2D(white, x, y, bw, h, kBorderR, kBorderG, kBorderB, 0.9f);
    r.quad2D(white, x + w - bw, y, bw, h, kBorderR, kBorderG, kBorderB, 0.9f);

    // --- roads --------------------------------------------------------------
    // Clip to the panel by hand (the renderer has no scissor here).
    const float cx0 = x, cx1 = x + w, cy0 = y, cy1 = y + h;
    size_t drawn = 0, labels = 0;
    // Occupied label boxes (the markers, the road shields and the names all
    // claim space; the first one drawn wins).
    const float rs = scale * 800.0f / 600.0f;      // panel scale -> 800-px UI scale
    std::vector<std::array<float, 4>> taken;
    auto overlaps = [&](float x0, float y0, float x1, float y1, float pad) {
        for (const auto& t : taken)
            if (x1 + pad > t[0] && x0 - pad < t[2] && y1 + pad > t[1] && y0 - pad < t[3])
                return true;
        return false;
    };
    const float thickMain = std::max(1.0f, 1.6f * scale);
    const float thickSide = std::max(1.0f, 1.0f * scale);
    for (const auto& s : segs_) {
        float ax = 0, az = 0;
        bool have = false;
        for (size_t k = 0; k < s.n; ++k) {
            const float bxp = wr((float)s.pts[k].x), bzp = wz((float)s.pts[k].z);
            if (have) {
                float x0 = ax, y0 = az, x1 = bxp, y1 = bzp;
                if (clipSegment(x0, y0, x1, y1, cx0, cy0, cx1, cy1)) {
                    r.line2D(white, x0, y0, x1, y1, s.main ? thickMain : thickSide,
                             kRoadR, kRoadG, kRoadB, 0.95f);
                    ++drawn;
                }
            }
            ax = bxp; az = bzp; have = true;
        }
    }

    // --- active route: the blue rectangle of the original --------------------
    if (haveRoute_) {
        const float tx = wr(rx_), tz = wz(rz_);
        const float t = std::max(1.0f, 1.0f * scale);
        // What the reference frames show: a thin BLUE RECTANGLE around the road
        // section between the truck and the destination (and a blue plate on the
        // target itself).
        float bx0 = std::min(wr(px_), tx), bx1 = std::max(wr(px_), tx);
        float bz0 = std::min(wz(pz_), tz), bz1 = std::max(wz(pz_), tz);
        const float pad = 14.0f * scale;
        bx0 = std::max(cx0, bx0 - pad); bx1 = std::min(cx1, bx1 + pad);
        bz0 = std::max(cy0, bz0 - pad); bz1 = std::min(cy1, bz1 + pad);
        r.line2D(white, bx0, bz0, bx1, bz0, t, kRouteR, kRouteG, kRouteB, 0.95f);
        r.line2D(white, bx0, bz1, bx1, bz1, t, kRouteR, kRouteG, kRouteB, 0.95f);
        r.line2D(white, bx0, bz0, bx0, bz1, t, kRouteR, kRouteG, kRouteB, 0.95f);
        r.line2D(white, bx1, bz0, bx1, bz1, t, kRouteR, kRouteG, kRouteB, 0.95f);
        r.quad2D(white, tx - 3 * scale, tz - 3 * scale, 6 * scale, 6 * scale,
                 kRouteR, kRouteG, kRouteB, 0.95f);
        if (!routeLabel_.empty())
            labelFont.draw(r, routeLabel_, tx + 6.0f * scale, tz - 4.0f * scale,
                           scale * 800.0f / 600.0f, 0.75f, 0.88f, 1.0f, 1.0f);
    }

    // --- facility icons + labels --------------------------------------------
    // The markers and their text are authored in the same 800-px UI space as the
    // HUD strip, so they keep the original pixel sizes at every resolution; only
    // their world positions follow the panel's world scale.
    // Cities first so their names win the space, then the smaller markers.
    std::vector<const Pin*> order;
    order.reserve(pins_.size());
    for (const auto& p : pins_) order.push_back(&p);
    std::stable_sort(order.begin(), order.end(),
                     [](const Pin* a, const Pin* b) { return a->icon < b->icon; });
    for (const Pin* p : order) {
        const float sx = wr(p->x), sz = wz(p->z);
        if (sx < cx0 - 14 * rs || sx > cx1 + 14 * rs) continue;
        if (sz < cy0 - 14 * rs || sz > cy1 + 14 * rs) continue;
        const float isz = iconSizePx(p->icon, rs);
        const bool city = (p->icon == kCity);
        const float ix0 = sx - isz * 0.5f, iy0 = sz - isz * 0.5f;
        if (overlaps(ix0, iy0, ix0 + isz, iy0 + isz, 0.0f)) continue;
        taken.push_back({ix0, iy0, ix0 + isz, iy0 + isz});

        if (p->icon > 0 && p->icon < kIconCount && iconTex_[p->icon])
            r.quad2D(iconTex_[p->icon], ix0, iy0, isz, isz, 1, 1, 1, 0.98f);
        else
            r.quad2D(white, sx - 2.5f * rs, sz - 2.5f * rs, 5 * rs, 5 * rs,
                     kRoadR, kRoadG, kRoadB, 0.95f);

        const std::string& lab = p->label;
        if (lab.empty()) continue;
        // City names are the big white labels of the reference; the facility
        // tags («АЗС 22», «П») are the small ones beside their icon.
        const float fs = (city ? 1.0f : 0.85f) * p->scale * rs;
        const float tw = labelFont.textWidth(lab, fs);
        float lx = sx + isz * 0.6f + 1.0f * rs;
        float ly = sz - fs * 4.2f;
        if (lx + tw > cx1 - 2.0f * rs) lx = sx - isz * 0.6f - 1.0f * rs - tw;
        if (lx < cx0 + 2.0f * rs) lx = cx0 + 2.0f * rs;
        if (ly < cy0 + 2.0f * rs) ly = sz + isz * 0.4f;
        const float lh = fs * 9.0f;
        if (overlaps(lx, ly, lx + tw, ly + lh, 1.5f * rs)) continue;
        taken.push_back({lx, ly, lx + tw, ly + lh});
        r.quad2D(white, lx - 1.5f * rs, ly - 1.0f * rs, tw + 3.0f * rs, lh,
                 0.02f, 0.05f, 0.03f, 0.85f);
        labelFont.draw(r, lab, lx, ly, fs, 1.0f, 1.0f, 1.0f, 1.0f);
        ++labels;
    }

    // --- highway shields: «А3С 22», the road numbers of the reference frames --
    for (const auto& t : roadTags_) {
        const float sx = wr(t.x), sz = wz(t.z);
        if (sx < cx0 + 14 * rs || sx > cx1 - 14 * rs) continue;
        if (sz < cy0 + 6 * rs || sz > cy1 - 6 * rs) continue;
        const float fs = 0.9f * rs;
        const float tw = labelFont.textWidth(t.text, fs);
        const float lh = fs * 9.0f;
        if (overlaps(sx - tw * 0.5f, sz - lh * 0.5f, sx + tw * 0.5f, sz + lh * 0.5f, 2.0f * rs))
            continue;
        taken.push_back({sx - tw * 0.5f, sz - lh * 0.5f, sx + tw * 0.5f, sz + lh * 0.5f});
        r.quad2D(white, sx - tw * 0.5f - 2.0f * rs, sz - lh * 0.5f, tw + 4.0f * rs, lh,
                 0.05f, 0.22f, 0.12f, 0.80f);
        labelFont.draw(r, t.text, sx - tw * 0.5f, sz - lh * 0.5f, fs,
                       0.95f, 1.0f, 0.95f, 1.0f);
        ++labels;
    }

    // --- the player: the original's arrow ------------------------------------
    {
        const float sx = wr(px_), sz = wz(pz_);
        // +x is right, +z is up: reflect the heading with the graph as well.
        const float a = headingRadians(yaw_);
        const float cs = cosf(a), sn = sinf(a);
        const float as = scale * 800.0f / 600.0f;
        const float L = 6.0f * as, Wd = 4.0f * as;
        const float tri[6] = {0, -L, -Wd, L, Wd, L};
        float xy[8], uv[8] = {0, 0, 1, 0, 1, 1, 0, 1};
        for (int i = 0; i < 4; ++i) {
            float tx = tri[(i % 3) * 2], ty = tri[(i % 3) * 2 + 1];
            xy[i * 2] = sx + tx * cs - ty * sn;
            xy[i * 2 + 1] = sz + tx * sn + ty * cs;
        }
        r.pushQuad2D(white, xy, uv, 1.0f, 0.30f, 0.22f, 1.0f);
    }

    lastSegs_ = drawn;
    lastLabels_ = labels;
    char buf[192];
    snprintf(buf, sizeof buf,
             "map: panel %.0fx%.0f at %.0f,%.0f scale %.2f | %zu road polylines drawn, "
             "%zu labels, %zu markers, %zu pins total",
             w, h, x, y, scale, drawn, labels, taken.size(), pins_.size());
    report_ = buf;
}

} // namespace ht2

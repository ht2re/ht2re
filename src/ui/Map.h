#pragma once
#include <string>
#include <vector>
#include <map>
#include "Font.h"
#include "../world/Way.h"

namespace ht2 {
class Renderer;
class GameData;
class Economy;

// The original in-game ROAD MAP overlay (docs/original_ui.md 2.6):
//
// The retail game draws a large semi-transparent map panel over the LEFT part of
// the screen while driving (verified in the reference frames of
// shots/ref/ref_map.png / ref_map_and_fine.png / ref_cab_and_map.png).  It shows
//   * the road network as thin bright-green lines,
//   * the settlement names in the game font (Приозерск, Залесск, Ельнино, …),
//   * the facilities that the scripts name with the `map*` .way nodes:
//       map_town_*      a settlement label
//       mapBenzo_*      a petrol station  «АЗС NN»
//       mapSto_*        a service / repair shop «П»
//       mapPark_*       a parking bay
//       mapNameStore_*  a warehouse (cargo)
//       map_gasholder_* a gas holder / hazard
//       debris_*        a road hazard
//     (the naming convention is the one docs/script_gameplay.md §5.6 records for
//      `town` field 8: the label node + 4 zoom variants),
//   * the player as an arrow and the active route as a blue rectangle.
//
// The panel geometry is authored in the same 800-pixel-wide UI space as the HUD
// strip (see Hud::layoutScale), so at 800x600 it lands exactly where the
// original does: the panel starts under the strip and covers the left third of
// the screen.
class Map {
public:
    // One labelled facility marker.
    struct Pin {
        float x = 0, z = 0;
        std::string label;      // «АЗС 22», «Приозерск», …
        std::string node;       // the .way node it came from (map_town_ad, …)
        int icon = 0;           // 0 none, see Icon
        float scale = 1.0f;     // label importance (settlements draw bigger)
    };
    enum Icon { kNone = 0, kCity, kFuel, kPark, kRepair, kTank, kHazard, kStore, kGasholder, kRace };

    // A highway shield number: the original prints «А3С 22»-style tags on the
    // main roads (the reference frames show four of them at ~1.5 km spacing).
    // The .way road names (W001, W005, ...) are the road identity the shipped
    // data carries, so the tag is derived from that name.
    struct RoadTag {
        float x = 0, z = 0;
        std::string text;      // «А3С 22»
        float spacing = 0;     // world units between two tags of this road
    };

    // Reference-space (800-px UI) panel rectangle, y measured from the strip's
    // bottom edge.  Measured off shots/ref/ref_cab_and_map.png / ref_map_and_fine.png.
    static constexpr float kPanelX = 6.0f;
    static constexpr float kPanelW = 268.0f;
    static constexpr float kPanelTop = 2.0f;     // below the 50-px strip
    static constexpr float kPanelH = 434.0f;     // down to y = 486 of 600

    // Loads the map icon masks from COMMON/esc16.RES and indexes the road network
    // of every loaded .way (road segments, facility nodes).  Safe to call again;
    // the heavy indexing happens once.
    bool load(Renderer& r, const std::string& dataDir, const std::vector<Way::Seg>& segs,
              const std::map<std::string, Way::Node>& nodes);
    bool loaded() const { return !segs_.empty(); }

    // Optional extra layers (the economy's towns/dealers/services).  Called once
    // the economy exists; additive, so a missing layer never breaks the map.
    void addEconomy(const Economy& eco);
    void addGameData(const GameData& gd);

    void setVisible(bool v) { visible_ = v; }
    bool visible() const { return visible_; }
    void toggle() { visible_ = !visible_; }
    // Show it even in non-interactive (screenshot) runs.  Tests and the
    // side-by-side shots use this.
    void setForced(bool f) { forced_ = f; }
    bool forced() const { return forced_; }
    // Whether the panel should be drawn this frame (the player's toggle, or the
    // test/--map override).
    bool wanted() const { return visible_ || forced_; }

    // Per-frame state: where the player is, where the route goes.
    void update(float px, float pz, float yawDeg);
    void setRoute(float x, float z, bool has, const std::string& label);

    // Draw.  (x, y, w, h) is the panel in screen pixels; scale is the HUD's
    // 800-px UI scale (so the icons/labels keep the original proportions).
    void draw(Renderer& r, Font& labelFont, float x, float y, float w, float h, float scale);

    size_t roadSegments() const { return segs_.size(); }
    size_t pins() const { return pins_.size(); }
    size_t visibleLabels() const { return lastLabels_; }
    size_t visibleRoadSegments() const { return lastSegs_; }
    // One machine-readable line per frame (shot_test reads it).
    const std::string& report() const { return report_; }

private:
    struct Seg { const Way::Pos* pts; size_t n; bool main; };
    // Icons uploaded from the esc16 masks.
    unsigned iconTex_[16] = {0};
    unsigned iconW_[16] = {0}, iconH_[16] = {0};
    std::vector<unsigned> owned_;

    void index(const std::vector<Way::Seg>& segs, const std::map<std::string, Way::Node>& nodes);
    void addPin(float x, float z, const std::string& label, int icon, float s,
                const std::string& node = std::string());
    float iconSizePx(int icon, float scale) const;

    std::vector<Seg> segs_;               // flattened road polylines
    std::vector<Pin> pins_;
    std::vector<std::string> roads_;      // road names (W001, ...), for the road tags
    std::vector<RoadTag> roadTags_;
    std::map<std::string, Way::Node> nodes_;   // the named .way node table
    float x0_ = 0, x1_ = 0, z0_ = 0, z1_ = 0;   // world bounds of the network

    float px_ = 0, pz_ = 0, yaw_ = 0;
    float rx_ = 0, rz_ = 0;
    bool haveRoute_ = false;
    std::string routeLabel_;

    bool visible_ = true;
    bool forced_ = false;
    bool indexed_ = false;
    size_t lastLabels_ = 0, lastSegs_ = 0;
    std::string report_;
};

} // namespace ht2

#pragma once
// The complete in-game interface: the ESC screens *and* every in-game panel of
// the original (docs/esc_screens.md, docs/original_ui.md, docs/ui_sort.md).
//
// Everything here is copied from the reverse-engineering reports, not invented:
//
//   * every screen is a **1200-pixel-tall vertical panel** (or a full 1600x1200
//     page / a 280x405 popup) decoded out of original/COMMON/esc16.RES (and
//     ind16.RES for the "5 yes/no" popup);
//   * the panels are drawn **1:1, never scaled**, at the verified anchor
//         y = -(1200 - screenH)/2 - 50        (0x110f20, docs/esc_screens.md 2.2)
//     docked left (x = 0), at screenW-224 (225-wide), screenW-383 (385-wide),
//     screenW-640 (the 640-wide service panel) or centred (full pages/popups);
//   * the button tables (art base name, original widget id, panel-local X/Y)
//     are the tables of docs/esc_screens.md 3.  The Russian captions are
//     **baked into the art** - no font or string table is needed for them.
//
// The class owns no game state: it *reads* the world modules (Economy, Vehicle,
// Way, GameData) through a non-owning Context and turns input into an *action
// string* which the App maps onto the world (refuel / repair / accept a job /
// save / load / restart / leave the world ...).  Screen navigation itself
// (open:<name>, back) is handled internally.
//
// Screen coordinates are panel-local + the anchor; the original clips whatever
// falls outside the window, and so does the GL viewport here.
#include <map>
#include <string>
#include <vector>
#include <cstdint>
#include "../res/Res.h"

namespace ht2 {

class Renderer;
class Font;
class Economy;
class Vehicle;
class Way;
class GameData;

class EscMenu {
public:
    // Where a 1200-tall panel is docked (docs/esc_screens.md 2.2).
    enum class Dock { Left, Right225, Right385, Right640, Center };
    // The ESC family is loaded eagerly (docs/esc_impl.md); the in-game panels
    // are decoded on first open, so a screenshot only pays for its own art.
    enum class Family { Esc, Panel };

    struct Button {
        std::string name;      // art base inside esc16.RES ("savg", "gaspay")
        std::string action;    // comma-separated: "open:save" | "restart" | ...
        int id = 0;            // original widget id (0xa7 ...)
        int x = 0, y = 0;      // panel-local top-left
        int w = 150, h = 30;
        bool disabled = false; // network game: paints the _f art
        // the original paints some hot spots into the backdrop itself
        // (the "СТАВИТЬ ВСЁ" row of the service panel): such a button is
        // clickable and navigable but draws no art of its own
        bool invisible = false;
        unsigned texA = 0, texP = 0, texAP = 0, texF = 0;   // a / p / ap / f
        bool hasP = false, hasAP = false, hasF = false;
    };

    // A slider row: a pair of arrow masks at the ends of the track painted into
    // the backdrop, plus the 12x22 "runner" knob showing the value.
    struct Slider {
        std::string name;          // the key the App uses ("voll", "skidl", "fuel-amount")
        std::string artL, artR;    // "m_16\\voll" / "m_16\\volr"
        std::string label;         // human/log name ("master")
        std::string valueKey;      // "fuel" when the row edits a game value
        int trackX0 = 41, trackX1 = 143;   // painted track, panel-local
        int trackY = 0;
        float value = 0.5f;
        unsigned texL = 0, texR = 0, texLA = 0, texRA = 0, texRun = 0, texRunA = 0;
        int arrowW = 16, arrowH = 28, runW = 12, runH = 22;
        bool flipR = false;        // the right arrow is the same art mirrored
    };

    // One text line inside a panel.  `key` is a data key of this table:
    //   money fuel fuel.pct fuel.liters fuel.cost damage clock speed gear pos odo
    //   vehicle driver road jobs jobs.near upgrades slot level
    //   job.cargo job.from job.to job.reward job.distance job.status
    //   order.cargo order.to order.premium order.time order.weight order.volume
    //   order.fragile sale.code sale.price sale.power sale.capacity
    //   hire.name hire.tariff hire.truck hire.price hire.power hire.capacity
    //   price.fuel price.repair ask
    //   key.<i>.k / key.<i>.d   (row i of EscMenu::keyBindings())
    //   text:<literal>           (a fixed string)
    struct Row { int x = 0, y = 0; std::string key; float scale = 1.4f; };

    // A decorative/authentic picture of a panel (portraits, service icons ...).
    struct Image { std::string art; int x = 0, y = 0; int w = 0, h = 0; unsigned tex = 0; };

    // One value printed into a *painted* cell of the backdrop (the 7-segment
    // LED windows of the fuel panel, the price cells of the service panel's
    // upgrade grid).  A rect lets the selected cell be highlighted.
    struct Cell { int x = 0, y = 0, w = 0, h = 0; std::string key; float scale = 1.3f; bool money = false; };

    struct KeyRow { const char* keys; const char* desc; };
    // What a Screen's data table shows (docs/ui_sort.md 3).
    enum class TableKind { None, Jobs, Vehicles };

    struct Screen {
        std::string name;
        std::string backdrop;   // "m_16\\BackEscMain.txr" (esc16.RES / ind16.RES)
        Dock dock = Dock::Left;
        Family family = Family::Panel;
        int backdropW = 215, backdropH = 1200;
        unsigned backdropTex = 0;
        std::vector<Button> buttons;
        std::vector<Slider> sliders;
        std::vector<Row> rows;
        std::vector<Image> images;
        std::vector<Cell> cells;
        // An optional data table (the ДИСПЕТЧЕРСКАЯ job board): the columns' x,
        // the first row's y, the row pitch and how many rows are drawn.
        std::vector<int> tableCols;
        TableKind tableKind = TableKind::None;
        int tableY = 0, tablePitch = 20, tableRows = 0;
        bool keyList = false;   // the 16 bindings beside the controls panel
        bool slotList = false;  // the save-slot line (save/load)
        bool question = false;  // the "5 yes/no" popup: draw ask()'s question
    };

    // The world data the panels display.  Non-owning pointers into the App's
    // modules: the UI layer reads them, it does not duplicate their logic.  The
    // App fills this once per frame (docs/ui_sort.md 4).
    struct Context {
        const Economy* economy = nullptr;
        const Vehicle* vehicle = nullptr;
        const Way* way = nullptr;
        const GameData* gameData = nullptr;
        float clockHours = 9.0f;          // App::timeOfDay_
        long refuelCost = -1;             // App's own pricing, -1 = unknown
        long repairCost = -1;
        long fuelPerLitre = -1;           // integer price, -1 = unknown (demo uses 9)
        double fuelUnit = -1;             // king.exe per-litre price, -1 = use fuelPerLitre
        float tankLitres = -1;            // the driven vehicle's tank, -1 = the demo 200 l
        std::string driverName;            // currently hired driver, empty = player
        std::string slotText;             // App::saveSummary()
        std::string notice;               // a transient message on the panel
        float noticeTimer = 0.0f;
    };

    bool load(const std::string& dataDir);   // esc16.RES + ind16.RES + the tables
    bool loaded() const { return res_.loaded(); }
    // Decode + upload the ESC family's art (the original batch, unchanged).
    // Needs a live GL context; the panels are decoded lazily on first open().
    void loadArt(Renderer& r);
    bool artLoaded() const { return artLoaded_; }

    void open(const std::string& screenName);
    void close() { active_ = false; dock_ = false; }
    bool active() const { return active_; }
    // The order board is a proximity overlay: it draws and takes OK, and the
    // truck keeps driving. Every other screen pauses the world.
    void showDock(const std::string& screenName);
    void hideDock();
    bool docked() const { return dock_ && active_; }
    bool pausesWorld() const { return active_ && !dock_; }
    const std::string& screen() const { return cur_; }
    const Screen* current() const;
    const std::vector<Screen>& screens() const { return screens_; }
    int screenCount() const { return (int)screens_.size(); }
    int panelCount() const;
    bool panelOpen() const;               // the open screen is an in-game panel

    // --- world data -------------------------------------------------------
    void setContext(const Context& c) { ctx_ = c; dataDirty_ = true; }
    const Context& context() const { return ctx_; }
    void refresh();                       // rebuild the cached data from ctx_
    // Index of the world object the open panel's action refers to
    // (Economy::offers() for "sale"/"buyvehicle", Economy::jobs() for
    // "order"/"acceptjob"/"deliverjob"), -1 when the panel is not open.
    int selectedOffer() const { return data_.saleIndex; }
    int selectedJob() const { return data_.orderIndex; }
    const std::string& selectedHireName() const { return data_.hireName; }
    // The upgrade the service panel has highlighted ("" when none / other panel):
    // a GameData::upgrades key ("PR_FORCED_ENGINE") and its price.  The App
    // applies it (deduct the price, then tell the UI with markUpgradeBought()).
    const std::string& selectedUpgrade() const;
    long selectedUpgradePrice() const;
    void markUpgradeBought(const std::string& id);
    void clearBoughtUpgrades() { bought_.clear(); }
    bool upgradeBought(const std::string& id) const;
    const std::vector<std::string>& boughtUpgrades() const { return bought_; }
    // Every upgrade cell of the service panel as (GameData key, price) in the
    // painted order - the App's "СТАВИТЬ ВСЁ" (tuneall) handler sums these.
    std::vector<std::pair<std::string, long>> upgradeList() const;
    int listSelection() const { return listSel_; }
    // The cell index of an upgrade key on the service panel (-1 when absent).
    int upgradeIndex(const std::string& id) const;
    // The value a row renders (tests / logs): rowText("fuel","fuel") == "42%".
    std::string rowText(const std::string& screen, const std::string& key) const;

    // --- navigation -------------------------------------------------------
    void selectNext(int delta);
    void adjustSelected(int delta);     // sliders / the checkbox / the fuel row
    void activateSelected();
    void handleKey(int key);            // SDLK_* (nav + activate)
    void handleMouse(float mx, float my, bool click, int vpW, int vpH);
    void update(float dt);

    // Activate by button name / by original widget id on the current screen.
    // Returns the action string ("" when there is no such button).
    std::string activateName(const std::string& name);
    std::string activateId(int id);
    // Esc / gamepad B: sub-screen -> main screen, main screen -> resume driving.
    std::string back();
    // The screen an in-game hotkey opens (SDL key) - "f1", "f2", "f3", "f5"...
    // "" when the key is not a UI hotkey.  The App calls it while driving.
    std::string hotkey(int key) const;

    // The "5 yes/no" popup (m16\MenuPopupA5.msk, 494x160): remember the pending
    // question and the action "ДА" has to produce, then open the popup.
    void ask(const std::string& question, const std::string& yesAction);
    const std::string& askText() const { return askText_; }

    // Action produced by the last input/activation ("" when none).  The App
    // consumes it once per frame.
    std::string takeActivated();
    const std::string& lastButton() const { return lastButton_; }

    void draw(Renderer& r, Font& font);

    // Save-slot line shown on the save/load screens (set by the App).
    void setSlotText(const std::string& s) { slotText_ = s; }

    // --- geometry (also used by the tests and docs/esc_impl.md) ------------
    // A 1200-px-tall *docked* panel is vertically centred and shifted 50 px up
    // (docs/esc_screens.md 2.2); a centred screen (the 1600x1200 win/lose pages,
    // the 280x405 / 494x160 popups) is centred 1:1 in both axes instead.
    static float panelY(int screenH) { return -(1200.0f - (float)screenH) * 0.5f - 50.0f; }
    float panelTop(int screenH) const {
        const Screen* s = current();
        if (!s) return panelY(screenH);
        if (s->dock == Dock::Center) return ((float)screenH - (float)s->backdropH) * 0.5f;
        return panelY(screenH);
    }
    float panelX(int screenW) const;
    int viewW() const { return vpW_; }
    int viewH() const { return vpH_; }
    float toScreenX(float px) const { return panelX(vpW_) + px; }
    float toScreenY(float py) const { return panelTop(vpH_) + py; }
    // The open screen's rectangle on screen (false when nothing is open).
    bool screenRect(float& x, float& y, float& w, float& h) const;

    // --- introspection (tests / logs) -------------------------------------
    int selectedIndex() const { return sel_; }
    int navCount() const { return (int)nav_.size(); }
    const Button* selectedButton() const;
    const Slider* selectedSlider() const;
    const Slider* slider(const std::string& name) const;
    float sliderValue(const std::string& name) const;
    int buttonId(const std::string& name) const;   // original widget id, 0 = unknown
    void setSliderValue(const std::string& name, float v);
    float autoGear() const { return autoGear_; }
    void setAutoGear(float v) { autoGear_ = v < 0.5f ? 0.0f : 1.0f; }
    static const std::vector<KeyRow>& keyBindings();

private:
    struct Nav { bool slider; int index; };
    // The data the rows/table render, rebuilt from Context (or the HT2_UI_DEMO
    // test hook when no Context is set) by refresh().
    struct Data {
        long money = 0;
        float fuel = 0, damage = 0, clock = 9.0f, speed = 0, odo = 0;
        float posX = 0, posZ = 0;
        int gear = 0, level = 0;
        std::string vehicle, road, driver, slot, station;
        size_t jobsTotal = 0, jobsNear = 0, upgrades = 0, offers = 0, delivered = 0;
        long refuelCost = -1, repairCost = -1, fuelPrice = -1;
        double fuelUnit = -1;
        float tankLitres = -1;
        bool hasJob = false;
        std::string jobCargo, jobFrom, jobTo;
        long jobReward = 0;
        float jobDistance = 0;
        int orderIndex = -1;
        std::string orderCargo, orderTo;
        long orderReward = 0;
        float orderDistance = 0;
        std::string orderWeight, orderVolume;
        bool orderIllegal = false;
        int saleIndex = -1;
        std::string saleCode, saleName, saleClass;
        long salePrice = 0, salePower = 0;
        int hireIndex = -1;
        std::string hireName, hireRole, hireTruck;
        long hireTariff = 0;
        std::vector<std::vector<std::string>> table;   // 4-column job board
        // the vehicle market (the ПРОДАЁТСЯ list): name/price per offer, plus
        // the Economy::offers() index each line refers to
        std::vector<std::vector<std::string>> vehicles;
        std::vector<int> vehicleIdx;
        long tradeIn = 0, toPay = 0;
        // the tuning table (GameData::upgrades: PR_* id -> price)
        std::map<std::string, long> upgPrice;
    };

    void buildTables();
    void buildNav();
    int firstEnabled() const;
    std::string runButton(int index);
    std::string actionForNav(int navIndex);
    // Runs a comma-separated action list: "open:x" navigates, the first terminal
    // token is returned to the App (the continuation "cbask,open:cbpol_ans").
    std::string runAction(const std::string& action);
    const Screen* find(const std::string& name) const;
    bool ensureArt(const std::string& screenName);   // lazy per-panel decode
    std::string field(const std::string& key) const;
    void drawKeyList(Renderer& r, Font& font, float ax, float ay);
    void drawSlotList(Renderer& r, Font& font, float ax, float ay);
    void drawRows(Renderer& r, Font& font, const Screen& s, float ax, float ay);
    void drawCells(Renderer& r, Font& font, const Screen& s, float ax, float ay);
    void drawTable(Renderer& r, Font& font, const Screen& s, float ax, float ay);

    Res res_;             // COMMON/esc16.RES (the in-game UI)
    Res resInd_;          // COMMON/ind16.RES (the MenuPopupA5 mask + 5yes/5no)
    std::vector<Screen> screens_;
    std::vector<Nav> nav_;
    std::string cur_;
    std::string activated_, lastButton_, slotText_;
    std::string askText_, askAction_, askReturn_;
    Context ctx_;
    Data data_;
    bool dataDirty_ = true;
    Renderer* renderer_ = nullptr;
    bool active_ = false;
    bool dock_ = false;          // order board shown by driving up to a window
    bool artLoaded_ = false;
    int sel_ = -1;
    float time_ = 0.0f;
    float mouseX_ = -1.0f, mouseY_ = -1.0f;
    std::string pressed_;        // the _p (pressed) art is shown for a moment
    float pressT_ = 0.0f;
    float autoGear_ = 0.0f;      // the 4th (checkbox) row of the controls panel
    int cbChannel_ = 9;          // the CB radio channel of the F5 pages
    int listSel_ = 0;            // the highlighted row/cell of the open list
    std::vector<std::string> bought_;   // the upgrades this session paid for
    void moveSelection(int delta);      // lists (vehicles / upgrades)
    std::vector<const Cell*> upgradeCells() const;
    unsigned checkOff_ = 0, checkOn_ = 0, checkOnSel_ = 0;   // check_a / check_pp / check_ap
    int checkW_ = 19, checkH_ = 17;
    int vpW_ = 1280, vpH_ = 720;
};

} // namespace ht2

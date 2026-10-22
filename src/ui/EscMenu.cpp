// The in-game interface: ESC screens + every in-game panel — see EscMenu.h,
// docs/esc_screens.md (the reverse-engineering report) and docs/ui_sort.md.
//
// All coordinates below are the *panel-local* ones of docs/esc_screens.md 3
// (verified from king.exe + the decoded art).  One correction to that document
// is applied here: the ESC-main table of section 3.1 prints the button Y as the
// 800x600 *screen* coordinate (104 = 454 - 350); the real panel-local Y is
// 454 + 45k, which is what the container translation (0x11152e: +18, +454) and
// the 45-px layout pitch (0x111513) produce — and what the verification render
// tools/out/esc/_compose_escmain.png shows.  The other tables are already
// panel-local.  See docs/esc_impl.md and docs/ui_sort.md for the panel tables.
//
// The text rows of the panels are placed inside the label/value boxes *painted
// into the backdrop*: the box rectangles were measured from the decoded art
// (their borders), so a value lands exactly in its painted cell.
#include "EscMenu.h"
#include "Font.h"
#include "../gfx/Renderer.h"
#include "../res/Mask.h"
#include "../game/Economy.h"
#include "../game/Vehicle.h"
#include "../world/Way.h"
#include "../script/GameData.h"
#include "../util/Log.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ht2 {

// ---------------------------------------------------------------------------
// Screen tables (docs/esc_screens.md 3.1 .. 3.6 and 3.7 .. 3.14)
// ---------------------------------------------------------------------------
namespace {

struct BtnDef { const char* name; int id; int x, y; const char* action; int w, h; };
struct SliderDef { const char* name; const char* artL; const char* artR; const char* label;
                   const char* valueKey; int y; bool flipR; };
struct RowDef { int x, y; const char* key; float scale = 1.4f; };
struct CellDef { int x, y, w, h; const char* key; float scale; bool money; };

// The sound screen: 4 sliders (container at (22, 489), pitch 70) + OK / CANCEL.
const SliderDef kSndSliders[] = {
    {"voll",  "m_16\\voll",  "m_16\\volr",  "master",  "", 489, false},
    {"musl",  "m_16\\musl",  "m_16\\musr",  "music",   "", 559, false},
    {"engl",  "m_16\\engl",  "m_16\\engr",  "engine",  "", 629, false},
    {"skidl", "m_16\\skidl", "m_16\\skidr", "brake",   "", 699, false},
};
// The controls screen: the slider container is at (22, 511) with the same 70-px
// pitch and only **three** widgets (sens / centre / force); the fourth painted
// row ("АВТОМАТИЧЕСКАЯ КОРОБКА ПЕРЕДАЧ") is a check box, not a slider — the
// report's "16 key rows at (22,511)" is a misreading of these three ADDs.
const SliderDef kCtrlSliders[] = {
    {"sensl",   "m_16\\sensl",   "m_16\\sensr",   "steer-sensitivity", "", 511, false},
    {"centerl", "m_16\\centerl", "m_16\\centerr", "steer-return",      "", 581, false},
    {"forcel",  "m_16\\forcel",  "m_16\\forcer",  "force-feedback",    "", 651, false},
};

// docs/original_ui.md 3.1 — the 16 in-game key bindings of the control-help
// screen (matched positionally there).
const EscMenu::KeyRow kKeys[] = {
    {"<Home><End><Del><PgDn>", "Chase Camera Control"},
    {">", "Right View"},
    {"<", "Left View"},
    {"<V>", "Cockpit Toggle"},
    {"<W>", "Wipers"},
    {"<H>", "Horn"},
    {"<0>", "Recover and Continue"},
    {"<U>", "TV Director"},
    {"<N>", "Rear Camera"},
    {"<C>", "Choose Camera"},
    {"<M>", "Switch Camera On/Off"},
    {"<Z>", "Change Gear Down"},
    {"<A>", "Change Gear Up"},
    {"<Left>,<Right>", "Turn"},
    {"<Down>", "Brake"},
    {"<Up>", "Accelerate"},
};

// The tank capacity used by the "ЛИТРЫ" cell of the fuel panel.  The shipped
// vehicle data has no tank volume this port reads, so the panel shows the level
// on a 200-litre tank (documented deviation, docs/ui_sort.md 6).
const float kTankLitres = 200.0f;

unsigned uploadRes(Renderer& r, const Res& res, const Res::Entry* e, int* w, int* h) {
    if (!e) return 0;
    DecodedImage img;
    bool ok = Mask::decode(res.data(*e), e->size, img);
    if (!ok) ok = Texture::decode(res.data(*e), e->size, img);
    if (!ok || img.width <= 0 || img.height <= 0) {
        LOG("esc: cannot decode %s", e->name.c_str());
        return 0;
    }
    if (w) *w = img.width;
    if (h) *h = img.height;
    return r.createTexture(img.rgba.data(), img.width, img.height);
}

const char* dockName(EscMenu::Dock d) {
    switch (d) {
        case EscMenu::Dock::Right225: return "right225";
        case EscMenu::Dock::Right385: return "right385";
        case EscMenu::Dock::Right640: return "right640";
        case EscMenu::Dock::Center:   return "center";
        default:                      return "left";
    }
}

std::string fmtMoney(long v) {
    char b[32];
    snprintf(b, sizeof b, "%ld", v);
    return b;
}

} // namespace

const std::vector<EscMenu::KeyRow>& EscMenu::keyBindings() {
    static const std::vector<KeyRow> v(kKeys, kKeys + sizeof(kKeys) / sizeof(kKeys[0]));
    return v;
}

// ---------------------------------------------------------------------------
// Tables
// ---------------------------------------------------------------------------
void EscMenu::buildTables() {
    screens_.clear();

    auto addScreen = [&](const char* name, const char* backdrop, Dock dock,
                         Family family, int backdropW, int backdropH) -> Screen& {
        Screen s;
        s.name = name;
        s.backdrop = backdrop;
        s.dock = dock;
        s.family = family;
        s.backdropW = backdropW;
        s.backdropH = backdropH;
        screens_.push_back(s);
        return screens_.back();
    };
    auto addButtons = [](Screen& s, const BtnDef* defs, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            Button b;
            b.name = defs[i].name;
            b.id = defs[i].id;
            b.x = defs[i].x;
            b.y = defs[i].y;
            b.w = defs[i].w;
            b.h = defs[i].h;
            b.action = defs[i].action;
            s.buttons.push_back(b);
        }
    };
    auto addSliders = [](Screen& s, const SliderDef* defs, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            Slider sl;
            sl.name = defs[i].name;
            sl.artL = defs[i].artL;
            sl.artR = defs[i].artR;
            sl.label = defs[i].label;
            sl.valueKey = defs[i].valueKey;
            // the 16x28 arrow pair sits at the ends of the painted track, which
            // spans panel x 41..143 in every panel art (measured on
            // BackEscSnd/BackEscCtrl); the track line is 12 px below the row top.
            sl.trackX0 = 41;
            sl.trackX1 = 143;
            sl.trackY = defs[i].y + 12;
            sl.value = 0.5f;
            sl.flipR = defs[i].flipR;
            s.sliders.push_back(sl);
        }
    };
    auto addCells = [](Screen& s, const CellDef* defs, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            Cell c;
            c.x = defs[i].x; c.y = defs[i].y; c.w = defs[i].w; c.h = defs[i].h;
            c.key = defs[i].key; c.scale = defs[i].scale; c.money = defs[i].money;
            s.cells.push_back(c);
        }
    };
    auto addRows = [](Screen& s, const RowDef* defs, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            Row r;
            r.x = defs[i].x;
            r.y = defs[i].y;
            r.key = defs[i].key;
            r.scale = defs[i].scale;
            s.rows.push_back(r);
        }
    };

    // =======================================================================
    // The ESC family (docs/esc_screens.md 3.1 .. 3.6) — unchanged
    // =======================================================================

    // --- ESC main: ПРОБЛЕМЫ В ПУТИ (3.1) -----------------------------------
    // Panel-local y = 454 + 45k; the labels are baked into the 150x30 art.
    static const BtnDef kMain[] = {
        {"savg",    0xa7, 18, 454, "open:save",    150, 30},   // СОХРАНИТЬ ИГРУ
        {"loadg",   0x6e, 18, 499, "open:load",    150, 30},   // ЗАГРУЗИТЬ ИГРУ
        {"restart", 0xa8, 18, 544, "restart",      150, 30},   // НАЧАТЬ СНАЧАЛА
        {"911",     0xa6, 18, 589, "recover",      150, 30},   // 911
        {"control", 0xa5, 18, 634, "open:control", 150, 30},   // УПРАВЛЕНИЕ
        {"sound",   0xa4, 18, 679, "open:sound",   150, 30},   // ЗВУК
        {"main",    0xa3, 18, 724, "menu",         150, 30},   // ГЛАВНОЕ МЕНЮ
        {"exitsys", 0xa9, 18, 769, "open:exit",    150, 30},   // ВЫХОД
    };
    {
        Screen& s = addScreen("main", "m_16\\BackEscMain.txr", Dock::Left, Family::Esc, 215, 1200);
        addButtons(s, kMain, sizeof(kMain) / sizeof(kMain[0]));
    }

    // --- exit popup (3.2): same backdrop, explicit rects --------------------
    static const BtnDef kExit[] = {
        {"exsave",  0xaa, 18, 564, "exitsave",   150, 30},     // СОХРАНИТЬ
        {"exnsave", 0xab, 18, 614, "exitnosave", 150, 30},     // НЕ СОХРАНЯТЬ
        {"excanc",  0xac, 18, 664, "back",       150, 30},     // ОТМЕНА
    };
    {
        Screen& s = addScreen("exit", "m_16\\BackEscMain.txr", Dock::Left, Family::Esc, 215, 1200);
        addButtons(s, kExit, sizeof(kExit) / sizeof(kExit[0]));
    }

    // --- sound (3.3) -------------------------------------------------------
    static const BtnDef kOkCanc[] = {
        {"ok",   0xb8, 21, 762, "back", 55, 30},   // OK
        {"canc", 0x2,  77, 762, "back", 80, 30},   // ОТМЕНА
    };
    {
        Screen& s = addScreen("sound", "m_16\\BackEscSnd.txr", Dock::Left, Family::Esc, 215, 1200);
        addSliders(s, kSndSliders, sizeof(kSndSliders) / sizeof(kSndSliders[0]));
        addButtons(s, kOkCanc, sizeof(kOkCanc) / sizeof(kOkCanc[0]));
    }

    // --- controls (3.4 + correction above) ---------------------------------
    {
        Screen& s = addScreen("control", "m_16\\BackEscCtrl.txr", Dock::Left, Family::Esc, 215, 1200);
        s.keyList = true;
        addSliders(s, kCtrlSliders, sizeof(kCtrlSliders) / sizeof(kCtrlSliders[0]));
        addButtons(s, kOkCanc, sizeof(kOkCanc) / sizeof(kOkCanc[0]));   // the same OK/CANCEL
    }

    // --- save game (3.5) ---------------------------------------------------
    static const BtnDef kSave[] = {
        {"save",    0xad,  24, 679, "writesave",  130, 30},    // СОХРАНИТЬ
        {"delgame", 0x11a, 24, 724, "deletesave", 130, 30},    // УДАЛИТЬ
        {"nsave",   0xac,  24, 769, "back",       130, 30},    // ОТМЕНА
    };
    {
        Screen& s = addScreen("save", "m_16\\BackEscSave.txr", Dock::Left, Family::Esc, 215, 1200);
        s.slotList = true;
        addButtons(s, kSave, sizeof(kSave) / sizeof(kSave[0]));
    }

    // --- load game (3.6) ---------------------------------------------------
    static const BtnDef kLoad[] = {
        {"load",    0x6e,  24, 679, "loadfile",   130, 30},    // ЗАГРУЗИТЬ
        {"delgame", 0x11a, 24, 724, "deletesave", 130, 30},    // УДАЛИТЬ
        {"nsave",   0xac,  24, 769, "back",       130, 30},    // ОТМЕНА
    };
    {
        Screen& s = addScreen("load", "m_16\\BackEscLoad.txr", Dock::Left, Family::Esc, 215, 1200);
        s.slotList = true;
        addButtons(s, kLoad, sizeof(kLoad) / sizeof(kLoad[0]));
    }

    // =======================================================================
    // The in-game panels (docs/esc_screens.md 3.7 .. 3.14)
    // =======================================================================

    // --- fuel station (3.7): BackGas 225x1200 right-docked -----------------
    // The art paints "УРОВЕНЬ ... ПОЛНЫЙ" over a tick scale and the "ЛИТРЫ / $"
    // value cells.  The two widgets the report places at (25,655) and (175,655)
    // are the ends of the amount scale (art m_16\gasfull_*.txr, 21x21).
    static const BtnDef kFuel[] = {
        {"gaspay",  0x113, 52,  755, "refuel", 70, 30},   // OK
        {"gascanc", 0x2,   123, 755, "back",   80, 30},   // ОТМЕНА
    };
    static const SliderDef kFuelSlider[] = {
        {"fuel-amount", "m_16\\gasfull", "m_16\\gasfull", "fuel-amount", "fuel", 655, true},
    };
    static const RowDef kFuelRows[] = {
        {158, 627, "fuel"},         // the level in the free cell of the УРОВЕНЬ row
        {50,  726, "fuel.liters", 1.2f},  // ЛИТРЫ cell
        {176, 726, "fuel.cost", 1.2f},    // $ cell (the App's price for the level)
    };
    // the five 7-segment LED windows the art paints: СУММА, $ / ЛИТРЫ / ЗА ЛИТР
    // (the amount being pumped) and the tank level in ЛИТРЫ / $ at the bottom
    static const CellDef kFuelCells[] = {
        {66,  466, 110, 24, "fuel.buy.cost",   1.4f, true},   // СУММА, $
        {66,  519, 110, 24, "fuel.buy.liters", 1.4f, false},  // ЛИТРЫ
        {66,  572, 110, 24, "fuel.price",      1.4f, true},   // ЗА ЛИТР
        {50,  707, 72,  16, "fuel.liters",     1.2f, false},  // the tank level
        {134, 707, 72,  16, "fuel.cost",       1.2f, true},   // what refuel() charges
    };
    {
        Screen& s = addScreen("fuel", "m_16\\BackGas.txr", Dock::Right225, Family::Panel, 225, 1200);
        addButtons(s, kFuel, sizeof(kFuel) / sizeof(kFuel[0]));
        addSliders(s, kFuelSlider, sizeof(kFuelSlider) / sizeof(kFuelSlider[0]));
        s.sliders[0].trackX0 = 46;      // arrows land at x = 25 and x = 175
        s.sliders[0].trackX1 = 175;
        s.sliders[0].trackY = 655;
        addCells(s, kFuelCells, sizeof(kFuelCells) / sizeof(kFuelCells[0]));
        addRows(s, kFuelRows, sizeof(kFuelRows) / sizeof(kFuelRows[0]));
    }

    // --- service station (3.8): BackService 640x1200, x = screenW-640 -------
    // The report gives the buttons' x (209 / 349) but not their y; 790 is the
    // first free band below the painted "МОЙ БАЛАНС / ИТОГО" row (y 747..766) —
    // documented deviation (docs/ui_sort.md 6).
    static const BtnDef kService[] = {
        {"servpay",  0x117, 209, 790, "repair", 130, 30},   // OK
        {"servcanc", 0x2,   349, 790, "back",   130, 30},   // ОТМЕНА
    };
    static const RowDef kServiceRows[] = {
        {24,  748, "damage", 1.2f},       // current damage (left of the boxes)
        {255, 748, "money", 1.2f},        // the МОЙ БАЛАНС value box
        {435, 748, "price.repair", 1.2f}, // the ИТОГО value box
    };
    // The tuning interface: every price cell the BackService art paints for the
    // 19 upgrades of restable.sch (read through GameData::upgrades).  The painted
    // labels are the Russian names, so only the price is drawn; the highlighted
    // cell is boxed and "servpay" applies it.  PR_TIRES is one script entry with
    // two variants (the two ШИНЫ rows), so both cells show its price.
    static const CellDef kServiceCells[] = {
        {604, 535, 58, 16, "price.repair", 1.2f, true},              // ПОЛНЫЙ РЕМОНТ
        {174, 599, 58, 16, "upg.PR_ADJUSTABLE_SUSPENSION", 1.15f, true},
        {174, 616, 58, 16, "upg.PR_BRAKES",                1.15f, true},
        {174, 632, 58, 16, "upg.PR_ENGINE_BRAKE",          1.15f, true},
        {174, 649, 58, 16, "upg.PR_REINFORCED_CHASSIS",    1.15f, true},
        {174, 665, 58, 16, "upg.PR_BULETPROOF_GLASS",      1.15f, true},
        {174, 682, 58, 16, "upg.PR_HEADLIGHT_PROTECTION",  1.15f, true},
        {374, 599, 58, 16, "upg.PR_FORCED_ENGINE",         1.15f, true},
        {374, 616, 58, 16, "upg.PR_OIL_ADDITIVE",          1.15f, true},
        {374, 632, 58, 16, "upg.PR_AIR_FILTER_SNORBEL",    1.15f, true},
        {374, 649, 58, 16, "upg.PR_TIRES",                 1.15f, true},
        {374, 665, 58, 16, "upg.PR_TIRES",                 1.15f, true},
        {374, 682, 58, 16, "upg.PR_EXTRA_TANK",            1.15f, true},
        {374, 698, 58, 16, "upg.PR_ABS",                   1.15f, true},
        {604, 599, 58, 16, "upg.PR_ANTI_RADAR",            1.15f, true},
        {604, 616, 58, 16, "upg.PR_STEALTH_BODY_PAINT",    1.15f, true},
        {604, 632, 58, 16, "upg.PR_CARGO_SCANNER",         1.15f, true},
        {604, 649, 58, 16, "upg.PR_FIRE_PROTECTION",       1.15f, true},
        {604, 665, 58, 16, "upg.PR_NAVIGATOR",             1.15f, true},
        {604, 682, 58, 16, "upg.PR_HORN",                  1.15f, true},
    };
    // the scroll arrows of the tuning list / the market list (m_16\m{up,down,run})
    static const BtnDef kListBtns[] = {
        {"mup",   0, 24, 790, "listup",   21, 20},
        {"mdown", 0, 48, 790, "listdown", 21, 20},
        {"mrun",  0, 72, 790, "listrun",  21, 20},
    };
    {
        Screen& s = addScreen("service", "m_16\\BackService.txr", Dock::Right640, Family::Panel, 640, 1200);
        addButtons(s, kService, sizeof(kService) / sizeof(kService[0]));
        addButtons(s, kListBtns, sizeof(kListBtns) / sizeof(kListBtns[0]));
        {
            // "СТАВИТЬ ВСЁ" is painted into the backdrop: the button is a hot spot
            Button all; all.name = "tuneall"; all.action = "tuneall";
            all.x = 452; all.y = 696; all.w = 150; all.h = 20; all.invisible = true;
            s.buttons.push_back(all);
        }
        addCells(s, kServiceCells, sizeof(kServiceCells) / sizeof(kServiceCells[0]));
        addRows(s, kServiceRows, sizeof(kServiceRows) / sizeof(kServiceRows[0]));
        // the truck/service pictures: the positions are the two explicit rect
        // writes the report tracked inside the service ctor (231,613)/(412,481)
        Image car; car.art = "m_16\\servcar";  car.x = 231; car.y = 613; s.images.push_back(car);
        Image up;  up.art  = "m_16\\upgradet"; up.x  = 412; up.y  = 481; s.images.push_back(up);
    }

    // --- market: ЗАКАЗ / ПРОДАЁТСЯ / ИЩУ РАБОТУ (225x1200, right-docked) ----
    // The painted value cells of each panel start right after its label boxes
    // (measured from the decoded art: order x=106 from y=554, sale x=106 from
    // y=584, job x=82 from y=564, 20-px row pitch).
    static const BtnDef kMarket[] = {
        {"ok",   0xb8, 40,  700, "TERMINAL", 55, 30},
        {"canc", 0x2,  105, 700, "back",     80, 30},
    };
    static const RowDef kOrderRows[] = {
        {124, 557, "order.cargo", 1.2f}, {124, 577, "order.to", 1.2f},
        {124, 597, "order.premium", 1.2f}, {124, 617, "order.time", 1.2f},
        {124, 637, "order.weight", 1.2f}, {124, 657, "order.volume", 1.2f},
        {124, 677, "order.fragile", 1.2f},
    };
    static const RowDef kSaleRows[] = {
        {102, 587, "sale.code", 1.2f}, {102, 607, "sale.price", 1.2f},
        {142, 627, "sale.power", 1.2f}, {142, 647, "sale.capacity", 1.2f},
    };
    static const RowDef kHireRows[] = {
        {128, 567, "hire.name", 1.2f}, {128, 587, "hire.tariff", 1.2f},
        {102, 607, "hire.truck", 1.2f}, {102, 627, "hire.price", 1.2f},
        {152, 647, "hire.power", 1.2f}, {146, 667, "hire.capacity", 1.2f},
    };
    {
        BtnDef b[2];
        memcpy(b, kMarket, sizeof(b));
        b[0].action = "acceptjob";
        Screen& s = addScreen("order", "m_16\\BackOrder.txr", Dock::Right225, Family::Panel, 225, 1200);
        addButtons(s, b, 2);
        addRows(s, kOrderRows, sizeof(kOrderRows) / sizeof(kOrderRows[0]));
    }
    static const BtnDef kSaleBtns[] = {
        {"mup",   0, 150, 440, "listup",   21, 20},
        {"mdown", 0, 150, 466, "listdown", 21, 20},
        {"mrun",  0, 150, 492, "listrun",  21, 20},
        {"ok",    0xb8, 40,  700, "buyvehicle", 55, 30},
        {"canc",  0x2,  105, 700, "back",       80, 30},
    };
    static const RowDef kSaleExtra[] = {
        {22, 550, "text:TRADE-IN", 0.95f}, {120, 550, "text:TO PAY", 0.95f},
        {22, 566, "sale.tradein", 1.05f},  {120, 566, "sale.topay", 1.05f},
    };
    {
        // the purchase interface: the free upper half of BackSale carries the
        // vehicle list (name | price), the painted КОД/ЦЕНА/МОЩНОСТЬ/ГРУЗОПОДЪЕМН
        // rows show the highlighted line, and OK buys it (with the trade-in)
        Screen& s = addScreen("sale", "m_16\\BackSale.txr", Dock::Right225, Family::Panel, 225, 1200);
        addButtons(s, kSaleBtns, sizeof(kSaleBtns) / sizeof(kSaleBtns[0]));
        addRows(s, kSaleRows, sizeof(kSaleRows) / sizeof(kSaleRows[0]));
        addRows(s, kSaleExtra, sizeof(kSaleExtra) / sizeof(kSaleExtra[0]));
        s.tableCols = {24, 118};
        s.tableKind = TableKind::Vehicles;
        s.tableY = 446;
        s.tablePitch = 19;
        s.tableRows = 6;
    }
    {
        BtnDef b[2];
        memcpy(b, kMarket, sizeof(b));
        b[0].action = "hiredriver";
        Screen& s = addScreen("job", "m_16\\BackJob.txr", Dock::Right225, Family::Panel, 225, 1200);
        addButtons(s, b, 2);
        addRows(s, kHireRows, sizeof(kHireRows) / sizeof(kHireRows[0]));
    }

    // --- market info / ДИСПЕТЧЕРСКАЯ (3.14): BackInfo 385x1200 --------------
    // The art paints a 4-column table (ГРУЗ | ОТКУДА | КУДА | ДЕНЬГИ): header
    // values at y=516, ten 20-px rows from y=556, columns at 42/149/239/327.
    static const BtnDef kInfo[] = {
        {"ok",   0xb8, 40,  766, "deliverjob", 55, 30},
        {"canc", 0x2,  105, 766, "back",       80, 30},
    };
    static const RowDef kInfoRows[] = {
        // the footer band, below the ten table rows (the art's header row already
        // carries the ГРУЗ | ОТКУДА | КУДА | ДЕНЬГИ labels)
        {42,  740, "money"}, {149, 740, "jobs"}, {239, 740, "jobs.near"}, {327, 740, "clock"},
    };
    {
        Screen& s = addScreen("info", "m_16\\BackInfo.txr", Dock::Right385, Family::Panel, 385, 1200);
        addButtons(s, kInfo, sizeof(kInfo) / sizeof(kInfo[0]));
        addRows(s, kInfoRows, sizeof(kInfoRows) / sizeof(kInfoRows[0]));
        s.tableCols = {42, 149, 239, 327};
        s.tableY = 537;              // the art's body rows run 534..734
        s.tablePitch = 20;
        s.tableRows = 10;
    }

    // --- my truck (3.11): BackMy 415x1200, left-docked ----------------------
    // The art paints ten 3-column boxes (16..74 | 88..160 | 172..327) from
    // y=564; the two buttons sit at the verified (85,774)/(186,774).
    static const BtnDef kMy[] = {
        {"fire", 0xba,  85, 774, "firedriver",    100, 30},   // УВОЛИТЬ
        {"chan", 0xb9, 186, 774, "changevehicle", 166, 30},   // ПОМЕНЯТЬ МАШИНУ
    };
    static const RowDef kMyRows[] = {
        // ДЕНЬГИ | МАШИНЫ | ВСЕГО | МОГУ НАНЯТЬ sit above this (empty) value row
        {36, 483, "money"}, {174, 483, "offers"}, {228, 483, "delivered"}, {286, 483, "jobs.near"},
        // the driver table (ИМЯ | АВАНС | ПРИБЫЛЬ | МАШИНА): ten 20-px rows
        {19, 547, "driver", 1.2f}, {92, 547, "road", 1.2f}, {168, 547, "level", 1.2f}, {245, 547, "vehicle", 1.2f},
        {19, 567, "pos", 1.2f}, {92, 567, "clock", 1.2f}, {168, 567, "fuel", 1.2f}, {245, 567, "damage", 1.2f},
        {19, 587, "money", 1.2f}, {92, 587, "odo", 1.2f}, {168, 587, "speed", 1.2f}, {245, 587, "gear", 1.2f},
        {19, 607, "price.fuel", 1.2f}, {92, 607, "price.repair", 1.2f}, {168, 607, "upgrades", 1.2f}, {245, 607, "jobs", 1.2f},
    };
    {
        Screen& s = addScreen("mytruck", "m_16\\BackMy.txr", Dock::Left, Family::Panel, 415, 1200);
        addButtons(s, kMy, sizeof(kMy) / sizeof(kMy[0]));
        addRows(s, kMyRows, sizeof(kMyRows) / sizeof(kMyRows[0]));
    }

    // --- in-game help F1/F2/F3 (3.14) --------------------------------------
    // BackF1 (415) is the key-binding page: 19 painted 20-px rows, two cells
    // (key box 16..164, description box 186..338) from y=454.  The 16 bindings
    // of docs/original_ui.md 3.1 go into the first 16 rows.
    static const BtnDef kBack80[] = {{"canc", 0x2, 40, 760, "back", 80, 30}};
    static const BtnDef kBack80Low[] = {{"canc", 0x2, 40, 840, "back", 80, 30}};
    {
        Screen& s = addScreen("f1", "m_16\\BackF1.txr", Dock::Left, Family::Panel, 415, 1200);
        addButtons(s, kBack80Low, 1);      // the art's 19 painted rows reach y=834
        const std::vector<KeyRow>& kb = keyBindings();
        for (size_t i = 0; i < kb.size() && i < 16; ++i) {
            char k[24];
            snprintf(k, sizeof k, "key.%zu.k", i); Row rk; rk.x = 20; rk.y = 457 + 20 * (int)i; rk.key = k;
            s.rows.push_back(rk);
            snprintf(k, sizeof k, "key.%zu.d", i); Row rd; rd.x = 190; rd.y = 457 + 20 * (int)i;
            rd.key = k; rd.scale = 1.15f;
            s.rows.push_back(rd);
        }
    }
    // BackF2 (415) is a 4-column statistics table (cells 49..116 | 123..201 |
    // 207..278 | 284..360, rows from y=476)
    {
        Screen& s = addScreen("f2", "m_16\\BackF2.txr", Dock::Left, Family::Panel, 415, 1200);
        addButtons(s, kBack80Low, 1);
        static const int cx[4] = {52, 126, 210, 287};
        static const char* cells[5][4] = {
            {"money", "jobs", "offers", "delivered"},
            {"fuel", "damage", "odo", "clock"},
            {"vehicle", "speed", "gear", "level"},
            {"pos", "road", "jobs.near", "upgrades"},
            {"price.fuel", "price.repair", "job.cargo", "job.status"},
        };
        for (int r = 0; r < 5; ++r)
            for (int c = 0; c < 4; ++c) {
                Row row; row.x = cx[c]; row.y = 479 + 20 * r; row.key = cells[r][c];
                s.rows.push_back(row);
            }
    }
    // BackF2Tball (215): a single column of 20-px rows from y=515
    {
        Screen& s = addScreen("f2tball", "m_16\\BackF2Tball.txr", Dock::Left, Family::Panel, 215, 1200);
        addButtons(s, kBack80, 1);
        static const char* keys[8] = {"money", "fuel", "damage", "odo",
                                      "speed", "gear", "clock", "level"};
        for (int i = 0; i < 8; ++i) {
            Row r; r.x = 19; r.y = 567 + 20 * i; r.key = keys[i];
            s.rows.push_back(r);
        }
    }
    // BackF3 (215): two columns (16..93 | 95..173) from y=606
    {
        Screen& s = addScreen("f3", "m_16\\BackF3.txr", Dock::Left, Family::Panel, 215, 1200);
        addButtons(s, kBack80, 1);
        static const char* keys[6][2] = {
            {"money", "fuel"}, {"damage", "jobs"}, {"odo", "speed"},
            {"clock", "jobs.near"}, {"vehicle", "level"}, {"upgrades", "delivered"},
        };
        for (int r = 0; r < 6; ++r)
            for (int c = 0; c < 2; ++c) {
                Row row; row.x = 19 + 79 * c; row.y = 610 + 20 * r; row.key = keys[r][c];
                s.rows.push_back(row);
            }
    }

    // --- win / lose (3.12): 1600x1200 art centred 1:1 -----------------------
    // The container the code moves holds the 130x30 buttons at a 160-px pitch
    // (0x1bdee0 spacing = 290 - 130); *_main sits at the verified (589,687).
    static const BtnDef kWinBtns[] = {
        {"ow_rest", 0xa8, 429, 687, "restart", 130, 30},   // НАЧАТЬ СНАЧАЛА
        {"ow_main", 0xa3, 589, 687, "menu",    130, 30},   // ГЛАВНОЕ МЕНЮ
        {"ow_cont", 0xac, 749, 687, "resume",  130, 30},   // ПРОДОЛЖАТЬ
    };
    static const BtnDef kLoseBtns[] = {
        {"o_rest", 0xa8, 429, 687, "restart", 130, 30},
        {"o_main", 0xa3, 589, 687, "menu",    130, 30},
    };
    {
        Screen& s = addScreen("win", "m_16\\BackWin.txr", Dock::Center, Family::Panel, 1600, 1200);
        addButtons(s, kWinBtns, 3);
    }
    {
        Screen& s = addScreen("win1", "m_16\\BackWin1.txr", Dock::Center, Family::Panel, 1600, 1200);
        addButtons(s, kWinBtns, 3);
    }
    {
        Screen& s = addScreen("win2", "m_16\\BackWin2.txr", Dock::Center, Family::Panel, 1600, 1200);
        addButtons(s, kWinBtns, 3);
    }
    for (int i = 1; i <= 5; ++i) {
        char nm[16], art[40];
        snprintf(nm, sizeof nm, "lose%d", i);
        snprintf(art, sizeof art, "m_16\\BackLoose%d.txr", i);
        Screen& s = addScreen(nm, art, Dock::Center, Family::Panel, 1600, 1200);
        addButtons(s, kLoseBtns, 2);
    }

    // --- the "5 yes/no" popup (3.10): m16\MenuPopupA5.msk (ind16), 494x160 --
    static const BtnDef kA5[] = {
        {"5yes", 0xa0, 128, 96, "yes", 100, 49},   // ДА
        {"5no",  0xa1, 266, 96, "no",  100, 49},   // НЕТ
    };
    {
        Screen& s = addScreen("popup5", "m16\\MenuPopupA5.msk", Dock::Center, Family::Panel, 494, 160);
        s.question = true;
        addButtons(s, kA5, sizeof(kA5) / sizeof(kA5[0]));
    }

    // --- F5 CB radio (3.13): 280x405 masks, centred 1:1 --------------------
    // The 4 questions sit on the verified x=11 / y=205+45k grid; the pages with
    // only three questions carry their CANCEL art in the free 4th row.
    struct F5Def { const char* page; const char* art; int asks; int askIds[4]; };
    static const F5Def kF5[] = {
        {"cbpol", "m_16\\BackF5pol.msk", 4, {0xbe, 0xbf, 0xc0, 0xc1}},
        {"cbmaf", "m_16\\BackF5maf.msk", 4, {0xc2, 0xc3, 0xc4, 0xc5}},
        {"cbopp", "m_16\\BackF5opp.msk", 3, {0xc6, 0xc7, 0xc8, 0}},
        {"cbnei", "m_16\\BackF5nei.msk", 3, {0xca, 0xcb, 0xcc, 0}},
    };
    for (const F5Def& f : kF5) {
        const std::string pg = f.page;
        const std::string pre = std::string("F5") + pg[2];      // "F5p"
        const std::string ansScreen = pg + "_ans";
        Screen& s = addScreen(pg.c_str(), f.art, Dock::Center, Family::Panel, 280, 405);
        for (int i = 0; i < f.asks; ++i) {
            Button b;
            b.name = pre + "_ask" + std::to_string(i + 1);
            b.id = f.askIds[i];
            b.x = 11;
            b.y = 205 + 45 * i;
            b.w = 258; b.h = 44;
            b.action = "cbask,open:" + ansScreen;   // pick a question -> answer page
            s.buttons.push_back(b);
        }
        if (f.asks < 4) {
            Button b;
            b.name = pre + "_canc";
            b.id = 0xcf;
            b.x = 11; b.y = 340; b.w = 258; b.h = 44;
            b.action = "back";
            s.buttons.push_back(b);
        }
        Image who; who.art = "m_16\\" + pre + "_player"; who.x = 84; who.y = 60;
        s.images.push_back(who);
        // the answer page: ok / cancel at the verified y 250 / 295 + the answer art
        Screen& a = addScreen(ansScreen.c_str(), f.art, Dock::Center, Family::Panel, 280, 405);
        Button ok;  ok.name = pre + "_ok";   ok.id = 0xce; ok.x = 11; ok.y = 250;
        ok.w = 258; ok.h = 44; ok.action = "cbok"; a.buttons.push_back(ok);
        Button cc;  cc.name = pre + "_canc"; cc.id = 0xcf; cc.x = 11; cc.y = 295;
        cc.w = 258; cc.h = 44; cc.action = "back"; a.buttons.push_back(cc);
        Image me; me.art = "m_16\\" + pre + "_player"; me.x = 84; me.y = 40; a.images.push_back(me);
        // the faction's reply art (129x69), shown above the ok/cancel rows
        const char* ansArt = pre == "F5p" ? "m_16\\F5p_ans_whats"
                           : pre == "F5m" ? "m_16\\F5m_ans_hell"
                           : pre == "F5o" ? "m_16\\F5o_ans_probl"
                                          : "m_16\\F5n_ans_need";
        Image an; an.art = ansArt; an.x = 75; an.y = 150; a.images.push_back(an);
    }
    {
        // the CB set itself: the "search" and "talk" buttons of the big CB mask
        static const BtnDef kCb[] = {
            {"F5search", 0xbc, 30,  135, "cbsearch", 99, 36},
            {"F5talk",   0xbd, 137, 146, "cbtalk",   84, 84},
        };
        Screen& s = addScreen("cbradio", "m_16\\BackF5cbradio.msk", Dock::Center,
                              Family::Panel, 245, 435);
        addButtons(s, kCb, sizeof(kCb) / sizeof(kCb[0]));
    }
    {
        // the trucker-talk page (BackF5tru.msk): channel arrows + colour + send
        static const BtnDef kTalk[] = {
            {"F5tr_l",    0, 60,  250, "cbleft",  20, 30},
            {"F5tr_col",  0, 85,  250, "cbcolor", 68, 30},
            {"F5tr_r",    0, 158, 250, "cbright", 20, 30},
            {"F5tr_ok",   0, 60,  330, "cbok",    80, 30},
            {"F5tr_canc", 0, 145, 330, "back",    80, 30},
        };
        Screen& s = addScreen("cbtalk", "m_16\\BackF5tru.msk", Dock::Center,
                              Family::Panel, 280, 405);
        addButtons(s, kTalk, sizeof(kTalk) / sizeof(kTalk[0]));
        Row r; r.x = 60; r.y = 290; r.key = "cb.channel";
        s.rows.push_back(r);
    }
}

bool EscMenu::load(const std::string& dataDir) {
    buildTables();
    const std::string path = dataDir + "/COMMON/esc16.RES";
    const bool ok = res_.load(path);
    if (!ok) {
        LOG("esc: %s missing - the ESC screens cannot be drawn", path.c_str());
        return false;
    }
    // the "5 yes/no" popup (m16\MenuPopupA5.msk) and its two buttons live in the
    // sibling in-game resource (ind16.RES); a missing file only disables the popup
    const std::string ind = dataDir + "/COMMON/ind16.RES";
    if (!resInd_.load(ind))
        LOG("esc: %s missing - the 5 yes/no popup stays unavailable", ind.c_str());
    LOG("esc: %s loaded (%zu entries)", path.c_str(), res_.entries().size());
    return true;
}

// Decodes one screen's backdrop/buttons/sliders/images.  The ESC family is
// loaded eagerly by loadArt(); the panels call this on first open().
bool EscMenu::ensureArt(const std::string& screenName) {
    if (!renderer_) return false;
    Screen* s = nullptr;
    for (Screen& x : screens_)
        if (x.name == screenName) { s = &x; break; }
    if (!s) return false;

    Renderer& r = *renderer_;
    auto load = [&](const std::string& nm, int* w, int* h) -> unsigned {
        const Res::Entry* e = res_.find(nm);
        const Res* own = &res_;
        if (!e) { e = resInd_.find(nm); own = &resInd_; }
        return uploadRes(r, *own, e, w, h);
    };

    int decoded = 0, wanted = 0;
    if (!s->backdropTex) {
        ++wanted;
        s->backdropTex = load(s->backdrop, &s->backdropW, &s->backdropH);
        if (s->backdropTex) ++decoded;
    }
    for (Button& b : s->buttons) {
        if (b.invisible) continue;            // painted into the backdrop
        if (b.texA) continue;                 // already decoded
        ++wanted;
        const std::string base = "m_16\\" + b.name;
        int w = 0, h = 0;
        b.texA = load(base + "_a.txr", &w, &h);
        if (b.texA) {
            if (w > 0) b.w = w;
            if (h > 0) b.h = h;
            ++decoded;
        }
        b.texP = load(base + "_p.txr", nullptr, nullptr);
        b.texAP = load(base + "_ap.txr", nullptr, nullptr);
        b.texF = load(base + "_f.txr", nullptr, nullptr);
        b.hasP = b.texP != 0;
        b.hasAP = b.texAP != 0;
        b.hasF = b.texF != 0;
    }
    for (Slider& sl : s->sliders) {
        if (sl.texL) continue;
        ++wanted;
        sl.texL = load(sl.artL + "_a.msk", &sl.arrowW, &sl.arrowH);
        if (!sl.texL) sl.texL = load(sl.artL + "_a.txr", &sl.arrowW, &sl.arrowH);
        sl.texR = load(sl.artR + "_a.msk", nullptr, nullptr);
        if (!sl.texR) sl.texR = load(sl.artR + "_a.txr", nullptr, nullptr);
        sl.texLA = load(sl.artL + "_ap.msk", nullptr, nullptr);
        if (!sl.texLA) sl.texLA = load(sl.artL + "_ap.txr", nullptr, nullptr);
        sl.texRA = load(sl.artR + "_ap.msk", nullptr, nullptr);
        if (!sl.texRA) sl.texRA = load(sl.artR + "_ap.txr", nullptr, nullptr);
        if (sl.texL) ++decoded;
        if (!sl.texRun)
            sl.texRun = load("m_16\\runner_a.msk", &sl.runW, &sl.runH);
        if (!sl.texRunA) sl.texRunA = load("m_16\\runner_ap.msk", nullptr, nullptr);
    }
    for (Image& im : s->images) {
        if (im.tex) continue;
        ++wanted;
        im.tex = load(im.art + ".txr", &im.w, &im.h);
        if (!im.tex) im.tex = load(im.art + ".msk", &im.w, &im.h);
        if (im.tex) ++decoded;
    }
    if (s->family == Family::Panel)
        LOG("ui: art loaded (%d/%d, '%s')", decoded, wanted, s->name.c_str());
    return wanted == 0 || decoded > 0;
}

void EscMenu::loadArt(Renderer& r) {
    if (artLoaded_ || !res_.loaded()) return;
    artLoaded_ = true;
    renderer_ = &r;

    int decoded = 0, wanted = 0, screens = 0;
    for (Screen& s : screens_) {
        if (s.family != Family::Esc) continue;    // the panels load on first open
        ++screens;
        if (!s.backdropTex) {
            ++wanted;
            const Res::Entry* e = res_.find(s.backdrop);
            s.backdropTex = uploadRes(r, res_, e, &s.backdropW, &s.backdropH);
            if (s.backdropTex) ++decoded;
        }
        for (Button& b : s.buttons) {
            ++wanted;
            const std::string base = "m_16\\" + b.name;
            int w = 0, h = 0;
            b.texA = uploadRes(r, res_, res_.find(base + "_a.txr"), &w, &h);
            if (b.texA) {
                if (w > 0) b.w = w;
                if (h > 0) b.h = h;
                ++decoded;
            }
            b.texP = uploadRes(r, res_, res_.find(base + "_p.txr"), nullptr, nullptr);
            b.texAP = uploadRes(r, res_, res_.find(base + "_ap.txr"), nullptr, nullptr);
            b.texF = uploadRes(r, res_, res_.find(base + "_f.txr"), nullptr, nullptr);
            b.hasP = b.texP != 0;
            b.hasAP = b.texAP != 0;
            b.hasF = b.texF != 0;
        }
        for (Slider& sl : s.sliders) {
            ++wanted;
            sl.texL = uploadRes(r, res_, res_.find(sl.artL + "_a.msk"), &sl.arrowW, &sl.arrowH);
            sl.texR = uploadRes(r, res_, res_.find(sl.artR + "_a.msk"), nullptr, nullptr);
            sl.texLA = uploadRes(r, res_, res_.find(sl.artL + "_ap.msk"), nullptr, nullptr);
            sl.texRA = uploadRes(r, res_, res_.find(sl.artR + "_ap.msk"), nullptr, nullptr);
            if (sl.texL) ++decoded;
        }
    }
    // the shared runner knob of every slider (12x22)
    for (Screen& s : screens_)
        for (Slider& sl : s.sliders) {
            if (!sl.texRun) sl.texRun = uploadRes(r, res_, res_.find("m_16\\runner_a.msk"), &sl.runW, &sl.runH);
            if (!sl.texRunA) sl.texRunA = uploadRes(r, res_, res_.find("m_16\\runner_ap.msk"), nullptr, nullptr);
        }
    // the checkbox of the controls panel: check_a = empty, check_pp = ticked,
    // check_ap = ticked + selected
    if (!checkOff_) checkOff_ = uploadRes(r, res_, res_.find("m_16\\check_a.txr"), &checkW_, &checkH_);
    if (!checkOn_) checkOn_ = uploadRes(r, res_, res_.find("m_16\\check_pp.txr"), nullptr, nullptr);
    if (!checkOnSel_) checkOnSel_ = uploadRes(r, res_, res_.find("m_16\\check_ap.txr"), nullptr, nullptr);
    LOG("esc: art loaded (%d/%d, %d screens)", decoded, wanted, screens);
}

// ---------------------------------------------------------------------------
// The world data the panels display (read from Context, never duplicated)
// ---------------------------------------------------------------------------
void EscMenu::refresh() {
    dataDirty_ = false;
    Data d;
    const Context& c = ctx_;

    // HT2_UI_DEMO: the headless test hook (same family as HT2_SAVE_FILE /
    // HT2_LOADING_DUMP) that fills the panels with a deterministic world so
    // `--ui-shot` can verify the data cells without the App's Context.
    bool demo = false;
    if (!c.economy && !c.vehicle) {
        const char* env = getenv("HT2_UI_DEMO");
        demo = env && *env && *env != '0';
    }

    if (c.vehicle) {
        d.fuel = c.vehicle->fuel();
        d.damage = c.vehicle->damage();
        d.speed = c.vehicle->speedKmh();
        d.gear = c.vehicle->gear();
        d.odo = c.vehicle->odometer();
        d.posX = c.vehicle->x();
        d.posZ = c.vehicle->z();
    } else if (demo) {
        d.fuel = 0.42f; d.damage = 0.31f; d.speed = 62.0f; d.gear = 3;
        d.odo = 12345.0f; d.posX = -3020.0f; d.posZ = -4625.0f;
    }
    d.clock = c.clockHours;
    d.slot = c.slotText;

    if (c.economy) {
        d.money = c.economy->money();
        d.offers = c.economy->offers().size();
        d.delivered = c.economy->delivered();
        d.jobsTotal = c.economy->jobs().size();
        if (const Economy::Job* aj = c.economy->activeJob()) {
            d.hasJob = true;
            d.jobCargo = aj->cargoName;
            d.jobFrom = aj->fromTown;
            d.jobTo = aj->toTown;
            d.jobReward = aj->reward;
            d.jobDistance = aj->distance;
        }
        // the job the ЗАКАЗ / ДИСПЕТЧЕРСКАЯ panels show (nearest pickup)
        if (!c.economy->jobs().empty()) {
            std::vector<size_t> near = c.economy->jobsForPlayer(d.posX, d.posZ, 1.0e9f);
            if (!near.empty()) {
                const Economy::Job& j = c.economy->jobs()[near[0]];
                d.orderIndex = (int)near[0];
                d.orderCargo = j.cargoName;
                d.orderTo = j.toTown;
                d.orderReward = j.reward;
                d.orderDistance = j.distance;
                d.orderWeight = std::to_string(j.weight);
                char v[32];
                snprintf(v, sizeof v, "%.1f", j.volume);
                d.orderVolume = v;
                d.orderIllegal = j.illegal;
                d.jobsNear = c.economy->jobsForPlayer(d.posX, d.posZ, 250.0f).size();
                // the dispatcher board: the 10 nearest jobs
                for (size_t k = 0; k < near.size() && (int)d.table.size() < 10; ++k) {
                    const Economy::Job& jj = c.economy->jobs()[near[k]];
                    d.table.push_back({jj.cargoName, jj.fromTown, jj.toTown, fmtMoney(jj.reward)});
                }
            }
        }
        // the truck ПРОДАЁТСЯ shows: the cheapest unowned offer we can pay for
        int best = -1;
        for (size_t i = 0; i < c.economy->offers().size(); ++i) {
            const Economy::Offer& o = c.economy->offers()[i];
            if (o.owned || o.price <= 0) continue;
            if (o.price > d.money) continue;
            if (best < 0 || o.price < c.economy->offers()[best].price) best = (int)i;
        }
        if (best < 0)
            for (size_t i = 0; i < c.economy->offers().size(); ++i) {
                const Economy::Offer& o = c.economy->offers()[i];
                if (o.owned || o.price <= 0) continue;
                if (best < 0 || o.price < c.economy->offers()[best].price) best = (int)i;
            }
        // the ПРОДАЁТСЯ list: every unowned offer, cheapest first
        std::vector<size_t> order;
        for (size_t i = 0; i < c.economy->offers().size(); ++i) {
            const Economy::Offer& o = c.economy->offers()[i];
            if (o.owned || o.price <= 0) continue;
            order.push_back(i);
        }
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return c.economy->offers()[a].price < c.economy->offers()[b].price;
        });
        for (size_t i : order) {
            const Economy::Offer& o = c.economy->offers()[i];
            const std::string nm = o.display.empty() ? o.model : o.display;
            d.vehicles.push_back({nm, fmtMoney(o.price)});
            d.vehicleIdx.push_back((int)i);
        }
        d.tradeIn = c.economy->ownedValue();
        if (best < 0 && !order.empty()) best = (int)order[0];
        if (best >= 0) {
            const Economy::Offer& o = c.economy->offers()[best];
            d.saleIndex = best;
            d.saleCode = o.id;
            d.saleName = o.display.empty() ? o.model : o.display;
            d.saleClass = o.klass;
            d.salePrice = o.price;
            d.salePower = o.power;
        }
    }
    if (c.gameData) {
        d.upgrades = c.gameData->upgrades.size();
        d.vehicle = c.gameData->playerVehicle;
        if (!c.gameData->people.empty()) {
            // ИЩУ РАБОТУ: a driver from names.sch, with the cheapest truck on offer
            size_t i = c.gameData->people.empty() ? 0
                     : (size_t)((d.money / 1000) % c.gameData->people.size());
            d.hireIndex = (int)i;
            d.hireName = c.gameData->people[i].name;
            d.hireRole = c.gameData->people[i].role;
            d.hireTruck = d.saleName;
            d.hireTariff = d.orderReward;
        }
    }
    if (!c.driverName.empty()) d.driver = c.driverName;
    if (c.way && (c.vehicle || demo)) {
        // the nearest *named* node of the road network (the "road" row)
        float best = 1.0e18f;
        for (const Way::Room& room : c.way->rooms)
            for (const Way::Node& n : room.nodes) {
                if (n.name.empty()) continue;
                float dx = (float)n.pos.x - d.posX, dz = (float)n.pos.z - d.posZ;
                float dd = dx * dx + dz * dz;
                if (dd < best) { best = dd; d.road = n.name; }
            }
    }
    d.level = (int)(d.damage * 9.0f + 0.5f);
    if (c.refuelCost >= 0) d.refuelCost = c.refuelCost;
    if (c.repairCost >= 0) d.repairCost = c.repairCost;
    if (c.fuelPerLitre >= 0) d.fuelPrice = c.fuelPerLitre;
    if (c.gameData) d.upgPrice = c.gameData->upgrades;
    // the highlighted line of the ПРОДАЁТСЯ list is what the panel shows and what
    // the "buyvehicle" action buys
    if (!d.vehicleIdx.empty() && c.economy) {
        if (listSel_ < 0 || listSel_ >= (int)d.vehicleIdx.size()) listSel_ = 0;
        const int oi = d.vehicleIdx[listSel_];
        const Economy::Offer& o = c.economy->offers()[oi];
        d.saleIndex = oi;
        d.saleCode = o.id;
        d.saleName = o.display.empty() ? o.model : o.display;
        d.saleClass = o.klass;
        d.salePrice = o.price;
        d.salePower = o.power;
    }
    d.toPay = d.salePrice - d.tradeIn;
    if (d.toPay < 0) d.toPay = 0;
    if (listSel_ < 0) listSel_ = 0;

    if (demo) {
        // a deterministic sample world for --ui-shot / the tests
        d.money = 45000; d.offers = 147; d.delivered = 3; d.jobsTotal = 1110;
        d.jobsNear = 4; d.upgrades = 19; d.vehicle = "ZilR"; d.road = "Moskva";
        d.driver = "Ivanov"; d.station = "FUEL STATION";
        d.refuelCost = 1044; d.repairCost = 1860;
        d.hasJob = true;
        d.jobCargo = "Телевизор"; d.jobFrom = "Москва"; d.jobTo = "Тверь";
        d.jobReward = 4820; d.jobDistance = 1820.0f;
        d.orderIndex = 0; d.orderCargo = "Телевизор"; d.orderTo = "Тверь";
        d.orderReward = 4820; d.orderDistance = 1820.0f;
        d.orderWeight = "1200"; d.orderVolume = "6.5";
        d.saleIndex = 0; d.saleCode = "ZILR_1"; d.saleName = "ZilR";
        d.saleClass = "lorry"; d.salePrice = 18000; d.salePower = 120;
        d.fuelPrice = 9;
        d.hireIndex = 0; d.hireName = "Иванов"; d.hireRole = "driver";
        d.hireTruck = "Фиат"; d.hireTariff = 4820;
        static const char* cargo[8] = {"Телевизор", "Кирпич", "Бензин", "Мебель",
                                       "Песок", "Цемент", "Ткань", "Консервы"};
        static const char* towns[4] = {"Москва", "Тверь", "Калуга", "Тула"};
        d.table.clear();
        for (int i = 0; i < 8; ++i)
            d.table.push_back({cargo[i], towns[i % 4], towns[(i + 1) % 4],
                               fmtMoney(1200 + 700 * i)});
        // a few vehicles for the purchase list (the demo world's market)
        static const char* vn[6] = {"ZilR", "Gazelle", "Kamaz", "Volvo", "Scania", "Kenworth"};
        static const long vp[6] = {18000, 24000, 39000, 62000, 84000, 96000};
        d.vehicles.clear();
        d.vehicleIdx.clear();
        for (int i = 0; i < 6; ++i) {
            d.vehicles.push_back({vn[i], fmtMoney(vp[i])});
            d.vehicleIdx.push_back(i);
        }
        d.tradeIn = 32000;
        d.salePrice = vp[0];
        d.toPay = vp[0] > d.tradeIn ? vp[0] - d.tradeIn : 0;
        // the demo garage already owns two upgrades (shows the OK cells)
        if (bought_.empty()) { bought_.push_back("PR_OIL_ADDITIVE"); bought_.push_back("PR_ABS"); }
    }
    data_ = d;
}

std::string EscMenu::field(const std::string& key) const {
    char buf[192];
    const Data& d = data_;
    if (key.rfind("text:", 0) == 0) return key.substr(5);
    if (key.rfind("key.", 0) == 0) {                 // key.<i>.k / key.<i>.d
        const int i = atoi(key.c_str() + 4);
        const std::vector<KeyRow>& kb = keyBindings();
        if (i < 0 || i >= (int)kb.size()) return "";
        return key.back() == 'k' ? kb[i].keys : kb[i].desc;
    }
    if (key == "money") return fmtMoney(d.money);
    if (key == "fuel" || key == "fuel.pct") {
        snprintf(buf, sizeof buf, "%d%%", (int)(d.fuel * 100.0f + 0.5f));
        return buf;
    }
    if (key == "fuel.liters") {
        snprintf(buf, sizeof buf, "%d", (int)(d.fuel * kTankLitres + 0.5f));
        return buf;
    }
    if (key == "fuel.price") {
        if (d.fuelPrice < 0) return "-";
        snprintf(buf, sizeof buf, "%ld", d.fuelPrice);
        return buf;
    }
    if (key == "fuel.buy.liters" || key == "fuel.buy.cost") {
        const Slider* sl = slider("fuel-amount");
        const float amt = sl ? sl->value : 0.0f;
        const long perL = d.fuelPrice < 0 ? 0 : d.fuelPrice;
        if (key == "fuel.buy.liters") {
            snprintf(buf, sizeof buf, "%d", (int)(amt * kTankLitres + 0.5f));
            return buf;
        }
        return fmtMoney((long)(amt * kTankLitres * (float)perL + 0.5f));
    }
    if (key == "sale.tradein") return fmtMoney(d.tradeIn);
    if (key == "sale.topay") return fmtMoney(d.toPay);
    if (key.rfind("upg.", 0) == 0) {                      // a tuning price cell
        const std::string id = key.substr(4);
        if (upgradeBought(id)) return "OK";
        auto it = d.upgPrice.find(id);
        if (it == d.upgPrice.end()) return "-";
        return fmtMoney(it->second);
    }
    if (key == "fuel.cost" || key == "price.fuel")
        return d.refuelCost < 0 ? "-" : fmtMoney(d.refuelCost);
    if (key == "price.repair")
        return d.repairCost < 0 ? "-" : fmtMoney(d.repairCost);
    if (key == "damage") {
        snprintf(buf, sizeof buf, "%d%%", (int)(d.damage * 100.0f + 0.5f));
        return buf;
    }
    if (key == "clock") {
        int hh = (int)d.clock, mm = (int)((d.clock - hh) * 60.0f + 0.5f);
        if (mm > 59) mm = 59;
        snprintf(buf, sizeof buf, "%02d:%02d", hh, mm);
        return buf;
    }
    if (key == "speed") { snprintf(buf, sizeof buf, "%d", (int)(d.speed + 0.5f)); return buf; }
    if (key == "gear") {
        if (d.gear == 0) return "N";
        if (d.gear < 0) return "R";
        snprintf(buf, sizeof buf, "%d", d.gear);
        return buf;
    }
    if (key == "level") { snprintf(buf, sizeof buf, "P%d", d.level); return buf; }
    if (key == "pos") {
        snprintf(buf, sizeof buf, "%d/%d", (int)d.posX, (int)d.posZ);
        return buf;
    }
    if (key == "odo") { snprintf(buf, sizeof buf, "%d", (int)(d.odo + 0.5f)); return buf; }
    if (key == "vehicle") return d.vehicle.empty() ? "-" : d.vehicle;
    if (key == "driver") return d.driver.empty() ? "-" : d.driver;
    if (key == "road") return d.road.empty() ? "-" : d.road;
    if (key == "station") return d.station;
    if (key == "jobs") { snprintf(buf, sizeof buf, "%zu", d.jobsTotal); return buf; }
    if (key == "jobs.near") { snprintf(buf, sizeof buf, "%zu", d.jobsNear); return buf; }
    if (key == "offers") { snprintf(buf, sizeof buf, "%zu", d.offers); return buf; }
    if (key == "delivered") { snprintf(buf, sizeof buf, "%zu", d.delivered); return buf; }
    if (key == "upgrades") {
        snprintf(buf, sizeof buf, "%zu", d.upgrades);
        return buf;
    }
    if (key == "slot") return slotText_.empty() ? d.slot : slotText_;
    if (key == "job.status") return d.hasJob ? "ACTIVE" : "NONE";
    if (key == "job.cargo") return d.hasJob ? d.jobCargo : "-";
    if (key == "job.from") return d.hasJob ? d.jobFrom : "-";
    if (key == "job.to") return d.hasJob ? d.jobTo : "-";
    if (key == "job.reward") return d.hasJob ? fmtMoney(d.jobReward) : "-";
    if (key == "job.distance") {
        if (!d.hasJob) return "-";
        snprintf(buf, sizeof buf, "%d", (int)(d.jobDistance + 0.5f));
        return buf;
    }
    if (key == "order.cargo") return d.orderIndex < 0 ? "-" : d.orderCargo;
    if (key == "order.to") return d.orderIndex < 0 ? "-" : d.orderTo;
    if (key == "order.premium") return d.orderIndex < 0 ? "-" : fmtMoney(d.orderReward);
    if (key == "order.time") {
        if (d.orderIndex < 0) return "-";
        snprintf(buf, sizeof buf, "%d", (int)(d.orderDistance / 16.7f + 1.0f));   // minutes
        return buf;
    }
    if (key == "order.weight") return d.orderIndex < 0 ? "-" : d.orderWeight;
    if (key == "order.volume") return d.orderIndex < 0 ? "-" : d.orderVolume;
    if (key == "order.fragile") return d.orderIllegal ? "НЕТ" : "ДА";
    if (key == "sale.code") return d.saleIndex < 0 ? "-" : d.saleCode;
    if (key == "sale.price") return d.saleIndex < 0 ? "-" : fmtMoney(d.salePrice);
    if (key == "sale.power") {
        if (d.saleIndex < 0) return "-";
        snprintf(buf, sizeof buf, "%ld", d.salePower);
        return buf;
    }
    if (key == "sale.capacity") return d.saleIndex < 0 ? "-" : d.saleClass;
    if (key == "hire.name") return d.hireIndex < 0 ? "-" : d.hireName;
    if (key == "hire.tariff") return d.hireIndex < 0 ? "-" : fmtMoney(d.hireTariff);
    if (key == "hire.truck") return d.hireIndex < 0 ? "-" : d.hireTruck;
    if (key == "hire.price") return d.saleIndex < 0 ? "-" : fmtMoney(d.salePrice);
    if (key == "hire.power") {
        if (d.saleIndex < 0) return "-";
        snprintf(buf, sizeof buf, "%ld", d.salePower);
        return buf;
    }
    if (key == "hire.capacity") return d.saleIndex < 0 ? "-" : d.saleClass;
    if (key == "cb.channel") { snprintf(buf, sizeof buf, "CH %d", cbChannel_); return buf; }
    if (key == "ask") return askText_;
    return "-";
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------
const EscMenu::Screen* EscMenu::find(const std::string& name) const {
    for (const Screen& s : screens_)
        if (s.name == name) return &s;
    return nullptr;
}

const EscMenu::Screen* EscMenu::current() const { return find(cur_); }

int EscMenu::panelCount() const {
    int n = 0;
    for (const Screen& s : screens_)
        if (s.family == Family::Panel) ++n;
    return n;
}

bool EscMenu::panelOpen() const {
    const Screen* s = current();
    return active_ && s && s->family == Family::Panel;
}

float EscMenu::panelX(int screenW) const {
    const Screen* s = current();
    if (!s) return 0.0f;
    switch (s->dock) {
        case Dock::Right225: return (float)screenW - 224.0f;
        case Dock::Right385: return (float)screenW - 383.0f;
        case Dock::Right640: return (float)screenW - 640.0f;
        case Dock::Center:   return ((float)screenW - (float)s->backdropW) * 0.5f;
        case Dock::Left:
        default:             return 0.0f;
    }
}

bool EscMenu::screenRect(float& x, float& y, float& w, float& h) const {
    const Screen* s = current();
    if (!s || !active_) return false;
    x = panelX(vpW_);
    y = panelTop(vpH_);
    w = (float)s->backdropW;
    h = (float)s->backdropH;
    return true;
}

void EscMenu::buildNav() {
    nav_.clear();
    const Screen* s = current();
    if (!s) { sel_ = -1; return; }
    for (size_t i = 0; i < s->sliders.size(); ++i) nav_.push_back(Nav{true, (int)i});
    if (s->keyList) nav_.push_back(Nav{false, -1});    // the AUTO GEARBOX checkbox
    for (size_t i = 0; i < s->buttons.size(); ++i)
        if (!s->buttons[i].disabled) nav_.push_back(Nav{false, (int)i});
    sel_ = firstEnabled();
}

int EscMenu::firstEnabled() const { return nav_.empty() ? -1 : 0; }

void EscMenu::open(const std::string& screenName) {
    const Screen* s = find(screenName);
    if (!s) {
        LOG("esc: unknown screen '%s'", screenName.c_str());
        return;
    }
    cur_ = s->name;
    active_ = true;
    if (dataDirty_) refresh();
    // the fuel panel's amount scale starts at "fill the tank"
    if (s->name == "fuel") {
        const float need = 1.0f - data_.fuel;
        setSliderValue("fuel-amount", need < 0.0f ? 0.0f : need);
    }
    if (s->family == Family::Panel) ensureArt(s->name);
    buildNav();
    const Screen* c = current();
    if (c && c->family == Family::Panel) {
        LOG("ui: panel '%s' dock=%s art %dx%d viewport %dx%d anchor (%d,%d): %zu buttons, "
            "%zu sliders, %zu rows%s",
            cur_.c_str(), dockName(c->dock), c->backdropW, c->backdropH, vpW_, vpH_,
            (int)panelX(vpW_), (int)panelTop(vpH_), c->buttons.size(), c->sliders.size(),
            c->rows.size(), c->tableRows ? " + table" : "");
        for (const Row& r : c->rows)
            if (r.key.rfind("key.", 0) != 0)
                LOG("ui:   row %s = '%s'", r.key.c_str(), field(r.key).c_str());
        for (const Cell& c2 : c->cells)
            LOG("ui:   cell %s = '%s'", c2.key.c_str(), field(c2.key).c_str());
        if (c->tableRows && c->tableKind == TableKind::Vehicles)
            LOG("ui:   list %zu offers (%zu shown), highlight %d",
                data_.vehicles.size(), (size_t)c->tableRows, listSel_ + 1);
        else if (c->tableRows)
            LOG("ui:   table %zu rows x %zu cols", data_.table.size(), c->tableCols.size());
    } else {
        LOG("esc: screen '%s' (%zu buttons, %zu sliders)%s", cur_.c_str(),
            c ? c->buttons.size() : 0, c ? c->sliders.size() : 0,
            c && c->keyList ? " + key list" : "");
        if (c && c->keyList)
            LOG("esc: key bindings %zu rows (docs/original_ui.md 3.1)", keyBindings().size());
    }
}

void EscMenu::selectNext(int delta) {
    if (nav_.empty()) return;
    const int n = (int)nav_.size();
    sel_ = ((sel_ + delta) % n + n) % n;
}

const EscMenu::Button* EscMenu::selectedButton() const {
    const Screen* s = current();
    if (!s || sel_ < 0 || sel_ >= (int)nav_.size() || nav_[sel_].slider) return nullptr;
    if (nav_[sel_].index < 0) return nullptr;          // the checkbox row
    return &s->buttons[nav_[sel_].index];
}

const EscMenu::Slider* EscMenu::selectedSlider() const {
    const Screen* s = current();
    if (!s || sel_ < 0 || sel_ >= (int)nav_.size() || !nav_[sel_].slider) return nullptr;
    return &s->sliders[nav_[sel_].index];
}

const EscMenu::Slider* EscMenu::slider(const std::string& name) const {
    for (const Screen& s : screens_)
        for (const Slider& sl : s.sliders)
            if (sl.name == name) return &sl;
    return nullptr;
}

float EscMenu::sliderValue(const std::string& name) const {
    const Slider* sl = slider(name);
    return sl ? sl->value : 0.0f;
}

int EscMenu::buttonId(const std::string& name) const {
    for (const Screen& s : screens_)
        for (const Button& b : s.buttons)
            if (b.name == name) return b.id;
    return 0;
}

void EscMenu::setSliderValue(const std::string& name, float v) {
    for (Screen& s : screens_)
        for (Slider& sl : s.sliders)
            if (sl.name == name) sl.value = std::max(0.0f, std::min(1.0f, v));
}

void EscMenu::adjustSelected(int delta) {
    if (sel_ >= 0 && sel_ < (int)nav_.size() && !nav_[sel_].slider && nav_[sel_].index < 0) {
        autoGear_ = autoGear_ > 0.5f ? 0.0f : 1.0f;    // the AUTO GEARBOX checkbox
        LOG("esc: setting 'auto-gearbox' -> %s", autoGear_ > 0.5f ? "on" : "off");
        return;
    }
    Slider* sl = const_cast<Slider*>(selectedSlider());
    if (!sl) return;
    const float v = std::max(0.0f, std::min(1.0f, sl->value + (float)delta * 0.05f));
    if (v == sl->value) return;
    sl->value = v;
    LOG("esc: slider '%s' -> %.2f", sl->label.c_str(), v);
}

std::vector<const EscMenu::Cell*> EscMenu::upgradeCells() const {
    std::vector<const Cell*> out;
    const Screen* s = current();
    if (!s) return out;
    for (const Cell& c : s->cells)
        if (c.key.rfind("upg.", 0) == 0) out.push_back(&c);
    return out;
}

int EscMenu::upgradeIndex(const std::string& id) const {
    const std::vector<const Cell*> up = upgradeCells();
    for (size_t i = 0; i < up.size(); ++i)
        if (up[i]->key == "upg." + id) return (int)i;
    return -1;
}

const std::string& EscMenu::selectedUpgrade() const {
    static const std::string none;
    const std::vector<const Cell*> up = upgradeCells();
    if (up.empty()) return none;
    int i = listSel_;
    if (i < 0 || i >= (int)up.size()) i = 0;
    // the returned reference is a temporary key of the cell: keep the caller's
    // use short (the App calls it right after activateName)
    static std::string cur;
    cur = up[i]->key.substr(4);
    return cur;
}

long EscMenu::selectedUpgradePrice() const {
    const std::string id = selectedUpgrade();
    if (id.empty()) return 0;
    auto it = data_.upgPrice.find(id);
    return it == data_.upgPrice.end() ? 0 : it->second;
}

void EscMenu::markUpgradeBought(const std::string& id) {
    if (id.empty()) return;
    for (const std::string& b : bought_)
        if (b == id) return;
    bought_.push_back(id);
    LOG("ui: upgrade '%s' installed", id.c_str());
}

std::vector<std::pair<std::string, long>> EscMenu::upgradeList() const {
    std::vector<std::pair<std::string, long>> out;
    for (const Cell* c : upgradeCells()) {
        const std::string id = c->key.substr(4);
        auto it = data_.upgPrice.find(id);
        out.push_back({id, it == data_.upgPrice.end() ? 0 : it->second});
    }
    return out;
}

bool EscMenu::upgradeBought(const std::string& id) const {
    for (const std::string& b : bought_)
        if (b == id) return true;
    return false;
}

void EscMenu::moveSelection(int delta) {
    const Screen* s = current();
    if (!s) return;
    int n = 0;
    if (s->tableKind == TableKind::Vehicles) n = (int)data_.vehicles.size();
    else n = (int)upgradeCells().size();
    if (n <= 0) return;
    listSel_ = ((listSel_ + delta) % n + n) % n;
    LOG("ui: selection %d/%d on '%s'", listSel_ + 1, n, s->name.c_str());
}

std::string EscMenu::runAction(const std::string& action) {
    // "open:<screen>" navigates and is also *returned* when the button has no
    // other action (the App logs it and ignores it); a comma-separated list may
    // add a terminal action ("cbask,open:cbpol_ans" -> "cbask"), and that
    // terminal token wins.
    std::string terminal, nav;
    size_t pos = 0;
    while (pos <= action.size()) {
        size_t comma = action.find(',', pos);
        std::string tok = action.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = (comma == std::string::npos) ? action.size() + 1 : comma + 1;
        if (tok.empty()) continue;
        if (tok.rfind("open:", 0) == 0) {          // screen navigation, handled here
            open(tok.substr(5));
            nav = tok;
            continue;
        }
        if (tok == "back" || tok == "resume") {
            if (panelOpen()) {                     // a panel: cancel resumes driving
                active_ = false;
                return "resume";
            }
            if (cur_ == "main") { active_ = false; return "resume"; }
            open("main");
            return "back";
        }
        if (tok == "yes" || tok == "no") {         // the 5 yes/no popup
            const std::string yes = askAction_;
            askAction_.clear();
            const std::string ret = askReturn_;
            askReturn_.clear();
            if (!ret.empty() && ret != cur_) open(ret);
            else if (ret.empty()) active_ = false;
            return tok == "yes" ? (yes.empty() ? "yes" : yes) : "no";
        }
        if (tok == "listup" || tok == "listdown") { // the list's scroll arrows
            moveSelection(tok == "listup" ? -1 : 1);
            continue;
        }
        if (tok == "listrun") {                    // the list's own OK
            const Screen* s = current();
            if (s && s->tableKind == TableKind::Vehicles) return "buyvehicle";
            return "tune";
        }
        if (tok == "cbleft" || tok == "cbright") { // the CB channel arrows
            cbChannel_ = std::max(1, std::min(40, cbChannel_ + (tok == "cbright" ? 1 : -1)));
            LOG("ui: CB channel %d", cbChannel_);
            continue;
        }
        if (tok == "cbcolor" || tok == "cbok" || tok == "cbsearch" || tok == "cbtalk")
            return tok;
        if (terminal.empty()) terminal = tok;      // a terminal action for the App
    }
    return terminal.empty() ? nav : terminal;
}

std::string EscMenu::runButton(int index) {
    const Screen* s = current();
    if (!s || index < 0 || index >= (int)s->buttons.size()) return "";
    const Button& b = s->buttons[index];
    lastButton_ = b.name;
    return runAction(b.action);
}

std::string EscMenu::actionForNav(int navIndex) {
    const Screen* s = current();
    if (!s || navIndex < 0 || navIndex >= (int)nav_.size()) return "";
    if (nav_[navIndex].slider) return "";      // sliders are adjusted, not activated
    if (nav_[navIndex].index < 0) {            // the AUTO GEARBOX checkbox
        autoGear_ = autoGear_ > 0.5f ? 0.0f : 1.0f;
        LOG("esc: setting 'auto-gearbox' -> %s", autoGear_ > 0.5f ? "on" : "off");
        return "";
    }
    return runButton(nav_[navIndex].index);
}

void EscMenu::activateSelected() {
    const std::string a = actionForNav(sel_);
    if (!a.empty()) activated_ = a;
}

std::string EscMenu::activateName(const std::string& name) {
    const Screen* s = current();
    if (!s) return "";
    for (size_t i = 0; i < s->buttons.size(); ++i)
        if (s->buttons[i].name == name) return runButton((int)i);
    return "";
}

std::string EscMenu::activateId(int id) {
    const Screen* s = current();
    if (!s) return "";
    for (size_t i = 0; i < s->buttons.size(); ++i)
        if (s->buttons[i].id == id) return runButton((int)i);
    return "";
}

std::string EscMenu::back() {
    if (!active_) return "";
    if (panelOpen()) { active_ = false; return "resume"; }   // panels: Esc resumes
    if (cur_ != "main") { open("main"); return "back"; }
    active_ = false;
    return "resume";
}

std::string EscMenu::hotkey(int key) const {
    switch (key) {
        case SDLK_F1: return "f1";
        case SDLK_F2: return "f2";
        case SDLK_F3: return "f3";
        case SDLK_F4: return "mytruck";
        case SDLK_F5: return "cbradio";
        default:      return "";
    }
}

void EscMenu::ask(const std::string& question, const std::string& yesAction) {
    askText_ = question;
    askAction_ = yesAction;
    askReturn_ = cur_;
    open("popup5");
}

std::string EscMenu::takeActivated() {
    const std::string s = activated_;
    activated_.clear();
    return s;
}

void EscMenu::update(float dt) {
    time_ += dt;
    if (pressT_ > 0.0f) {
        pressT_ -= dt;
        if (pressT_ <= 0.0f) pressed_.clear();
    }
}

void EscMenu::handleKey(int key) {
    switch (key) {
        case SDLK_UP:    selectNext(-1); break;
        case SDLK_DOWN:  selectNext(1); break;
        case SDLK_LEFT:  adjustSelected(-1); break;
        case SDLK_RIGHT: adjustSelected(1); break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE: activateSelected(); break;
        case SDLK_ESCAPE: {
            const std::string a = back();
            if (!a.empty()) activated_ = a;
            break;
        }
        default: {
            // F1/F2/F3/F5 swap between the in-game panels while one is open
            const std::string h = hotkey(key);
            if (!h.empty()) { open(h); break; }
            break;
        }
    }
}

void EscMenu::handleMouse(float mx, float my, bool click, int vpW, int vpH) {
    vpW_ = vpW; vpH_ = vpH;
    if (!click && mx == mouseX_ && my == mouseY_) return;
    mouseX_ = mx; mouseY_ = my;
    const Screen* s = current();
    if (!s) return;
    const float ax = panelX(vpW_), ay = panelTop(vpH_);
    for (int i = 0; i < (int)nav_.size(); ++i) {
        if (nav_[i].slider) {
            const Slider& sl = s->sliders[nav_[i].index];
            if (my < ay + sl.trackY - 22.0f || my > ay + sl.trackY + 22.0f) continue;
            if (mx < ax + sl.trackX0 - 24.0f || mx > ax + sl.trackX1 + 24.0f) continue;
            sel_ = i;
            if (click) {
                const float v = (mx - ax - (float)sl.trackX0) /
                                (float)(sl.trackX1 - sl.trackX0);
                setSliderValue(sl.label, v);
                LOG("esc: slider '%s' -> %.2f", sl.label.c_str(), sliderValue(sl.label));
            }
            return;
        }
        const Button* b = nav_[i].index >= 0 ? &s->buttons[nav_[i].index] : nullptr;
        float bx = 0, by = 0, bw = 0, bh = 0;
        if (b) { bx = (float)b->x; by = (float)b->y; bw = (float)b->w; bh = (float)b->h; }
        else { bx = 150.0f; by = 716.0f; bw = (float)checkW_; bh = (float)checkH_; }
        if (mx < ax + bx || mx > ax + bx + bw) continue;
        if (my < ay + by || my > ay + by + bh) continue;
        sel_ = i;
        if (click) {
            if (b) { pressed_ = b->name; pressT_ = 0.12f; }
            const std::string a = actionForNav(i);        // may rebuild nav_
            if (!a.empty()) activated_ = a;
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
void EscMenu::draw(Renderer& r, Font& font) {
    if (!active_) return;
    vpW_ = r.width_;
    vpH_ = r.height_;
    renderer_ = &r;
    if (dataDirty_) refresh();
    Screen* sm = nullptr;
    for (Screen& x : screens_)
        if (x.name == cur_) { sm = &x; break; }
    if (!sm) return;
    if (sm->family == Family::Panel && !sm->backdropTex) ensureArt(sm->name);
    const Screen& s = *sm;

    const float ax = panelX(vpW_);
    const float ay = panelTop(vpH_);
    r.begin2D();
    const unsigned white = r.whiteTex();

    // 1) the 1200-px panel, 1:1 at its verified anchor (clipped by the viewport)
    if (s.backdropTex)
        r.quad2D(s.backdropTex, ax, ay, (float)s.backdropW, (float)s.backdropH,
                 1, 1, 1, 1, 0, 0, 1, 1);
    else
        r.quad2D(white, ax, ay, (float)s.backdropW, (float)s.backdropH, 0.05f, 0.07f, 0.10f, 0.90f);

    // 2) the panel's pictures (portraits, the service truck, ...)
    for (const Image& im : s.images)
        if (im.tex)
            r.quad2D(im.tex, ax + (float)im.x, ay + (float)im.y, (float)im.w, (float)im.h,
                     1, 1, 1, 1, 0, 0, 1, 1);

    // 3) the question of the "5 yes/no" popup (the art itself is generic)
    if (s.question && !askText_.empty())
        font.drawCentered(r, askText_, ax + (float)s.backdropW * 0.5f, ay + 40.0f, 1.5f,
                          1.0f, 0.95f, 0.75f, 1.0f);

    // 4) the checkbox row of the controls panel (m_16\check_*.txr, 19x17):
    //    "АВТОМАТИЧЕСКАЯ КОРОБКА ПЕРЕДАЧ" is the only painted row without a track
    if (s.keyList) {
        const float bx = ax + 150.0f, by = ay + 716.0f;
        const bool cbSel = (sel_ >= 0 && sel_ < (int)nav_.size() &&
                            !nav_[sel_].slider && nav_[sel_].index < 0);
        unsigned t = checkOff_;
        if (autoGear_ > 0.5f) t = cbSel && checkOnSel_ ? checkOnSel_ : checkOn_;
        if (t)
            r.quad2D(t, bx, by, (float)checkW_, (float)checkH_, 1, 1, 1, 1, 0, 0, 1, 1);
        else {
            r.quad2D(white, bx, by, 19.0f, 17.0f, 0.05f, 0.07f, 0.10f, 0.85f);
            r.quad2D(white, bx + 2.0f, by + 2.0f, 15.0f, 2.0f, 0.95f, 0.95f, 0.6f, 0.95f);
            r.quad2D(white, bx + 2.0f, by + 13.0f, 15.0f, 2.0f, 0.95f, 0.95f, 0.6f, 0.95f);
            r.quad2D(white, bx + 2.0f, by + 2.0f, 2.0f, 13.0f, 0.95f, 0.95f, 0.6f, 0.95f);
            r.quad2D(white, bx + 15.0f, by + 2.0f, 2.0f, 13.0f, 0.95f, 0.95f, 0.6f, 0.95f);
        }
    }

    // 5) sliders: arrow pair at the ends of the painted track + the runner knob
    for (size_t i = 0; i < s.sliders.size(); ++i) {
        const Slider& sl = s.sliders[i];
        const bool on = (selectedSlider() == &sl);
        const float arrowY = ay + (float)sl.trackY - sl.arrowH * 0.5f;
        const float arrowLx = ax + (float)sl.trackX0 - sl.arrowW;
        const float arrowRx = ax + (float)sl.trackX1;
        if (sl.texL)
            r.quad2D(on && sl.texLA ? sl.texLA : sl.texL, arrowLx, arrowY,
                     (float)sl.arrowW, (float)sl.arrowH, 1, 1, 1, 1, 0, 0, 1, 1);
        if (sl.texR)
            r.quad2D(on && sl.texRA ? sl.texRA : sl.texR, arrowRx, arrowY,
                     (float)sl.arrowW, (float)sl.arrowH, 1, 1, 1, 1,
                     sl.flipR ? 1.0f : 0.0f, 0, sl.flipR ? 0.0f : 1.0f, 1);
        const float rx = ax + (float)sl.trackX0 +
                         sl.value * (float)(sl.trackX1 - sl.trackX0) - sl.runW * 0.5f;
        const float ry = ay + (float)sl.trackY - sl.runH * 0.5f;
        if (sl.texRun)
            r.quad2D(on && sl.texRunA ? sl.texRunA : sl.texRun, rx, ry,
                     (float)sl.runW, (float)sl.runH, 1, 1, 1, 1, 0, 0, 1, 1);
        else
            r.quad2D(white, rx, ry, 6.0f, 14.0f, 0.9f, 0.9f, 0.5f, 1);
    }

    // 6) buttons — the Russian caption is inside the art; _ap = selected,
    //    _p = just clicked, _f = forbidden (network game only, never set here)
    for (const Button& b : s.buttons) {
        if (b.invisible) continue;            // the art is already in the backdrop
        const bool on = (selectedButton() == &b);
        unsigned t = b.texA;
        if (b.disabled && b.texF) t = b.texF;
        else if (pressT_ > 0.0f && pressed_ == b.name && b.texP) t = b.texP;
        else if (on && b.texAP) t = b.texAP;
        const float bx = ax + (float)b.x, by = ay + (float)b.y;
        if (t) {
            r.quad2D(t, bx, by, (float)b.w, (float)b.h, 1, 1, 1, 1, 0, 0, 1, 1);
        } else {
            r.quad2D(white, bx, by, (float)b.w, (float)b.h, 0.10f, 0.14f, 0.22f, 0.55f);
            r.quad2D(white, bx, by, (float)b.w, 2.0f, 1, 0.85f, 0.3f, 1);
            r.quad2D(white, bx, by + b.h - 2.0f, (float)b.w, 2.0f, 1, 0.85f, 0.3f, 1);
        }
    }

    // 7) the data rows / the LED + price cells / the tables / the key list
    drawRows(r, font, s, ax, ay);
    drawCells(r, font, s, ax, ay);
    drawTable(r, font, s, ax, ay);
    if (s.keyList) drawKeyList(r, font, ax, ay);
    if (s.slotList) drawSlotList(r, font, ax, ay);

    // 8) a transient notice (the App's response to an action)
    if (ctx_.noticeTimer > 0.0f && !ctx_.notice.empty()) {
        const float cx = ax + (float)s.backdropW * 0.5f;
        font.drawCentered(r, ctx_.notice, cx, ay + 24.0f, 1.4f, 1.0f, 0.85f, 0.4f, 1.0f);
    }

    r.end2D();
}

void EscMenu::drawRows(Renderer& r, Font& font, const Screen& s, float ax, float ay) {
    if (s.rows.empty() || !font.ready()) return;
    for (const Row& row : s.rows) {
        const std::string t = field(row.key);
        if (t.empty()) continue;
        // the panel's value cells are light grey; money-like rows get the amber
        // ink of the original's LED readouts
        const bool amber = (row.key == "money" || row.key.rfind("price.", 0) == 0 ||
                            row.key.rfind("order.premium", 0) == 0 ||
                            row.key.rfind("job.reward", 0) == 0 ||
                            row.key.rfind("sale.price", 0) == 0 ||
                            row.key.rfind("hire.tariff", 0) == 0 ||
                            row.key == "fuel.cost");
        if (amber)
            font.draw(r, t, ax + (float)row.x, ay + (float)row.y, row.scale,
                      1.0f, 0.86f, 0.35f, 1.0f);
        else
            font.draw(r, t, ax + (float)row.x, ay + (float)row.y, row.scale,
                      0.93f, 0.95f, 0.98f, 1.0f);
    }
}

// The painted value cells: the 7-segment LED windows of the fuel panel and the
// price cells of the service panel's upgrade grid.  The highlighted cell of a
// list (the tuning grid, the vehicle list) gets a box + the original's 12x22
// "runner" marker.
void EscMenu::drawCells(Renderer& r, Font& font, const Screen& s, float ax, float ay) {
    if (s.cells.empty() || !font.ready()) return;
    const unsigned white = r.whiteTex();
    const std::vector<const Cell*> up = upgradeCells();
    const Cell* sel = nullptr;
    if (!up.empty()) {
        int i = listSel_;
        if (i < 0 || i >= (int)up.size()) i = 0;
        sel = up[i];
    }
    for (const Cell& c : s.cells) {
        const std::string t = field(c.key);
        const float x = ax + (float)c.x, y = ay + (float)c.y;
        if (&c == sel) {
            // highlight the cell being edited (1-px amber frame like the art)
            const float w = (float)(c.w ? c.w : 58), h = (float)(c.h ? c.h : 16);
            r.quad2D(white, x - 3, y - 2, w + 6, 1, 1, 0.85f, 0.3f, 0.9f);
            r.quad2D(white, x - 3, y + h + 1, w + 6, 1, 1, 0.85f, 0.3f, 0.9f);
            r.quad2D(white, x - 3, y - 2, 1, h + 4, 1, 0.85f, 0.3f, 0.9f);
            r.quad2D(white, x + w + 2, y - 2, 1, h + 4, 1, 0.85f, 0.3f, 0.9f);
        }
        if (t.empty() || t == "-") continue;
        const float sc = c.scale;
        if (c.money) font.draw(r, t, x, y, sc, 1.0f, 0.86f, 0.35f, 1.0f);
        else         font.draw(r, t, x, y, sc, 0.93f, 0.95f, 0.98f, 1.0f);
    }
}

void EscMenu::drawTable(Renderer& r, Font& font, const Screen& s, float ax, float ay) {
    if (s.tableRows <= 0 || s.tableCols.empty() || !font.ready()) return;
    const bool vehicles = (s.tableKind == TableKind::Vehicles);
    const std::vector<std::vector<std::string>>& rows = vehicles ? data_.vehicles : data_.table;
    // the list is windowed on the highlighted line (the market list can hold 147)
    int off = 0;
    if (vehicles) {
        const int n = (int)rows.size();
        off = listSel_ - s.tableRows + 1;
        const int maxOff = n > s.tableRows ? n - s.tableRows : 0;
        if (off < 0) off = 0;
        if (off > maxOff) off = maxOff;
    }
    const unsigned white = r.whiteTex();
    for (size_t row = 0; row < rows.size() && (int)row < s.tableRows; ++row) {
        const int abs = (int)row + off;
        if (abs < 0 || abs >= (int)rows.size()) continue;
        const std::vector<std::string>& cells = rows[abs];
        const float y = ay + (float)s.tableY + (float)s.tablePitch * (float)row;
        if (vehicles && abs == listSel_) {           // the highlighted purchase line
            r.quad2D(white, ax + (float)s.tableCols[0] - 16.0f, y - 2.0f, 176.0f, 17.0f,
                     1.0f, 0.85f, 0.30f, 0.22f);
            if (s.images.empty()) { /* no art needed for the marker */ }
            r.quad2D(white, ax + (float)s.tableCols[0] - 8.0f, y + 1.0f, 4.0f, 11.0f,
                     1.0f, 0.85f, 0.30f, 1.0f);
        }
        for (size_t c = 0; c < s.tableCols.size() && c < cells.size(); ++c) {
            if (cells[c].empty()) continue;
            const float x = ax + (float)s.tableCols[c];
            const bool last = (c + 1 == s.tableCols.size());
            if (last || (vehicles && c == 1))
                font.draw(r, cells[c], x, y, 1.15f, 1.0f, 0.86f, 0.35f, 1.0f);
            else
                font.draw(r, cells[c], x, y, 1.15f, 0.93f, 0.95f, 0.98f, 1.0f);
        }
    }
}

// The original key list lives on the front-end controls page; the in-game
// BackEscCtrl panel is the 3-slider settings panel, so the list is drawn beside
// it (see docs/esc_impl.md).  The authentic in-game list is the BackF1 page.
void EscMenu::drawKeyList(Renderer& r, Font& font, float ax, float ay) {
    (void)ay;
    const float bx = ax + 224.0f;
    const float bw = std::min(420.0f, (float)vpW_ - bx - 12.0f);
    if (bw < 190.0f || !font.ready()) return;
    const float rows = (float)keyBindings().size();
    const float bh = rows * 15.0f + 34.0f;
    const float by = std::max(6.0f, ((float)vpH_ - bh) * 0.5f);
    const unsigned white = r.whiteTex();
    r.quad2D(white, bx, by, bw, bh, 0.03f, 0.05f, 0.09f, 0.82f);
    font.draw(r, "KEY BINDINGS", bx + 12.0f, by + 8.0f, 1.4f, 1.0f, 0.9f, 0.4f, 1.0f);
    float y = by + 24.0f;
    for (const KeyRow& k : keyBindings()) {
        font.draw(r, k.keys, bx + 12.0f, y, 1.15f, 1.0f, 1.0f, 0.85f, 1.0f);
        font.draw(r, k.desc, bx + 104.0f, y, 1.15f, 0.82f, 0.88f, 0.95f, 1.0f);
        y += 15.0f;
    }
}

void EscMenu::drawSlotList(Renderer& r, Font& font, float ax, float ay) {
    if (!font.ready()) return;
    // the panel art paints the slot box from panel y ~510 to ~670
    const std::string line = slotText_.empty() ? "SLOT 1: EMPTY" : slotText_;
    font.draw(r, line, ax + 28.0f, ay + 524.0f, 1.15f, 1.0f, 1.0f, 0.9f, 1.0f);
}

} // namespace ht2

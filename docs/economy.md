# Player economy — money, vehicle market, cargo jobs

`src/game/Economy.{h,cpp}` owns the player's money, the bazaar vehicle market and
the cargo transport jobs.  Everything is derived from the shipped scripts
(`evehicles.sch`, `merchandise.sch`, `vd.sch`, `ENV/GAME.CNF`, the `town` and
`parking` records of `ENV/mod_*.sch`) plus the `.way` node table — no tables are
hard-coded here, and no positions are invented: a job whose pickup or drop-off
name is not in the `.way` table is **dropped and counted**, never placed at (0,0).

Contents: [1 wiring](#1-wiring) · [2 evehicles.sch fields](#2-evehicles-sch-field-layout-and-the-price-question)
· [3 jobs](#3-cargo-jobs) · [4 dealers](#4-car-dealers) · [5 numbers](#5-numbers-on-the-shipped-data)
· [6 API](#6-api) · [7 verification](#7-verification) · [8 open items](#8-what-is-not-resolved)

---

## 1. Wiring

The scripts only contain *names* of `.way` nodes; the coordinates live in
`ENV/<tile>.way` (`RNOD`/`POSN`), already indexed once by the world owner
(`App::indexWayNodes` → `App::wayNodes_`, `std::map<std::string, Way::Node>`).
The intended call order is:

```cpp
economy_.setWayNodes(wayNodes_);   // every named node of every tile
economy_.init(gameData_);          // catalogue, dealers, jobs, positions
```

`init()` may be called again (e.g. once the node table exists): the rebuild keeps
`money()`, `delivered()` and which job is active / done.  `setWayNodes()` after
`init()` immediately re-resolves the jobs that were already built (a node that
appears becomes usable, one that disappears drops those jobs and counts them);
towns that only become resolvable later need another `init()`, which is safe.

Per frame the parent only needs:

```cpp
economy_.activeJob();                                  // HUD
economy_.deliver(x, z, 8.0f, &reward);                 // every tick, true on arrival
size_t i = economy_.jobsNear(x, z, 40.0f).front();     // at a warehouse
economy_.accept(i);
int d = economy_.dealerNear(x, z, 25.0f);              // at a bazaar stall
if (d >= 0 && economy_.buy(index)) respawnTruck(economy_.playerModel(), economy_.playerVariant());
```

## 2. `evehicles.sch` field layout and the price question

```
( vehicle  NAME  "MODEL"  VARIANT  CLASS  AI_KIND  F1  F2  ( TRAILERS… )  [TANK] )
```

`GameData::VehicleDef` calls `F1` *mass* and `F2` *price* (docs/script_gameplay.md
§5.9 does the same).  The shipped values say the opposite, so this module reads
**`F1` = price** and **`F2` = a per-model power/mass figure** (exposed as
`Offer::power`, unused by the economy):

| evidence | F1 (used as price) | F2 (used as power) |
|---|---|---|
| range over 158 entries | 0 … 118000 (11 trailers = 0) | 90 … 700 |
| `FIAT_1` / `BUS_1` / `VOLVO_LORRY1` | 32000 / 32000 / 80500 | 408 / 408 / 560 |
| model ladder | ZIL 20500/22000, KAMAZ 22000/32000, SCANIA 58000/60000, INTERNATIONAL 80200, STORM 118000 | ZIL 200/250, KAMAZ 240/269, SCANIA 360, VOLVO 420, MERCEDES 530, PETERBILT 532, STORM 700 |
| within a class | varies per model → a price | cars all 408, semitrailers all 101 → a class default |
| game economy | 22000–118000 for a truck, i.e. 10–50 cargo runs | 90–700 would be bought after one delivery |
| manual (*Замена машины*) | shows “… модель, **мощность**, грузоподъёмность и **стоимость**” per car | 408 hp for a Fiat is a data default, not a mass |

`Economy` therefore does **not** use `GameData::VehicleDef::price` (which is `F2`,
408 for a Fiat); it re-reads the record from `GameData::documents()`, exactly like
the price field appears in the file, and falls back to `GameData::vehicles` only
when the documents are unavailable (reported as `cargo=…(GameData fallback)`).
Offers come from every entry with price > 0: **147 of 158** (the 11
semitrailers have price 0 and are not for sale; the manual says they are picked
up free at the roadside).

## 3. Cargo jobs

**Markets.** A `town` record becomes a market when it has a node with role
`unload` that resolves in the `.way` table *and* a non-empty goods list.  On the
shipped data: 29 `town` records → **11 markets** (ids 0–10, the real towns);
the 14 petrol stations (`TOWN_XX_GSn`, empty goods) and 4 fuel depots
(`TOWN_XX_FUEL`, only a `fuel` node, no `unload`) are counted as skipped.

Each market's warehouse (its `unload` node) is the pickup **and** the drop-off:

| town | id | warehouse node | goods |
|---|---|---|---|
| TOWN_AJ Приозерск | 0 | `node_store_0000` | 11 |
| TOWN_AR Залесск | 1 | `node_store_0100` | 12 |
| TOWN_AU Ельнино | 2 | `node_store_0200` | 10 |
| TOWN_AP Южный | 3 | `node_store_0300` | 12 |
| TOWN_BC Ключи | 4 | `node_store_0400` | 8 |
| TOWN_AM Шахты | 5 | `node_store_0500` | 10 |
| TOWN_AD Боровое | 6 | `node_store_0600` | 11 |
| TOWN_CB Нижнегорск | 7 | `node_store_0700` | 10 |
| TOWN_BD Бухта | 8 | `node_store_0800` | 16 |
| TOWN_DC Горный | 9 | `node_store_0900` | 8 |
| TOWN_AA Алмазный | 10 | `node_store_1000` | 3 |

**Generation.** One job per (source market, traded item, destination market):
each item in the source town's goods list is delivered to every *other* market.
The goods lists are per-town (an item traded in one town usually is not traded
elsewhere), which is exactly the manual's rule that each town's trade base hands
out orders for goods brought in *from other towns*; the dispatcher screenshot in
`king.chm` (`bm61.gif`) shows the same shape: cargo | ОТКУДА | КУДА | №склада,
with the local town as the pickup.

111 deduplicated items × 10 destinations = **1110 jobs**, all 1110 resolved to
real `.way` nodes (0 unresolved).  Items whose id does not resolve are skipped
and counted (goods refs 115, 4 of them repeats inside one town list).

**Goods-id quirks (all handled):**

* `TOWN_AD` lists `c_TABLEWARE` while `merchandise.sch` spells `C_TABLEWARE` — the
  lookup falls back to a case-insensitive match (it is the car version of the
  truck item `TABLEWARE` listed in the same town).
* `C_WAGES`'s sell price is written `1#e+006` in `merchandise.sch`; `#` stands for
  the exponent marker, so the value is 1 000 000 (`strtod` alone would read 0 and
  drop the item).  This is the only `#` number in the whole script set.
* `merchandise.sch` escapes a quote (`"Телевизор 14\""`); `Sexp.cpp` handles it
  (107 items), `tools/sexp.py` does not — `tools/economy_check.py` patches its
  tokenizer so both sides agree.

**Reward.** Per the manual the payment is a share of the cargo's value plus what
the distance costs, so:

```
value  = max(buy, sell, 500)                 # merchandise.sch ( PRICE_BUY PRICE_SELL … )
km     = distance(pickup, dropoff) / 1000    # POSN is (x, z, h); 1 unit = 1 m
reward = clamp( round(0.20 * value * (1 + km)), 200, 250000 )
```

`buy`/`sell` are the two numbers of the item's price block; as `C_WAGES` has
0/0 there is a value floor of 500 (so a wages run is at least worth something).
The 250 000 cap tames the deliberate outliers — `RDIAMOND`/`C_DIAMONDS`
(280 000/380 000) and `C_WAGES` (1 000 000) — of which 31 jobs hit the cap.
Distances between markets are 1.6…12.4 km (median 5.3 km), rewards **259 … 250000**
(median 816).  Examples (all with the pickup at Южный `node_store_0300`):

| cargo | to | distance | value | reward |
|---|---|---|---|---|
| COCA_COLA | Шахты | 2799 m | 500 (floor) | 380 |
| MARTINI | Шахты | 2799 m | 1000 | 760 |
| TVSET21 | Шахты | 2799 m | 1500 | 1140 |
| ANALEPSIN | Шахты | 2799 m | 3000 | 2280 |
| EGGS (Ключи → Бухта) | | 1588 m | 500 (buy 100) | 259 (minimum) |
| PENTIUM_III (Бухта → Южный) | | 8619 m | 5000 | 9619 |
| C_MAIL (Южный → Алмазный) | | 7727 m | 30000 | 52365 |
| RDIAMOND (Алмазный → Южный) | | 7727 m | 280000 | 250000 (cap) |

**Delivery.** `accept()` allows one active job at a time (and refuses a job with
an unresolved node); `deliver(x, z, radius, &reward)` pays only when the player is
within `radius` of the drop-off node, marks the job done and increments
`delivered()`.  A job with no `.way` position can never become active.

## 4. Car dealers

The manual puts the car sellers on the parking lots next to the cars for sale
(“продавцы машин … обычно располагаются на стоянках”).  In the scripts those are
the bazaar `parking` groups (`bazaar` id ≥ 0; the off-road groups use −1) and the
stall itself is the bay with role 4, which also names the town.  All **15
bazaars** resolve to a `.way` node (0 unresolved), e.g.

| dealer | tile | stall node | position | note |
|---|---|---|---|---|
| MARKET_G (Южный) | ap | `node_car_0608` | (−2935, −4672) | 97 m from the start position |
| MARKET_AD (Боровое) | ad | `node_car_0004` | (568, −520) | |
| MARKET_D (Залесск) | ar | `node_car_0308` | (−4185, −1950) | |
| … 12 more | | | | bazaars 1–5, 7–14 |

`dealer()` is the one nearest `GameData::startPos` (−3020, −4625) → **MARKET_G**;
`dealerNear(x, z, r)` returns the stall the player is standing at.  Every dealer
offers the same catalogue (`offers()`); `buy(index)` is the transaction.

`GameData::Parking::tile` is always empty — `GameData::ingest()` builds its
`Walker` with an aggregate initialiser that omits the `tile` member — so the tile
in `Economy::Dealer::tile` is read from the script path instead.

## 5. Numbers on the shipped data

```
catalogue : 147 offers of 158 vehicle defs (11 semitrailers, price 0)
prices    : 14000 .. 118000   median 42000   mean 53646
start     : kStartMoney = 45000  -> buys 74/147 offers (50%)
            player starts in a ZilR (ZIL_LORRY2, variant 1), worth 22000
cargo     : 107 items (106 clean + C_WAGES `1#e+006`)
markets   : 11 of 29 town records (18 skipped), 115 goods refs -> 111 unique items
dealers   : 15 bazaar stalls, 0 unresolved; nearest to the start = MARKET_G
jobs      : 1110 = 111 items x 10 destinations, 0 unresolved
distances : 1588 .. 12448 m (median 5282)
rewards   : 259 .. 250000 (median 816), 31 jobs at the cap
counters  : GAME.CNF forsale=25 forhire=20 missions=10 fuel=10 (kept in counters())
```

The 45000 start is the “some funds and a mid-class truck” of the manual: it buys
every standard car (32000), the ZIL and KAMAZ lorries (22000/32000) and the KAMAZ
tractor, but none of the expensive rigs.  `Economy::kStartMoney` is the single
place to change it.

## 6. API

```cpp
// ---- setup ---------------------------------------------------------------
void setWayNodes(const std::map<std::string, Way::Node>& nodes);  // App::wayNodes_
void init(const GameData& gd);

// ---- money ---------------------------------------------------------------
long   money() const;                     // starts at kStartMoney
void   addMoney(long d);
void   setMoney(long m);
size_t delivered() const;                 // completed jobs
const std::map<std::string, long>& counters() const;   // ENV/GAME.CNF numbers

// ---- vehicle market ------------------------------------------------------
const std::vector<Offer>& offers() const; // sorted by price
bool   buy(size_t index, long tradeInCredit = 0);      // deducts price - credit
const std::string& playerModel() const;   // model container to respawn, "ZilR"
const std::string& playerVehicleId() const;// "ZIL_LORRY2"
int    playerVariant() const;             // variant/skin from GAME.CNF
long   ownedValue() const;                // catalogue price of the current truck

// ---- dealers (bazaar trade stalls) --------------------------------------
const std::vector<Dealer>& dealers() const;
bool   haveDealer() const;
const Dealer* dealer() const;             // nearest to the start position
int    dealerNear(float x, float z, float radius) const;   // -1 when none

// ---- cargo jobs ----------------------------------------------------------
const std::vector<Job>& jobs() const;
std::vector<size_t> jobsNear(float x, float z, float radius) const;  // nearest first
bool   accept(size_t jobIndex);           // one active job at a time
bool   deliver(float x, float z, float radius, long* rewardOut);
const Job* activeJob() const;
float  distance2D(float ax, float az, float bx, float bz) const;

// ---- reporting -----------------------------------------------------------
const Stats& stats() const;               // resolved/skipped counts + names
const Way::Node* wayNode(const std::string& name) const;
std::string report() const;               // one line for LOG()

// constants
Economy::kStartMoney = 45000;  kCargoValueFloor = 500;  kRewardValueFrac = 0.20;
Economy::kRewardMin  = 200;    kRewardCap       = 250000;
```

`Offer{id, model, display, klass, variant, price, power, owned}`,
`Job{cargoId, cargoName, fromTown, toTown, fromNode, toNode, fromX, fromZ, toX,
toZ, reward, distance, weight, volume, illegal, mafia, active, done}`,
`Dealer{id, display, tile, node, x, z, resolved}`,
`Stats{vehicleDefs, offers, offersSkipped, cargoDefs, cargoFromGameData, towns,
townsSkipped, goodsRefs, goodsResolved, goodsUnresolved, jobs, jobsResolved,
jobsUnresolved, bazaarBays, dealers, dealersUnresolved, unresolved[]}`.

Strings are the raw cp1251 bytes of the scripts (like the rest of the engine), so
`Job::cargoName`/`fromTown`/`toTown` and `Offer::display` (from `vd.sch`) feed the
original font directly.

## 7. Verification

```bash
cmake --build build -j                    # the game
tools/build_economy_check.sh              # C++ harness + independent Python check
python3 tools/economy_check.py            # just the Python side
python3 tools/economy_check.py -v         # + every offer and dealer
```

* `tools/economy_check.py` re-implements the rules from the shipped scripts only
  (it decrypts with `tools/decrypt_sch.py`, parses with `tools/sexp.py`, and has a
  small `WTWR`/`RNOD`/`POSN` reader).  It prints the offer count and price range,
  the job count, how many jobs resolved to real `.way` nodes, the reward range and
  example jobs, and an `EXPECT` line that the C++ harness must reproduce.  It
  exits non-zero if any node or cargo id is unresolved, so nothing is skipped
  silently.
* `tools/economy_selftest.cpp` (built by the script, not part of the game) runs
  `Economy` against the real scripts and `.way` files and asserts the API
  behaviour: offers loaded, a dealer placed, `jobsResolved > 0` with
  `jobsUnresolved == 0`, the start money, `buy()` deducting the price and
  re-pointing `playerModel()/playerVehicleId()`, refusing an owned vehicle / a bad
  index / a vehicle that costs more than the money, one active job at a time,
  `deliver()` paying only at the drop-off, and `init()` re-runs keeping progress.

Both print the same numbers (offers 147, prices 14000…118000, jobs 1110,
rewards 259…250000, 15 dealers).

## 8. What is not resolved

* **Field names.** `F1 = price` is well supported (§2) but the project spec calls
  `F2` “mass”; the game's dealer board shows *power* and *payload*.  `F2` is
  exposed but unused, so nothing depends on the answer.  `GameData::VehicleDef`
  keeps its own (swapped) `mass`/`price` — worth fixing there, not here.
* **`GameData::Cargo` is parsed from the wrong fields**: `display` is the unit
  (`CRATE`) and `price` is the weight (`25`), not the display name and
  `PRICE_BUY`.  This module re-reads `merchandise.sch` itself, so its own cargo
  table is correct; `--scripts`/`--vehicles` still print the GameData values.
* **No time limit / penalties / damage.** The manual's job deadline, “first to
  arrive” bonus, damage and fines are not modelled; `deliver()` pays the full
  reward immediately.
* **Reward tuning is a design choice**, not a shipped formula: the shipped data
  has no per-job price.  The 0.20 share, the +1 per-km factor, the floor of 500
  and the cap of 250000 are documented above and live in `Economy.h`.
* **No traffic competition and no mafia/police risk** even though the cargo tags
  (`illegal`, `formafia`) are carried on each job for the parent to use.
* **Fuel and repairs are out of scope**: `restable.sch` prices (including
  `PR_FUEL 100`) stay in `GameData::upgrades` for the parent.
* **`GameData::Parking::tile` is always empty** (aggregate-init bug) — worked
  around here by reading the tile from the script path.
* The dispatcher screenshot (`king.chm`) shows orders whose cargo is *not* traded
  by the local town (e.g. Яйца at Южный); the real generator therefore also
  considers the destination town's demand.  This module's rule (source goods ×
  every other destination) is a superset of that pattern for the cargo the
  scripts do list, but it does not invent supply/demand pairs that the data does
  not contain.

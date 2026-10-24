#pragma once
#include <string>
#include <algorithm>
#include <vector>
#include "../gfx/Math3D.h"

namespace ht2 {

class GroundMap;

// ---------------------------------------------------------------------------
// Driving model for an original vehicle (COMMON/TRUCKS.B3D mesh + the
// `vehicle.tech` parameter record of original/king.exe).
//
// The assembled vehicle mesh is in vehicle-local coordinates with
//   +x = right, +y = up, +z = forward (verified: the cab anchor ZilRBoothSpace
//   is at +3.26 on the longitudinal axis, the rear lights at -8.0),
// and its wheels touch the ground at y = -rideHeight.
//
// Physics.  Every drivetrain number below comes from the shipped
// `original/vehicle.tech`, which is an array of 43 fixed 2416-byte records
// (king.exe fcn.00508000 reads the record size at 0x508104 and the 0x25c-dword
// copy loop is at 0x508196; record i starts at file offset 0x39 + i*2416).
// fcn.004ebd00 copies one record verbatim into the runtime vehicle at
// object+0x1f48 (0x4ec78d..0x4ec79c), so `record offset X` is also
// `runtime object offset 0x1f48 + X`.  Record fields used here (see
// docs/physics.md for the full address list):
//
//   +0x060  mass                 [kg]        The fuel seed reads [ebx+0x60]
//           (0x508aa6, mass * dword 0x64dac8). The "mass" key pointer at
//           0x50879d is record+0 and is overwritten by the 12-dword basis
//           copy at 0x509030, so it is not the field to load.
//   +0x064  wheel radius         [m]         (key "rwheel", 0x5087fa, esi-0x5c)
//   +0x068  rear wheel radius    [m]         (key "rwheel1", 0x50881c)
//   +0x06c  tank                 [l]         (key "tank", 0x508b0b, ebx+0x6c).
//           The shipped floats also satisfy tank = fuelConsumption * kilometrage
//           (0x508af4). This is not horse_power.
//   +0x0a0  fuelConsumption      [l / km-unit] (0x508aaf, ebx+0xa0). Seed is
//           mass * 0.001 (dword 0x64dac8); the ini default is that seed times
//           FuelScale (dword 0x674ae8 = 10), then the stored value is divided
//           by FuelScale again (0x508ae1).
//   +0x0a8  kilometrage          (0x508a82, ebx+0xa8, default 50)
//   +0x070  fuelTimeEndurance    (0x508a1f, ebx+0x70, default dword 0x674ae4
//           = 1200). ZIL stores 846.22. The per-tick burn at 0x500641 divides
//           by this value; it does not read +0xa0. Not applied: the two other
//           factors are filter outputs (see Vehicle.cpp).
//   +0x0ac  air_press_coeff      [0.3]       (king.exe 0x50888a)
//   +0x0b0  air_resist_coeff     [0.3]       (king.exe 0x5088ac)
//   +0x0b4  air_side_resist_coeff[0.8]       (king.exe 0x5088d5)
//   +0x0c0  maxrotmomentum       [kg m^2/s]  (king.exe 0x5087bd, dest esi)
//   +0x0c4  derived, no key: maxrotmomentum * 0.2 / (2 * tach_rps)
//           (0x508a39..0x508a4b, qword 0x64b840 = 0.2). Copied to vehicle+0x1c8c
//           (0x4ed3cc) and multiplied inside 0x507360. Not the port's
//           inertia M / (2π·tach).
//   +0x0c8  breakpower           (0x5087d8 dest esi+8, default 5872). The
//           shipped ZIL float is 36000. No instruction was isolated that
//           applies this float as a brake force, so the port does not feed it
//           into braking.
//   +0x0cc  rear_axle_coeff      [5.0]       (king.exe 0x50891a, esi+0xc)
//   +0x0d0  reverse_trans_coeff  [-4.0]      (king.exe 0x50893e)
//   +0x0d8+ gear_trans_coeff1..13             (king.exe 0x50897d; a missing
//           gear is auto-filled as prev * 5/7, 0x64dae8 = 5/7 at 0x5089b7)
//   +0x10c  tach_with_max_power  [rev/s]     (king.exe 0x5088f3, default 20)
//   +0x11c  body_square          [m^2]       (king.exe 0x5098bf, ebx+0x11c)
//   +0x12c  driving_wheels[8]    ints        (king.exe 0x508f9a). Indexed
//           address 0x697994 has no reader; the ctor never loads stack slot
//           0x248+0x12c. kDriveShare stays 0.55.
//   +0x7c4  horse_power          int         (king.exe 0x5090f3, default 150).
//           Indexed address 0x69802c has no reader, and the file contains no
//           735.5 / 745.7 / 735.0 float or double. The engine output is
//           0x4ff460 → 0x507380 (exp and sin), not P = n·735.5. The port still
//           feeds the +0x6c float (the tank) into that watt expression.
//   +0x7c8  price                int         (king.exe 0x509116, default 10000)
//   +0x860  numtech              [count]     (0x4ec861 reads 0x6980c8 = +0x860)
//
// `maxrotmomentum` is the engine's angular momentum at the max-power speed:
// dividing it by omega(max power) yields a flywheel inertia of 0.2..18 kg m^2
// across the 43 shipped vehicles, which is the physical range for a car..lorry
// engine.  That is the relation the model uses for the engine inertia.
class Collision;

class Vehicle {
public:
    struct Input {
        float throttle = 0;   // -1..1
        float steer = 0;      // -1..1
        bool handbrake = false;
    };

    // One record of original/vehicle.tech.
    struct TechSpec {
        std::string section;      // "ZIL"
        std::string file;         // ".\lorries.tch"
        std::string display;      // "ZilR"
        float mass = 0.0f;        // kg
        float wheelRadius = 0.0f; // m
        float wheelRadiusRear = 0.0f;
        float powerHp = 0.0f;        // +0x6c float, the port torque input (same slot as the tank)
        float tankLitres = 0.0f;     // +0x6c, key "tank"
        float fuelPerKm = 0.0f;      // +0xa0, key "fuelConsumption"
        float rangeKm = 0.0f;         // +0xa8, key "kilometrage"
        float maxRotMomentum = 0.0f;
        float breakPower = 0.0f;     // +0xc8, key "breakpower" (not applied)
        int techPrice = 0;           // +0x7c8, key "price"
        int horsePower = 0;          // +0x7c4, key "horse_power" (no other xref)
        float bodySquare = 0.0f;     // +0x11c, key "body_square"
        float rearAxle = 5.0f;
        float reverseCoeff = -4.0f;
        float airPress = 0.3f, airResist = 0.3f, airSide = 0.8f;
        float tachMaxPower = 20.0f;         // rev/s at maximum power
        float gears[13] = {0};
        int gearCount = 0;                  // authored gears (auto-fill cut off)
        float numtech = 0.0f;
        bool valid = false;
    };

    void setGround(const GroundMap* g) { ground = g; }
    // The same map also carries the road network's own profile (GroundMap::
    // surfaceAt).  When `onRoad` is set as well the drivable height at a wheel
    // is the *road surface* wherever the point is on a road, and the terrain
    // field (which can sit several metres below the asphalt) everywhere else -
    // without this the truck settles onto the terrain under the carriageway and
    // the asphalt runs up through its wheels.
    void setRoads(const GroundMap* g) { roads = g; }
    // Per-frame reference for the off-network fallback (a cut under an unmapped
    // road): the drivable height may not change faster than kClimbRate * dt.
    void setHeightRef(float y) { heightRef = y; }
    // Drivable height at (x, z): the road surface when the point is on a road.
    float surfaceY(float x, float z, float fallback) const;
    void setCollisionHeight(float h) { collisionHeight_ = std::max(0.5f,h); }
    float collisionHeight() const { return collisionHeight_; }
    // Solid world geometry (src/game/Collision.cpp); when set, the body slides
    // along walls instead of passing through them.
    void setCollision(const Collision* c) { collision = c; }
    // Nudge the body sideways (used to push the player out of an AI car).
    void nudge(float dx, float dz) { x_ += dx; z_ += dz; }
    float halfLength() const { return WHEELBASE * 0.6f; }
    float halfWidth() const { return TRACK * 0.5f + 0.6f; }
    // x/z in world units, yaw in degrees (0 = +z ... see forward())
    void reset(float x, float z, float yawDeg);
    void update(const Input& in, float dt);

    // world transform for the mesh (rotation about up + translation)
    Mat4 modelMatrix() const;

    float x() const { return x_; }
    float y() const { return y_; }
    float z() const { return z_; }
    float yaw() const { return yaw_; }
    float speed() const { return speed_; }          // world units / second
    float speedKmh() const { return speed_ * 3.6f; }
    float pitch() const { return pitch_; }
    float roll() const { return roll_; }
    float steering() const { return steer_; } // smoothed steering, -1..1
    // Tyre radius from the tech record (+0x64). The rendered wheel spin is
    // odometer / this, the same ground-speed ratio the drivetrain uses.
    float wheelRadius() const {
        return spec_.wheelRadius > 0.01f ? spec_.wheelRadius : 0.5f;
    }
    // distance travelled since reset (for the drive test)
    float odometer() const { return odo_; }
    // Instrument cluster values: fuel 0..1 of the vehicle's own tank, the gear
    // the simulated gearbox is in (0 = N/neutral, -1 = R) and the engine speed
    // derived from wheel speed * gear * final drive.
    float fuel() const { return fuel_; }
    void setFuel(float f) { fuel_ = std::min(1.0f, std::max(0.0f, f)); }
    void addFuel(float litres);
    int gear() const { return gear_; }
    float rpm() const { return rpm_; }
    float engineLoad() const { return load_; }
    // Accumulated body damage 0..1: caps the top speed and is fixed at a service
    // station for money.
    float damage() const { return damage_; }
    void addDamage(float d) { damage_ = std::min(1.0f, damage_ + d); }
    void repair() { damage_ = 0.0f; }
    bool blocked() const { return blocked_; }
    void forward(float& fx, float& fz) const;
    // lateral acceleration the tyres are currently asked for, m/s^2 (telemetry)
    float lateralG() const { return latAccel_; }

    // ---- original drivetrain data ----------------------------------------
    // Load the shipped tech records.  Called automatically on the first reset()
    // when no spec has been chosen yet; the search path is HT2_TECH_FILE,
    // $HT2_DATA/vehicle.tech, ./original/vehicle.tech, ./vehicle.tech.
    static const std::vector<TechSpec>& techTable();
    static const TechSpec* findTech(const std::string& sectionOrDisplay);
    static bool techLoaded() { return !techTable().empty(); }
    static std::string techSource();
    // Pick the record that drives this instance (section name or display name);
    // an empty name keeps the current one.
    void useVehicle(const std::string& sectionOrDisplay);
    const TechSpec& spec() const { return spec_; }
    // Cargo / trailer mass added to the vehicle's own mass [kg].
    void setCargoMass(float kg) { cargoMass_ = std::max(0.0f, kg); }
    float cargoMass() const { return cargoMass_; }
    float totalMass() const { return spec_.mass + cargoMass_; }
    // Axle span used by the bicycle model.  Defaults to the ZIL measurement;
    // App replaces it with the span of the loaded body's wheel meshes.
    float wheelbase() const { return wheelbase_; }
    void setWheelbase(float metres);
    float tankLitres() const { return tankLitres_; }
    // record+0x11c when the shipped area is positive; otherwise the mass proxy.
    static float frontalArea(float bodySquare, float mass);

    // Station price per litre. king.exe 0x433240 takes the 3D distance from
    // the vehicle (x, horizontal, height) to the nearer of (-1720, 3950, 3)
    // and (-3000, -4715, 20); 0x4332e0 then does
    // trunc(20 + dist * 0.005) * 0.1. The result is never below 2.0.
    static double stationLitrePrice(float x, float z, float height);
    // Litres a pay press buys. king.exe 0x520d22:
    //   requested = (slider as a fraction of the tank), capped at the empty part
    //   if 2 * requested > money: requested = money / 2 - 1
    // `addFraction` < 0 means "the empty part" (the gasfull word, set to 100).
    static double stationPurchaseLitres(double tank, double fuelFraction,
                                        double money, double addFraction);
    // Full body-repair bill. king.exe 0x555f30 writes one damage float into
    // body channels 0, 1 and 2. The service open (0x521bcb) prices each with
    // the matching virtual (+0x38 / +0x34 / +0x28): a value below the qword at
    // 0x64b5a0 (0.01) is hidden, otherwise the price is _ftol(value * 5000)
    // (qword 0x64b848) and the three integers are summed. Wheel rows read
    // numtech (record +0x860), which is 0 on every drivable vehicle.
    // `condition` is that shared channel value (Vehicle::damage).
    static long bodyRepairPrice(double condition);

    // model constants (measured from the assembled mesh)
    static constexpr float RIDE_HEIGHT = 1.0f;   // wheel contact below y=0
    static constexpr float WHEELBASE = 7.9f;     // front +2.10 .. rear -5.80
    static constexpr float TRACK = 1.7f;

    // ---- tuning -----------------------------------------------------------
    // Redline / max-power-speed ratio.  The port's tachometer is scaled 0..2500
    // rpm with the redline at 2000 (src/app/App.cpp drawGauge), and the shipped
    // ZIL record has tach_with_max_power = 26.67 rev/s = 1600 rpm, so the
    // engine may rev 1.25x past its max-power speed.
    static constexpr float kRedlineRatio = 1.25f;
    static constexpr float kIdleRatio = 0.25f;      // idle = 25% of max power revs
    static constexpr float kDriveEfficiency = 0.90f;
    // Tyre friction and the share of the weight on the driven axle: this is what
    // limits the launch (a lorry cannot use its 1st gear multiplication).
    static constexpr float kTyreMu = 0.90f;
    static constexpr float kDriveShare = 0.55f;
    // The largest lateral acceleration the tyres can hold - the steering rate
    // falls off as a_lat / v above the speed where the tyres saturate.
    static constexpr float kLateralGrip = 6.0f;     // m/s^2
    // Holding the brake at a standstill for this long selects reverse (the
    // original's automatic box shifts to R from the selector; with no selector
    // in the port the held brake is the equivalent gesture).
    static constexpr float kReverseHold = 0.35f;
    static constexpr float kMaxSteerDeg = 38.0f;    // lock at the wheel
    // Braking.  king.exe stores "breakpower" at record+0xc8 (0x5087d8, default
    // 5872; ZIL ships 36000). Dividing that float by weight is not a constant
    // deceleration across the 43 records, and the force-site was not separated
    // from unrelated [reg+0xc8] uses, so the deceleration stays 0.5 g. The
    // handbrake locks the rear axle.
    static constexpr float kBrakeG = 0.50f;
    static constexpr float kHandbrakeG = 0.75f;
    static constexpr float kRollingCrr = 0.006f;    // asphalt, 0.6% of the weight
    static constexpr float kOffroadCrr = 0.035f;    // dirt / grass
    // A move that would drop the wheels more than this is refused, so the truck
    // cannot drive off a cliff, onto the sea or out of the mapped world where
    // the ground map has no data at all.
    float climbLimit = 3.5f;
    float dropLimit = 8.0f;       // max ground step the truck can climb (walls block)
    // reverse is limited to the reverse_trans_coeff-derived ratio
    float maxReverseKmh = 25.0f;

private:
    void applySpec(const TechSpec& s);
    void autoGear(float dt, float throttle);

    const GroundMap* ground = nullptr;
    const GroundMap* roads = nullptr;    // road profile of the same map (setRoads)
    float heightRef = -1e30f;    // committed centre contact height, never query-mutated
    float collisionHeight_ = 4.0f; // model bbox height; fallback without a loaded mesh
    const Collision* collision = nullptr;
    float x_ = 0, y_ = 0, z_ = 0, yaw_ = 0;
    float speed_ = 0, steer_ = 0;
    float pitch_ = 0, roll_ = 0;
    float odo_ = 0;
    float fuel_ = 1.0f;
    float load_ = 0.0f;
    float damage_ = 0.0f;
    bool blocked_ = false;

    TechSpec spec_;
    bool haveSpec_ = false;
    float cargoMass_ = 0.0f;
    float wheelbase_ = WHEELBASE;
    float tankLitres_ = 200.0f;
    float omega_ = 0.0f;        // engine angular velocity, rad/s
    int gear_ = 0;              // 0 = N, -1 = R, 1.. gearCount
    float rpm_ = 600.0f;
    float shiftTimer_ = 0.0f;
    float latAccel_ = 0.0f;
    float steerCmd_ = 0.0f;
    float revHold_ = 0.0f;   // how long the brake has been held at a standstill
};

} // namespace ht2

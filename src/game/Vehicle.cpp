#include "Vehicle.h"
#include "Collision.h"
#include "../world/GroundMap.h"
#include "../util/Log.h"
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace ht2 {

static const float PI = 3.14159265358979f;
static const float GRAV = 9.81f;

// ---------------------------------------------------------------------------
// original/vehicle.tech - the shipped drivetrain table
// ---------------------------------------------------------------------------
// king.exe fcn.00508000 (0x508000) opens ".\vehicle.tech" read-only, reads a
// u32 record size and requires it to be 2416 (0x508104: `cmp eax,0x970`), then
// slurps 2416-byte records (0x508196: fread(record,1,0x970)) into the global
// array at 0x697868.  43 of the 92 slots carry a record; the record grid starts
// at file offset 0x39 and record i therefore lives at 0x39 + i*2416.
// Each record is NUL-terminated strings on top of a numeric block:
//   +0x868 b3d booth node, +0x8a4 source .tch, +0x8c2 section,
//   +0x8e0 section_tech, +0x8fe display name, +0x912 short name, +0x926 node.
// The numeric offsets used below are documented in Vehicle.h.

namespace {

constexpr size_t kRecordBase = 0x39;
constexpr size_t kRecordSize = 0x970;

std::string recStr(const std::vector<char>& r, size_t off) {
    if (off >= r.size()) return {};
    size_t end = off;
    while (end < r.size() && r[end] != '\0') ++end;
    return std::string(r.data() + off, end - off);
}

float recF(const std::vector<char>& r, size_t off) {
    float v = 0.0f;
    if (off + 4 <= r.size()) std::memcpy(&v, r.data() + off, 4);
    return v;
}

// A gear coefficient that the record does not author is auto-filled by king.exe
// as `previous * 5/7` (0x5089b7 with the double 5/7 at 0x64dae8, reached when
// the value is below 0x64daf0).  The first such step is the end of the real
// gearbox, so the authored count is the index where the ratio jumps to exactly
// 5/7 of its predecessor.
int authoredGears(const float g[13]) {
    const float k = 5.0f / 7.0f;
    for (int i = 1; i < 13; ++i) {
        if (g[i] <= 0.0f) return i;
        if (std::fabs(g[i] - g[i - 1] * k) < 1e-4f * std::max(1.0f, std::fabs(g[i]))) return i;
        if (g[i] >= g[i - 1]) return i;         // must keep getting taller
    }
    return 13;
}

std::vector<std::string> techCandidates() {
    std::vector<std::string> out;
    if (const char* p = std::getenv("HT2_TECH_FILE")) out.emplace_back(p);
    if (const char* d = std::getenv("HT2_DATA")) out.emplace_back(std::string(d) + "/vehicle.tech");
    out.emplace_back("original/vehicle.tech");
    out.emplace_back("./vehicle.tech");
    out.emplace_back("vehicle.tech");
    return out;
}

std::string g_techSource;

std::vector<Vehicle::TechSpec> loadTech() {
    std::vector<Vehicle::TechSpec> table;
    std::ifstream f;
    std::string used;
    for (const std::string& c : techCandidates()) {
        f.open(c, std::ios::binary);
        if (f) { used = c; break; }
        f.clear();
    }
    if (!f) return table;
    std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < kRecordBase + kRecordSize) return table;
    for (size_t off = kRecordBase; off + kRecordSize <= data.size(); off += kRecordSize) {
        if (data[off + 0x8a4] != '.') continue;          // padding slot
        std::vector<char> r(data.begin() + off, data.begin() + off + kRecordSize);
        Vehicle::TechSpec s;
        s.file = recStr(r, 0x8a4);
        s.section = recStr(r, 0x8c2);
        s.display = recStr(r, 0x8fe);
        s.mass = recF(r, 0x60);
        s.wheelRadius = recF(r, 0x64);
        s.wheelRadiusRear = recF(r, 0x68);
        s.powerHp = recF(r, 0x6c);
        s.airPress = recF(r, 0xac);
        s.airResist = recF(r, 0xb0);
        s.airSide = recF(r, 0xb4);
        s.maxRotMomentum = recF(r, 0xc0);
        s.price = recF(r, 0xc8);
        s.rearAxle = recF(r, 0xcc);
        s.reverseCoeff = recF(r, 0xd0);
        for (int i = 0; i < 13; ++i) s.gears[i] = recF(r, 0xd8 + 4 * (size_t)i);
        s.tachMaxPower = recF(r, 0x10c);
        s.numtech = recF(r, 0x860);
        if (s.mass <= 0.0f || s.powerHp <= 0.0f || s.wheelRadius <= 0.0f) continue;
        if (s.tachMaxPower <= 0.5f) s.tachMaxPower = 20.0f;    // 1200 rpm default
        if (s.rearAxle <= 0.0f) s.rearAxle = 5.0f;
        if (s.reverseCoeff >= 0.0f) s.reverseCoeff = -4.0f;
        if (s.airResist <= 0.0f) s.airResist = 0.3f;
        s.gearCount = authoredGears(s.gears);
        if (s.gearCount < 1) continue;
        s.valid = true;
        table.push_back(s);
    }
    g_techSource = used + " (" + std::to_string(table.size()) + " vehicles)";
    return table;
}

} // namespace

const std::vector<Vehicle::TechSpec>& Vehicle::techTable() {
    static const std::vector<TechSpec> t = loadTech();
    return t;
}

std::string Vehicle::techSource() { return g_techSource; }

const Vehicle::TechSpec* Vehicle::findTech(const std::string& want) {
    if (want.empty()) return nullptr;
    std::string up;
    for (char c : want) up.push_back((char)::toupper((unsigned char)c));
    const std::vector<TechSpec>& t = techTable();
    // exact section first, then the display name (the b3d container name)
    for (const TechSpec& s : t) if (s.section == up) return &s;
    for (const TechSpec& s : t) {
        std::string d;
        for (char c : s.display) d.push_back((char)::toupper((unsigned char)c));
        if (d == up) return &s;
    }
    const TechSpec* variant = nullptr;
    size_t longest = 0;
    for (const TechSpec& s : t) {
        std::string d;
        for (char c : s.display) d.push_back((char)::toupper((unsigned char)c));
        if (!d.empty() && d.size() > longest && up.compare(0, d.size(), d) == 0) {
            variant = &s;
            longest = d.size();
        }
    }
    return variant; // B3D variants, e.g. ZilR, append a suffix to the display name
}

void Vehicle::useVehicle(const std::string& sectionOrDisplay) {
    if (sectionOrDisplay.empty()) return;
    if (const TechSpec* s = findTech(sectionOrDisplay)) { applySpec(*s); return; }
    if (!techTable().empty()) {
        // Default to the lorry the shipped GAME.CNF starts the player in.
        if (const TechSpec* s = findTech("ZIL")) { applySpec(*s); return; }
        applySpec(techTable().front());
    }
}

void Vehicle::applySpec(const TechSpec& s) {
    spec_ = s;
    haveSpec_ = true;
    // A lorry tank is ~200 l and a car ~50; the shipped record does not carry
    // the tank key that the .tch text format reads (king.exe 0x508a08) at a
    // location we could pin, so it is derived from the mass.
    tankLitres_ = std::min(400.0f, std::max(40.0f, s.mass / 25.0f));
    omega_ = 0.0f;
    gear_ = 0;
}

void Vehicle::addFuel(float amount) {
    // `fuel_` is a 0..1 fraction of tankLitres_ (the HUD, the service screen and
    // the save file all work in fractions), so `amount` is a fraction top-up.
    fuel_ = std::min(1.0f, std::max(0.0f, fuel_ + amount));
}

// The drivable height under a wheel: the road's own profile where the point is
// on a road (the `.way` network every tile's asphalt agrees with), the terrain
// field otherwise.  `GroundMap::heightAt()` is a *terrain* field and under a
// road it is the ground beside/below the carriageway - several metres below the
// asphalt (docs/terrain_road.md) - which is what let the truck sink through the
// road surface it was standing on.
float Vehicle::surfaceY(float x, float z, float fallback) const {
    // HT2_NO_ROAD_PROFILE=1 is the A/B control for the "cars fly and sink"
    // measurement: with it the truck follows the terrain field again, which is
    // what it did before the road profile was wired in (docs/terrain_road.md §8).
    static const bool noProfile = getenv("HT2_NO_ROAD_PROFILE") != nullptr;
    if (roads && !noProfile) {
        // Query actual asphalt first, then graph/terrain fallback. The explicit
        // reference distinguishes a deck from the carriageway underneath it.
        // Sampling wheels must not change the reference for other samples.
        float base = ground ? ground->heightAt(x, z, fallback) : fallback;
        if (!ground) return fallback;
        int src = 0;
        float dist = 0.0f;
        const float refIn = fallback > -1e29f ? fallback : base;
        float h = ground->drivableY(x, z, refIn, &src, &dist);
        if (getenv("HT2_RIDE_DEBUG") && std::fabs(h) < 1.0f && std::fabs(z + 4575.0f) < 5.0f)
            LOG("surfaceY-debug: (%.1f, %.1f) refIn %.2f base %.2f h %.3f src %d heightRef %.2f",
                x, z, refIn, base, h, src, heightRef);
        if (src != 0) return h;
        // Off every mapped road: the terrain field there can be a deep cut under
        // unmapped asphalt, so one query is not allowed to drop the truck into
        // it.  A 3 m correction is already 10x any road grade, and the ride
        // smooths it over the following frames.
        if (fallback > -1e29f) {
            float d = base - fallback;
            if (d > 3.0f) d = 3.0f;
            if (d < -3.0f) d = -3.0f;
            return fallback + d;      // query only: never mutate the ride state
        }
        return base;
    }
    return ground ? ground->heightAt(x, z, fallback) : fallback;
}

void Vehicle::reset(float x, float z, float yawDeg) {
    x_ = x; z_ = z; yaw_ = yawDeg;
    speed_ = steer_ = odo_ = 0;
    // Seed the fallback reference from the terrain (as before), then ask the
    // layered surface for the real height with that reference - asking with 0
    // makes the tile-asphalt tier pick the wrong surface and the first frames
    // then correct by metres (a 30-degree pitch spike at spawn).
    float ref = ground ? ground->heightAt(x, z, 0.0f) : 0.0f;
    heightRef = ref;
    y_ = surfaceY(x, z, ref) + RIDE_HEIGHT;
    pitch_ = roll_ = 0;
    if (!haveSpec_) useVehicle("ZIL");
    gear_ = 0;
    omega_ = 2.0f * PI * spec_.tachMaxPower * kIdleRatio;
    rpm_ = omega_ * 60.0f / (2.0f * PI);
    load_ = 0.0f;
    latAccel_ = 0.0f;
    shiftTimer_ = 0.0f;
    blocked_ = false;
}

void Vehicle::forward(float& fx, float& fz) const {
    // the vehicle's +z is forward; the renderer maps b3d z to world z directly,
    // and the camera code uses yaw with +z at yaw 0, so:
    float a = yaw_ * PI / 180.0f;
    fx = sinf(a);
    fz = cosf(a);
}

// Automatic gearbox.  Up-shifts just under the redline, down-shifts when the
// engine would bog, and kick-downs under full throttle - the original's HUD
// shows the resulting gear (the port's own tachometer is scaled 0..2500 rpm
// with the redline at 2000, src/app/App.cpp drawGauge).
void Vehicle::autoGear(float dt, float throttle) {
    shiftTimer_ = std::max(0.0f, shiftTimer_ - dt);
    const float omegaRed = 2.0f * PI * spec_.tachMaxPower * kRedlineRatio;
    const float omegaMaxP = 2.0f * PI * spec_.tachMaxPower;
    const float omegaDown = omegaMaxP * 0.45f;
    const float r = spec_.wheelRadius > 0.01f ? spec_.wheelRadius : 0.5f;
    float w = std::fabs(speed_) / r;                 // wheel rad/s

    if (std::fabs(speed_) < 0.4f) {
        // stationary: the driver selects D with the throttle, R by holding the
        // brake pedal for kReverseHold, and the box drops to N when neither is
        // touched - the original's automatic transmission does the same.
        if (throttle < -0.01f && speed_ <= 0.1f) {
            revHold_ += dt;
            if (revHold_ >= kReverseHold) { gear_ = -1; return; }
        } else {
            revHold_ = 0.0f;
        }
        if (throttle > 0.01f) { gear_ = 1; return; }
        if (gear_ == -1) return;             // keep R while it is rolling back
        gear_ = 0;
        return;
    }
    revHold_ = 0.0f;
    if (speed_ < -0.05f) { gear_ = -1; return; }
    if (gear_ == -1) gear_ = 1;

    if (gear_ < 1) gear_ = 1;
    if (gear_ > spec_.gearCount) gear_ = spec_.gearCount;

    auto omegaIn = [&](int g) {
        float ratio = spec_.gears[g - 1] * spec_.rearAxle;
        return w * ratio;
    };
    if (shiftTimer_ > 0.0f) return;
    if (gear_ < spec_.gearCount && omegaIn(gear_) > omegaRed * 0.96f) {
        gear_++; shiftTimer_ = 0.30f;
    } else if (gear_ > 1 && omegaIn(gear_) < omegaDown) {
        gear_--; shiftTimer_ = 0.25f;
    } else if (gear_ > 2 && steerCmd_ * 0.0f == 0.0f && omegaIn(gear_) < omegaMaxP * 0.75f &&
               load_ > 0.85f) {
        gear_--; shiftTimer_ = 0.25f;                // kick-down
    }
}

void Vehicle::update(const Input& in, float dt) {
    if (dt <= 0) return;
    if (!haveSpec_) useVehicle("ZIL");

    const float r = spec_.wheelRadius > 0.01f ? spec_.wheelRadius : 0.5f;
    const float mass = totalMass();
    const float omegaMaxP = 2.0f * PI * spec_.tachMaxPower;
    const float omegaRed = omegaMaxP * kRedlineRatio;
    const float omegaIdle = std::max(omegaMaxP * kIdleRatio, 60.0f);
    // Flywheel + drivetrain inertia: maxrotmomentum is the engine's angular
    // momentum at the max-power speed (king.exe 0x5087bd stores it in
    // record+0xc0), so I = maxrotmomentum / omega(max power).
    const float inertia = std::max(0.02f, spec_.maxRotMomentum / std::max(1.0f, omegaMaxP));

    load_ = std::min(1.0f, std::fabs(in.throttle));
    steerCmd_ = in.steer;

    // --- fuel burn: the tank scaled by the engine's own consumption curve ----
    {
        float kmh = speedKmh();
        float litresPerSec = (0.08f + 0.22f * std::fabs(in.throttle)) * (0.4f + kmh / 90.0f);
        // fuel_ stays a 0..1 fraction of the tank (the HUD, the service screen
        // and the save file all work in fractions); the 300 l reference is the
        // historical burn rate and is what the save-file round trip pins.
        fuel_ = std::max(0.0f, fuel_ - litresPerSec * dt / 300.0f);
    }

    // ---- select the gear --------------------------------------------------
    autoGear(dt, in.throttle);

    // ---- engine torque ----------------------------------------------------
    // P = T * omega with a low-end torque ceiling (1.3x the max-power torque)
    // and a hard redline; damage and an empty tank cut the power.
    float powerW = spec_.powerHp * 735.5f;
    powerW *= (1.0f - 0.85f * damage_);              // a wrecked engine barely pulls
    if (fuel_ <= 0.0f) powerW = 0.0f;
    float omega = std::max(omega_, omegaIdle);
    float torque = powerW / omega;
    const float tMax = 1.3f * powerW / omegaMaxP;
    torque = std::min(torque, tMax);
    if (omega_ < omegaIdle) torque *= omega_ / omegaIdle;
    float thr = std::max(0.0f, gear_ < 0 ? -in.throttle : in.throttle);
    torque *= thr;

    float ratio = 0.0f;
    if (gear_ > 0 && gear_ <= spec_.gearCount) ratio = spec_.gears[gear_ - 1];
    else if (gear_ < 0) ratio = std::fabs(spec_.reverseCoeff);
    float driveRatio = ratio * spec_.rearAxle;
    float fDrive = torque * driveRatio * kDriveEfficiency / r;
    if (gear_ < 0) fDrive = -fDrive;

    // ---- resistances ------------------------------------------------------
    int dir = speed_ > 0.0f ? 1 : (speed_ < 0.0f ? -1 : 0);
    float crr = kRollingCrr;
    bool offroad = false;
    if (ground && !ground->empty()) {
        offroad = !ground->onRoad(x_, z_);
        if (offroad) crr = kOffroadCrr;
    }
    float fRoll = -dir * mass * GRAV * crr;
    // Frontal-area proxy from the mass (the record's own body_square key was not
    // located): 2.9 m^2 for a 1500 kg car .. 9.2 m^2 for a 12 t lorry.
    float area = 0.6f * mass / 1000.0f + 2.0f;
    float fAir = -dir * 0.5f * 1.2f * std::max(0.05f, spec_.airResist) * area * speed_ * speed_;
    float fBrake = 0.0f;
    if (in.throttle < -0.01f && speed_ > 0.1f) {
        fBrake = -mass * GRAV * kBrakeG * std::min(1.0f, -in.throttle);
    } else if (in.throttle > 0.01f && speed_ < -0.1f) {
        fBrake = mass * GRAV * kBrakeG * std::min(1.0f, in.throttle);
    }
    if (in.handbrake) fBrake += -dir * mass * GRAV * kHandbrakeG;

    float fTotal = fDrive + fRoll + fAir + fBrake;

    // ---- integrate the drivetrain ----------------------------------------
    // The tyres can only transmit kTyreMu * the weight; the driven axle gets a
    // share of that for traction, the brakes may use the whole contact patch.
    // The drivetrain's own inertia (reflected through the gear) resists the
    // acceleration - that is what makes 1st gear so short on a lorry.
    float fGrip = kTyreMu * mass * GRAV;
    fDrive = std::max(-fGrip * kDriveShare, std::min(fDrive, fGrip * kDriveShare));
    fBrake = std::max(-fGrip, std::min(fBrake, fGrip));
    fTotal = fDrive + fRoll + fAir + fBrake;

    float iRefl = inertia * driveRatio * driveRatio / (r * r);
    // Under braking the torque converter unlocks (a real automatic does the
    // same), so the engine's reflected inertia no longer fights the brakes.
    const bool changingDirection = (in.throttle < -0.01f && speed_ > 0.1f) ||
                                   (in.throttle > 0.01f && speed_ < -0.1f);
    float mEff = changingDirection ? mass : mass + iRefl;

    if (gear_ == 0) {
        // neutral / clutch out: coast on the resistances only
        float fCoast = fRoll + fAir;
        speed_ += (fCoast / mass) * dt;
    } else {
        speed_ += (fTotal / mEff) * dt;
    }
    if (std::fabs(speed_) < 0.05f && std::fabs(in.throttle) < 0.01f) speed_ = 0;

    // ---- engine speed -----------------------------------------------------
    // Below the clutch speed the engine idles; above it the engine is locked to
    // the wheel through the selected gear.
    float wWheel = std::fabs(speed_) / r;
    float omegaTarget;
    if (gear_ == 0) {
        omegaTarget = omegaIdle + std::fabs(in.throttle) * (omegaMaxP - omegaIdle);
    } else {
        omegaTarget = wWheel * driveRatio;
    }
    omegaTarget = std::min(omegaTarget, omegaRed);
    omegaTarget = std::max(omegaTarget, omegaIdle * 0.9f);
    // first-order lag (the engine + flywheel), faster on the way up
    float tau = omegaTarget > omega_ ? 0.08f : 0.20f;
    omega_ += (omegaTarget - omega_) * std::min(1.0f, dt / tau);
    // while stationary the engine follows the throttle for the audio
    if (gear_ == 0 && std::fabs(speed_) < 0.4f)
        omega_ = omegaIdle + std::min(1.0f, std::fabs(in.throttle)) * 0.55f * (omegaMaxP - omegaIdle);
    rpm_ = omega_ * 60.0f / (2.0f * PI);

    // top-speed clamp: the redline in the tallest gear (and reverse)
    {
        float topRatio = spec_.gears[spec_.gearCount - 1] * spec_.rearAxle;
        float vTop = omegaRed * r / std::max(0.05f, topRatio);
        if (speed_ > vTop) speed_ = vTop;
        float vRev = 2.0f * PI * spec_.tachMaxPower * kRedlineRatio * r /
                     std::max(0.05f, std::fabs(spec_.reverseCoeff) * spec_.rearAxle);
        vRev = std::min(vRev, maxReverseKmh / 3.6f);
        if (speed_ < -vRev) speed_ = -vRev;
    }

    // --- steering ----------------------------------------------------------
    // Bicycle model: the yaw rate is v*tan(lock)/wheelbase, capped by the
    // lateral acceleration the tyres can hold - so the effective steering rate
    // climbs with speed and then falls off as grip/v.
    float steerTarget = in.steer;
    if (in.handbrake) steerTarget *= 0.6f;
    steer_ += (steerTarget - steer_) * std::min(1.0f, dt * 6.0f);
    float sp = std::fabs(speed_);
    float lockRad = kMaxSteerDeg * PI / 180.0f;
    // the driver winds less lock on at speed (idle_steering / steering_sence)
    lockRad *= 1.0f / (1.0f + sp * sp * 0.0022f);
    float yawRate = speed_ * std::tan(lockRad * steer_) / WHEELBASE;   // rad/s
    float yawCap = kLateralGrip / std::max(1.0f, sp);                  // rad/s
    latAccel_ = std::fabs(yawRate) * sp;
    if (std::fabs(yawRate) > yawCap) {
        yawRate = (yawRate > 0 ? 1.0f : -1.0f) * yawCap;
        latAccel_ = kLateralGrip;
    }
    yaw_ += yawRate * 180.0f / PI * dt;
    while (yaw_ > 180.0f) yaw_ -= 360.0f;
    while (yaw_ < -180.0f) yaw_ += 360.0f;

    // --- integrate position ------------------------------------------------
    float fx, fz;
    forward(fx, fz);
    float dx = fx * speed_ * dt, dz = fz * speed_ * dt;
    // The ground step is still the fallback when no collision module is wired:
    // refuse a move that would climb more than climbLimit.
    blocked_ = false;
    if (ground && (dx != 0 || dz != 0)) {
        // The same drivable surface the wheels follow: a road profile that has to
        // be entered through the terrain field would otherwise look like a 12 m
        // wall here and stop the truck at the end of the `.way` data.
        float cur = surfaceY(x_, z_, y_ - RIDE_HEIGHT);
        float nxt = surfaceY(x_ + dx, z_ + dz, cur);
        if (nxt - cur > climbLimit) {
            blocked_ = true;
            dx = dz = 0;
            speed_ *= 0.25f;
        } else if (!ground->inside(x_ + dx, z_ + dz)) {
            blocked_ = true;                      // the edge of the mapped world
            dx = dz = 0;
            speed_ *= 0.1f;
        } else if (ground->waterAt(x_ + dx, z_ + dz, nxt)) {
            blocked_ = true;                      // the sea is not a road
            dx = dz = 0;
            speed_ *= 0.1f;
        } else if (cur - nxt > dropLimit) {
            // a cliff, the sea or the edge of the mapped world: the truck stops
            // at the brink instead of driving into the void
            blocked_ = true;
            dx = dz = 0;
            speed_ *= 0.1f;
        }
    }
    // solid geometry: test the body as two circles (front and rear axle) so a
    // long truck cannot clip a wall with the trailer
    if (collision && !collision->empty() && (dx != 0 || dz != 0)) {
        const float rad = halfWidth();
        const float off = halfLength() * 0.5f;
        float ox, oz;
        bool ok = true;
        for (int end = -1; end <= 1 && ok; end += 2) {
            float sx = x_ + fx * off * (float)end, sz = z_ + fz * off * (float)end;
            float tx = sx + dx, tz = sz + dz;
            const float bottom = y_ - RIDE_HEIGHT + 0.05f;
            if (!collision->resolveMove(sx, sz, tx, tz, rad, ox, oz,
                                        bottom, bottom + collisionHeight_)) {
                ok = false;
                dx = ox - sx;
                dz = oz - sz;
            }
        }
        if (!ok) {
            blocked_ = true;
            if (std::fabs(speed_) > 4.0f) damage_ = std::min(1.0f, damage_ + 0.004f);
            speed_ *= 0.3f;                       // scrape along the wall, lose speed
        }
    }
    x_ += dx; z_ += dz;
    odo_ += sqrtf(dx * dx + dz * dz);

    // --- ground following --------------------------------------------------
    if (ground) {
        float h = surfaceY(x_, z_, y_ - RIDE_HEIGHT);
        heightRef = h;
        // sample the ground at the four wheel contact points for pitch/roll
        float hf = surfaceY(x_ + fx * WHEELBASE * 0.5f, z_ + fz * WHEELBASE * 0.5f, h);
        float hr = surfaceY(x_ - fx * WHEELBASE * 0.5f, z_ - fz * WHEELBASE * 0.5f, h);
        float lx = -fz, lz = fx;                  // left vector
        float hl = surfaceY(x_ + lx * TRACK * 0.5f, z_ + lz * TRACK * 0.5f, h);
        float hr2 = surfaceY(x_ - lx * TRACK * 0.5f, z_ - lz * TRACK * 0.5f, h);
        float targetY = h + RIDE_HEIGHT;
        // Smooth the vertical so bumps do not teleport the body, and rate-limit
        // the target itself: the `.way` road network stops short of a few
        // driveable tiles, and where the terrain field takes over (a cut metres
        // below the asphalt) a single-frame target change would still be a jolt.
        // 8 m/s is far above any real road grade; a frame at 60 Hz may correct
        // at most 0.13 m.
        if (dt > 0.0f) {
            const float maxD = 8.0f * dt;
            float step = (targetY - y_) * (dt > 0.3f ? 1.0f : 0.25f);
            if (step > maxD) step = maxD;
            if (step < -maxD) step = -maxD;
            y_ += step;
        }
        float targetPitch = atan2f(hf - hr, WHEELBASE) * 180.0f / PI;
        float targetRoll = atan2f(hr2 - hl, TRACK) * 180.0f / PI;
        pitch_ += (targetPitch - pitch_) * 0.2f;
        roll_ += (targetRoll - roll_) * 0.2f;
    }
}

Mat4 Vehicle::modelMatrix() const {
    // rotate about up by yaw, tilt by pitch/roll around the vehicle axes, then
    // translate so the wheel contact plane sits on the ground.
    Mat4 m = Mat4::translation(x_, y_, z_);
    m = m * Mat4::rotY(yaw_);
    m = m * Mat4::rotX(-pitch_ * PI / 180.0f);
    m = m * Mat4::rotZ(roll_ * PI / 180.0f);
    return m;
}

} // namespace ht2

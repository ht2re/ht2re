#pragma once
#include <string>
#include <algorithm>
#include "../gfx/Math3D.h"

namespace ht2 {

class GroundMap;

// Arcade driving model for an original vehicle mesh (COMMON/TRUCKS.B3D).
//
// The assembled vehicle mesh is in vehicle-local coordinates with
//   +x = right, +y = up, +z = forward (verified: the cab anchor ZilRBoothSpace
//   is at +3.26 on the longitudinal axis, the rear lights at -8.0),
// and its wheels touch the ground at y = -rideHeight.
//
// Physics is deliberately simple (the original .tech tables are still binary):
// throttle/brake -> speed, steering -> yaw rate proportional to speed, gravity
// pulls the body to the terrain surface, and the terrain gradient gives pitch
// and roll.  Everything is deterministic so a headless drive test can replay it.
class Collision;

class Vehicle {
public:
    struct Input {
        float throttle = 0;   // -1..1
        float steer = 0;      // -1..1
        bool handbrake = false;
    };

    void setGround(const GroundMap* g) { ground = g; }
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
    // distance travelled since reset (for the drive test)
    float odometer() const { return odo_; }
    // Instrument cluster values: fuel 0..1 (start full), a 6-speed gearbox
    // derived from speed and engine revolutions derived from gear + speed.
    float fuel() const { return fuel_; }
    void setFuel(float f) { fuel_ = f; }
    void addFuel(float litres) { fuel_ = std::min(1.0f, fuel_ + litres); }
    int gear() const;
    float rpm() const;
    float engineLoad() const { return load_; }
    // Accumulated body damage 0..1: caps the top speed and is fixed at a service
    // station for money.
    float damage() const { return damage_; }
    void addDamage(float d) { damage_ = std::min(1.0f, damage_ + d); }
    void repair() { damage_ = 0.0f; }
    bool blocked() const { return blocked_; }
    void forward(float& fx, float& fz) const;

    // model constants (measured from the assembled mesh)
    static constexpr float RIDE_HEIGHT = 1.0f;   // wheel contact below y=0
    static constexpr float WHEELBASE = 7.9f;     // front +2.10 .. rear -5.80
    static constexpr float TRACK = 1.7f;

    float maxSpeed = 30.0f;        // ~108 km/h
    float accel = 7.0f;            // units/s^2
    float brakeDecel = 14.0f;
    float drag = 0.45f;
    float climbLimit = 3.5f;
    // A move that would drop the wheels more than this is refused, so the truck
    // cannot drive off a cliff, onto the sea or out of the mapped world where
    // the ground map has no data at all.
    float dropLimit = 8.0f;       // max ground step the truck can climb (walls block)
    float steerRate = 55.0f;       // deg/s at full lock, scaled by speed

private:
    const GroundMap* ground = nullptr;
    const Collision* collision = nullptr;
    float x_ = 0, y_ = 0, z_ = 0, yaw_ = 0;
    float speed_ = 0, steer_ = 0;
    float pitch_ = 0, roll_ = 0;
    float odo_ = 0;
    float fuel_ = 1.0f;
    float load_ = 0.0f;
    float damage_ = 0.0f;     
    bool blocked_ = false;
};

} // namespace ht2

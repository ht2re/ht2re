#include "Vehicle.h"
#include "Collision.h"
#include "../world/GroundMap.h"
#include "../util/Log.h"
#include <cmath>

namespace ht2 {

static const float PI = 3.14159265358979f;

void Vehicle::reset(float x, float z, float yawDeg) {
    x_ = x; z_ = z; yaw_ = yawDeg;
    speed_ = steer_ = odo_ = 0;
    y_ = ground ? ground->heightAt(x, z, 0.0f) + RIDE_HEIGHT : RIDE_HEIGHT;
    pitch_ = roll_ = 0;
}

// Six-speed box: the ratios are chosen so the top speed lands near the 6th
// gear at ~2000 rpm, which is what the original dashboard shows.
int Vehicle::gear() const {
    float kmh = speedKmh();
    if (kmh < 1.0f) return 0;                       // N
    if (speed_ < -0.5f) return -1;                  // R
    const float limits[6] = {12.0f, 24.0f, 38.0f, 54.0f, 72.0f, 200.0f};
    for (int g = 0; g < 6; ++g)
        if (kmh <= limits[g]) return g + 1;
    return 6;
}

float Vehicle::rpm() const {
    const int g = gear();
    if (g <= 0) return 600.0f + fabsf(speed_) * 40.0f;   // idle
    const float lo[6] = {0.0f, 12.0f, 24.0f, 38.0f, 54.0f, 72.0f};
    const float hi[6] = {12.0f, 24.0f, 38.0f, 54.0f, 72.0f, 110.0f};
    float t = (speedKmh() - lo[g - 1]) / std::max(1.0f, hi[g - 1] - lo[g - 1]);
    t = std::min(1.0f, std::max(0.0f, t));
    return 700.0f + t * 1800.0f;
}

void Vehicle::forward(float& fx, float& fz) const {
    // the vehicle's +z is forward; the renderer maps b3d z to world z directly,
    // and the camera code uses yaw with +z at yaw 0, so:
    float a = yaw_ * PI / 180.0f;
    fx = sinf(a);
    fz = cosf(a);
}

void Vehicle::update(const Input& in, float dt) {
    if (dt <= 0) return;

    // fuel burn: ~30 l/100 km at full throttle, scaled by speed and throttle
    {
        float kmh = speedKmh();
        float litresPerSec = (0.08f + 0.22f * fabsf(in.throttle)) * (0.4f + kmh / 90.0f);
        fuel_ = std::max(0.0f, fuel_ - litresPerSec * dt / 300.0f);
        load_ = std::min(1.0f, fabsf(in.throttle));
    }

    // --- longitudinal ------------------------------------------------------
    float thr = in.throttle;
    if (thr > 0.01f) {
        speed_ += accel * thr * dt;
    } else if (thr < -0.01f) {
        // brake / reverse
        if (speed_ > 0.1f) speed_ -= brakeDecel * -thr * dt;
        else speed_ += accel * 0.6f * thr * dt;     // reverse gear
    }
    // rolling drag + engine braking
    float dragK = drag * (in.handbrake ? 3.0f : 1.0f);
    speed_ -= speed_ * dragK * dt;
    speed_ -= (speed_ > 0 ? 1 : -1) * (in.handbrake ? 12.0f : 1.2f) * dt;
    if (fabsf(speed_) < 0.05f && fabsf(thr) < 0.01f) speed_ = 0;
    const float topSpeed = maxSpeed * (1.0f - 0.45f * damage_);
    if (speed_ > topSpeed) speed_ = topSpeed;
    if (speed_ < -maxSpeed * 0.35f) speed_ = -maxSpeed * 0.35f;

    // --- steering ----------------------------------------------------------
    // yaw rate grows with speed up to a plateau, then falls off (like a real car)
    float sp = fabsf(speed_);
    float grip = sp / (sp + 6.0f);                 // 0 at standstill -> 1 fast
    float dir = speed_ >= 0 ? 1.0f : -1.0f;
    yaw_ += in.steer * steerRate * grip * dir * dt * 0.5f;
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
        float cur = ground->heightAt(x_, z_, y_ - RIDE_HEIGHT);
        float nxt = ground->heightAt(x_ + dx, z_ + dz, cur);
        if (nxt - cur > climbLimit) {
            blocked_ = true;
            dx = dz = 0;
            speed_ *= 0.25f;
        } else if (!ground->inside(x_ + dx, z_ + dz)) {
            blocked_ = true;                      // the edge of the mapped world
            dx = dz = 0;
            speed_ *= 0.1f;
        } else if (ground->waterAt(x_ + dx, z_ + dz)) {
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
        const float r = halfWidth();
        const float off = halfLength() * 0.5f;
        float ox, oz;
        bool ok = true;
        for (int end = -1; end <= 1 && ok; end += 2) {
            float sx = x_ + fx * off * (float)end, sz = z_ + fz * off * (float)end;
            float tx = sx + dx, tz = sz + dz;
            if (!collision->resolveMove(sx, sz, tx, tz, r, ox, oz)) {
                ok = false;
                dx = ox - sx;
                dz = oz - sz;
            }
        }
        if (!ok) {
            blocked_ = true;
            if (fabsf(speed_) > 4.0f) damage_ = std::min(1.0f, damage_ + 0.004f);
            speed_ *= 0.3f;                       // scrape along the wall, lose speed
        }
    }
    x_ += dx; z_ += dz;
    odo_ += sqrtf(dx * dx + dz * dz);

    // --- ground following --------------------------------------------------
    if (ground) {
        float h = ground->heightAt(x_, z_, y_ - RIDE_HEIGHT);
        // sample the ground at the four wheel contact points for pitch/roll
        float hf = ground->heightAt(x_ + fx * WHEELBASE * 0.5f, z_ + fz * WHEELBASE * 0.5f, h);
        float hr = ground->heightAt(x_ - fx * WHEELBASE * 0.5f, z_ - fz * WHEELBASE * 0.5f, h);
        float lx = -fz, lz = fx;                  // left vector
        float hl = ground->heightAt(x_ + lx * TRACK * 0.5f, z_ + lz * TRACK * 0.5f, h);
        float hr2 = ground->heightAt(x_ - lx * TRACK * 0.5f, z_ - lz * TRACK * 0.5f, h);
        float targetY = h + RIDE_HEIGHT;
        // smooth the vertical so bumps do not teleport the body
        y_ += (targetY - y_) * (dt > 0.3f ? 1.0f : 0.25f);
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
    m = m * Mat4::rotX(pitch_ * PI / 180.0f);
    m = m * Mat4::rotZ(roll_ * PI / 180.0f);
    return m;
}

} // namespace ht2

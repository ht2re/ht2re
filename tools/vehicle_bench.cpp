// Headless longitudinal / braking / steering bench for src/game/Vehicle.cpp.
//
// It uses only the API that is common to the pre-recovery model and the
// vehicle.tech-driven model, so the same source builds against both (pick the
// header with the include path):
//
//   tools/physics_ab.sh                     # builds and runs both models
//
// The ground map is left unset, which is flat asphalt: the terrain following
// and the boundary/climb rules are exercised by --drive instead.
//
// Output: one `RESULT <key> <value> <unit>` line per measurement.
#include "game/Vehicle.h"
#include <cstdio>
#include <cmath>
#include <cstring>

using namespace ht2;

namespace {

const float DT = 1.0f / 60.0f;

struct Run {
    Vehicle v;
    float t = 0;
    explicit Run(const char* veh) {
#ifdef HT2_NEW_VEHICLE
        if (veh && *veh) v.useVehicle(veh);
#else
        (void)veh;
#endif
        v.reset(0, 0, 0);
    }
    // advance `secs` of full-throttle-or-brake input; stop early when the speed
    // reaches `targetKmh` (returning the time) or when the truck has stopped
    // (`stopAtZero`, returning the time).
    float run(float secs, float throttle, float steer, float targetKmh = 0.0f,
              bool stopAtZero = false) {
        int steps = (int)(secs / DT);
        for (int i = 0; i < steps; ++i) {
            Vehicle::Input in;
            in.throttle = throttle;
            in.steer = steer;
            v.update(in, DT);
            t += DT;
            if (targetKmh > 0.0f && v.speedKmh() >= targetKmh) return t;
            if (stopAtZero && v.speedKmh() < 2.0f) return t;
        }
        return -1.0f;
    }
};

void result(const char* k, double val, const char* unit = "") {
    std::printf("RESULT %-28s %10.3f %s\n", k, val, unit);
}

} // namespace

int main(int argc, char** argv) {
    const char* veh = argc > 1 ? argv[1] : "";
    std::printf("# vehicle_bench: model=%s vehicle=%s tech=%s\n",
#ifdef HT2_NEW_VEHICLE
                "vehicle.tech",
#else
                "pre-recovery",
#endif
                *veh ? veh : "(default)",
#ifdef HT2_NEW_VEHICLE
                Vehicle::techSource().c_str()
#else
                "-"
#endif
    );

    // Reverse must produce negative displacement, then D must regain forward motion.
    {
        Run r(veh);
        r.run(5.0f, -1.0f, 0.0f);
        result("reverse_speed_kmh", r.v.speedKmh(), "km/h");
        result("reverse_z_m", r.v.z(), "m");
#ifdef HT2_NEW_VEHICLE
        if (r.v.gear() != -1 || r.v.speedKmh() >= -1.0f || r.v.z() >= -1.0f) return 2;
#endif
        r.run(5.0f, 1.0f, 0.0f);
        result("reverse_to_forward_kmh", r.v.speedKmh(), "km/h");
#ifdef HT2_NEW_VEHICLE
        if (r.v.gear() < 1 || r.v.speedKmh() <= 1.0f) return 3;
#endif
    }
    // ---- acceleration -----------------------------------------------------
    {
        Run r(veh);
        float t50 = -1, t60 = -1, t100 = -1;
        {
            Run a(veh); t50 = a.run(180.0f, 1.0f, 0.0f, 50.0f);
            result("0_50_kmh_s", t50, "s");
        }
        {
            Run a(veh); t60 = a.run(180.0f, 1.0f, 0.0f, 60.0f);
            result("0_60_kmh_s", t60, "s");
        }
        {
            Run a(veh); t100 = a.run(180.0f, 1.0f, 0.0f, 100.0f);
            result("0_100_kmh_s", t100, "s");
        }
        r.run(180.0f, 1.0f, 0.0f);
        result("top_speed_kmh", r.v.speedKmh(), "km/h");
        result("top_tach_rpm", r.v.rpm(), "rpm");
        result("top_gear", r.v.gear(), "");
        result("top_odo_m", r.v.odometer(), "m");
    }
    // ---- shift speeds -----------------------------------------------------
    {
        Run r(veh);
        int g = r.v.gear();
        for (int i = 0; i < 60 * 180; ++i) {
            Vehicle::Input in; in.throttle = 1.0f;
            r.v.update(in, DT);
            if (r.v.gear() != g) {
                char key[64];
                std::snprintf(key, sizeof key, "shift_%d_%d_kmh", g, r.v.gear());
                result(key, r.v.speedKmh(), "km/h");
                g = r.v.gear();
            }
        }
    }
    // ---- braking ----------------------------------------------------------
    {
        Run r(veh);
        r.run(180.0f, 1.0f, 0.0f, 60.0f);
        for (int i = 0; i < 60 * 20 && r.v.speedKmh() < 59.0f; ++i) {
            Vehicle::Input in; in.throttle = 1.0f; r.v.update(in, DT);
        }
        result("brake_60_from_kmh", r.v.speedKmh(), "km/h");
        float d0 = r.v.odometer(), t0 = r.t;
        r.run(60.0f, -1.0f, 0.0f, 0.0f, true);
        result("brake_60_m", r.v.odometer() - d0, "m");
        result("brake_60_s", r.t - t0, "s");
    }
    {
        Run r(veh);
        r.run(180.0f, 1.0f, 0.0f);
        float d0 = r.v.odometer(), t0 = r.t, v0 = r.v.speedKmh();
        r.run(60.0f, -1.0f, 0.0f, 0.0f, true);
        result("brake_top_from_kmh", v0, "km/h");
        result("brake_top_m", r.v.odometer() - d0, "m");
        result("brake_top_s", r.t - t0, "s");
    }
    // ---- coast-down -------------------------------------------------------
    {
        Run r(veh);
        r.run(180.0f, 1.0f, 0.0f);
        float v0 = r.v.speedKmh(), d0 = r.v.odometer();
        r.run(30.0f, 0.0f, 0.0f);
        result("coast_from_kmh", v0, "km/h");
        result("coast_30s_to_kmh", r.v.speedKmh(), "km/h");
        result("coast_30s_m", r.v.odometer() - d0, "m");
    }
    // ---- steering rate vs speed ------------------------------------------
    {
        const float speeds[] = {10, 20, 30, 40, 60, 80};
        for (float target : speeds) {
            Run r(veh);
            for (int i = 0; i < 60 * 240 && r.v.speedKmh() < target; ++i) {
                Vehicle::Input in; in.throttle = 1.0f; r.v.update(in, DT);
            }
            float y0 = r.v.yaw();
            for (int i = 0; i < 60; ++i) {         // 1 second at full lock
                Vehicle::Input in; in.throttle = 0.5f; in.steer = 1.0f;
                r.v.update(in, DT);
            }
            char key[64];
            std::snprintf(key, sizeof key, "yaw_rate_1s_at_%d_kmh", (int)target);
            result(key, r.v.yaw() - y0, "deg/s");
        }
    }
    return 0;
}

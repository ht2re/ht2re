// Locks the vehicle.tech fields whose destinations were recovered from
// king.exe (tank 0x508b0b, body_square 0x5098bf, breakpower 0x5087d8,
// horse_power 0x5090f3, price 0x509116) and the authored identity
// tank = fuelConsumption * kilometrage (0x508af4) on powered records.
#include "game/Vehicle.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static int g_checks = 0, g_failed = 0;

static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_failed;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
}

static bool near(float a, float b, float eps = 0.05f) {
    return std::fabs(a - b) <= eps;
}

int main() {
    using ht2::Vehicle;
    Vehicle zil;
    zil.useVehicle("ZIL");
    const Vehicle::TechSpec& z = zil.spec();
    check(z.section == "ZIL", "ZIL section");
    check(near(z.mass, 5100.0f), "ZIL mass 5100");
    check(near(z.tankLitres, 255.0f), "ZIL tank is record+0x6c = 255 l");
    check(near(zil.tankLitres(), 255.0f), "applySpec keeps the authored tank");
    check(near(z.fuelPerKm, 5.1f), "ZIL fuelConsumption 5.1");
    check(near(z.rangeKm, 50.0f), "ZIL kilometrage 50");
    check(near(z.tankLitres, z.fuelPerKm * z.rangeKm), "ZIL tank = consumption * range");
    check(near(z.bodySquare, 19.9f), "ZIL body_square 19.9 m^2");
    check(near(Vehicle::frontalArea(z.bodySquare, z.mass), 19.9f),
          "frontal area uses body_square, not the mass proxy");
    check(near(Vehicle::frontalArea(0.0f, 5100.0f), 0.6f * 5.1f + 2.0f),
          "missing body_square still uses the mass proxy");
    check(near(z.breakPower, 36000.0f), "ZIL breakpower stored at +0xc8");
    check(z.horsePower == 150, "ZIL horse_power int is the default 150");
    check(z.techPrice == 10000, "ZIL price int is the default 10000");
    check(near(z.powerHp, 255.0f), "torque input still reads the +0x6c float");

    const Vehicle::TechSpec* fiat = Vehicle::findTech("FIAT");
    const Vehicle::TechSpec* storm = Vehicle::findTech("STORM");
    check(fiat && near(fiat->tankLitres, 30.0f), "FIAT tank 30 l is below the old 40 l floor");
    check(storm && near(storm->tankLitres, 600.0f), "STORM tank 600 l is above the old 400 l cap");
    check(fiat && near(fiat->bodySquare, 2.0f), "FIAT body_square 2.0");
    check(storm && near(storm->bodySquare, 26.0f), "STORM body_square 26");

    int identity = 0, powered = 0;
    for (const Vehicle::TechSpec& s : Vehicle::techTable()) {
        if (s.fuelPerKm <= 0.01f) continue;
        ++powered;
        if (near(s.tankLitres, s.fuelPerKm * s.rangeKm, 0.2f)) ++identity;
    }
    check(powered >= 30 && identity == powered,
          "every powered record stores tank = fuelConsumption * kilometrage");

    // Loader identities that are not yet inputs of Vehicle::update.
    // +0xc4 = maxrotmomentum * 0.2 / (2 * tach_rps) at 0x508a39.
    // +0x70 is fuelTimeEndurance (0x508a1f). +0x90 is the first missing
    // gear index stored at 0x5089d9 (ZIL edi == 7).
    std::ifstream tech("original/vehicle.tech", std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(tech)), std::istreambuf_iterator<char>());
    const size_t rec0 = 0x39, recSize = 0x970;
    int derived = 0, derivedOk = 0, occupied = 0;
    float zilFte = 0, zilC4 = 0, zilTach = 0;
    int zilGear = 0;
    bool sawZil = false;
    auto f32 = [&](size_t off) {
        float v = 0;
        if (off + 4 <= raw.size()) std::memcpy(&v, raw.data() + off, 4);
        return v;
    };
    auto i32 = [&](size_t off) {
        std::int32_t v = 0;
        if (off + 4 <= raw.size()) std::memcpy(&v, raw.data() + off, 4);
        return (int)v;
    };
    for (size_t off = rec0; off + recSize <= raw.size(); off += recSize) {
        if (raw[off + 0x8a4] != '.') continue;
        ++occupied;
        float tach = f32(off + 0x10c);
        float mom = f32(off + 0xc0);
        float c4 = f32(off + 0xc4);
        if (tach > 0.5f && mom > 0.0f) {
            ++derived;
            float pred = mom * 0.2f / (2.0f * tach);
            if (near(c4, pred, std::max(1e-3f, std::fabs(pred) * 1e-4f))) ++derivedOk;
        }
        std::string section;
        {
            size_t n = off + 0x8c2, e = n;
            while (e < raw.size() && raw[e] != '\0') ++e;
            section.assign(raw.data() + n, e - n);
        }
        if (!sawZil && section == "ZIL") {
            sawZil = true;
            zilFte = f32(off + 0x70);
            zilC4 = c4;
            zilTach = tach;
            zilGear = i32(off + 0x90);
        }
    }
    check(occupied >= 40 && derived >= 30 && derivedOk == derived,
          "every occupied record stores +0xc4 = maxrotmomentum * 0.2 / (2 * tach)");
    check(sawZil && near(zilTach * 60.0f, 1600.0f, 0.05f), "ZIL tach_with_max_power is 1600 rpm as rev/s");
    check(sawZil && near(zilC4, 3.5625f, 1e-3f), "ZIL +0xc4 is 3.5625");
    check(sawZil && near(zilFte, 846.217f, 0.01f), "ZIL fuelTimeEndurance +0x70 is 846.217, not the 1200 default");
    check(sawZil && zilGear == 7, "ZIL +0x90 is the first missing gear index 7");

    std::printf("RESULT tech fields %d checks, %d failures\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}

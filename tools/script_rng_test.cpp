#include "script/EngineRandom.h"
#include <array>
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    // Oracle: original king.exe bytes executed with radare2 ESIL, starting at
    // 0x401450, followed by 64 calls to 0x4156e0. Crosses the 55-word wrap.
    constexpr std::array<uint32_t, 8> bounds = {
        1000000, 1000000, 300, 17, 55, 1, 0xffffffffu, 256};
    constexpr std::array<uint32_t, 64> expected = {
        126868,58232,188,4,29,0,1549231671,111,
        439392,773857,205,13,34,0,370329049,150,
        811702,972131,295,6,33,0,2862619107u,55,
        296019,885113,253,10,52,0,2534221651u,245,
        394854,912689,137,9,11,0,1967688935,37,
        421088,580396,272,16,25,0,609952668,26,
        534757,178678,95,15,34,0,1433222140,223,
        630674,537295,31,6,38,0,3230502792u,59};
    ht2::EngineRandom rng;
    if (argc == 2 && std::strcmp(argv[1], "--dump") == 0) {
        for (size_t i = 0; i < expected.size(); ++i)
            std::printf("%u\n", rng.bounded(bounds[i % bounds.size()]));
        return 0;
    }
    for (int pass = 0; pass < 2; ++pass) {
        rng.reset();
        for (size_t i = 0; i < expected.size(); ++i) {
            const auto actual = rng.bounded(bounds[i % bounds.size()]);
            if (actual != expected[i]) {
                std::fprintf(stderr, "original PRNG draw %zu: %u != %u\n",
                             i, actual, expected[i]);
                return 1;
            }
        }
    }
    try {
        (void)rng.bounded(0);
        return 1;
    } catch (const std::invalid_argument&) {}
    std::puts("script_rng_test: 129/129 checks passed (original byte oracle)");
}

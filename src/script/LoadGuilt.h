#pragma once
#include "EngineRandom.h"
#include <cstdint>
#include <vector>

namespace ht2 {

// Raw input to king.exe accountForGuilt (0x442850). Do not replace this state
// with a cargo's formafia tag: related records, order, rating and PRNG matter.
struct LoadGuiltCargo {
    uint32_t flags = 0;            // cargo definition +0x30 (formafia = 2)
    bool hasRelated = false;       // cargo record +4 != -1
    uint32_t relatedFlags = 0;
    double value40 = 0;            // cargo record +0x40; meaning not renamed yet
    int marker88 = -1;             // persisted in the actual cargo record
};
struct LoadGuiltState {
    double currentPlayerShare = 0; // player LIST NODE +0x300, payload +0x2f8
    uint32_t containerFlags = 0;   // node->container204 +0x28
    std::vector<LoadGuiltCargo> cargo; // container208 if valid, otherwise 204
    bool illegalWanted = false;   // output: player node +0xc8
    bool mafiaWanted = false;     // output: player node +0xc9
    bool suspiciousLoad = false;  // output: player node +0x108
};

// 0x44289c..0x442bcb, excluding diagnostic/SEH setup. Stateful flags are NOT
// independent per item: a later record can erase the result of an earlier one.
inline void accountForLoadGuilt(LoadGuiltState& state, EngineRandom& rng) {
    bool illegal = false, mafia = false;
    int marker = -1;
    for (auto& item : state.cargo) {
        if (item.hasRelated) illegal = true;
        if (((item.flags & 2) || (item.hasRelated && (item.relatedFlags & 2))) &&
            state.currentPlayerShare > 0.15)
            mafia = true;
        // 0x4429ba tests the accumulated flag, not this record's +4 field.
        if (!illegal) {
            if (mafia) {
                // x87 unordered also takes the '<' branch (C0 is set).
                const uint32_t threshold = !(item.value40 >= 10000.0) ? 500000 : 200000;
                if (rng.bounded(1000000) < threshold) mafia = false;
            }
        } else if (rng.bounded(1000000) < 200000) {
            illegal = false;
            mafia = false;
        } else if (mafia) {
            if (item.marker88 == -1) {
                // 0x442a99..0x442ae2 advances the RNG, but stores ZERO, not
                // the generated word, into +0x88. Preserve even that oddity.
                (void)rng.bounded(1);
                item.marker88 = 0;
            }
            marker = item.marker88;
        }
    }
    if (illegal && mafia) {
        if (marker == 1) illegal = false;
        else mafia = false;
    }
    state.illegalWanted = illegal;
    state.mafiaWanted = mafia;
    state.suspiciousLoad = (state.containerFlags & 2) && rng.bounded(1000000) < 500000;
}

} // namespace ht2

#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace ht2 {

// SDTE stream cipher used by every shipped .sch / .CNF file.
//
// Recovered from king.exe (init @0x40ee0, stream @0xb5640): a subtractive lagged
// Fibonacci generator over 55 dwords with lags 31 and the shared key 0x5CB0.
// The keystream byte is the low byte of the updated state word.
class Sdte {
public:
    static constexpr uint32_t KEY = 0x5CB0;
    // true when `data` starts with the "SDTE" magic
    static bool isEncrypted(const uint8_t* data, size_t n);
    // decrypt in place: returns the plaintext (input without the magic when it
    // was encrypted, otherwise a copy of the input)
    static std::vector<uint8_t> decrypt(const uint8_t* data, size_t n, uint32_t key = KEY);
};

} // namespace ht2

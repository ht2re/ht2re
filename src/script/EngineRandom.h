#pragma once
#include <array>
#include <cstdint>
#include <stdexcept>

namespace ht2 {

// king.exe 0x401450 / reset 0x403940, draw(bound) 0x4156e0.
// Unsigned 32-bit subtraction is intentional; do not use signed arithmetic
// or the modulus of the superficially similar Numerical Recipes generator.
class EngineRandom {
public:
    static constexpr uint32_t kInitialWord = 0x09a4ec86u;
    explicit EngineRandom(uint32_t word=kInitialWord) { reset(word); }
    // Non-default words are a port testing extension; the original reset uses
    // the fixed kInitialWord. Zero is a valid test word, not an alias for one.
    void reset(uint32_t word=kInitialWord) {
        words_.fill(0);
        words_[54]=word;
        uint32_t next=1;
        for(uint32_t offset=0;offset<1134;offset+=21) {
            const uint32_t index=offset%55;
            word-=next;
            words_[index]=next;
            next=word;
            word=words_[index];
        }
        for(int pass=0;pass<4;++pass)
            for(uint32_t i=0;i<55;++i)words_[i]-=words_[(i+31)%55];
        first_=0;second_=31;
    }
    uint32_t bounded(uint32_t bound) {
        if(!bound)throw std::invalid_argument("EngineRandom bound must be nonzero");
        first_=(first_+1)%55;second_=(second_+1)%55;
        words_[first_]-=words_[second_];
        return words_[first_]%bound;
    }
private:
    std::array<uint32_t,55> words_{};
    uint32_t first_=0,second_=31;
};

} // namespace ht2

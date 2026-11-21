// mini xoshiro256** random number generator
// needed to make sure the output is the same on every compiler and std lib, as
// std::uniform_int_distribution gives different numbers on different platforms
#pragma once

#include <cstdint>

namespace crdt {

// scrambles a 64-bit state into a mixed number (keeps state)
inline uint64_t splitmix64(uint64_t& state) {
    uint64_t mixed = (state += 0x9E3779B97F4A7C15ull);
    mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ull;
    mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBull;

    return mixed ^ (mixed >> 31);
}

// scramble for a single value (doesn't keep state)
inline uint64_t mix64(uint64_t value) { 
    return splitmix64(value);
}

class RandomGenerator {
public:
    explicit RandomGenerator(uint64_t seed = 1) {
        for (auto& word : state_) 
            word = splitmix64(seed);
    }

    uint64_t next() {
        const uint64_t result = rotate_left(state_[1] * 5, 7) * 9;
        const uint64_t shifted = state_[1] << 17;

        state_[2] ^= state_[0];
        state_[3] ^= state_[1];
        state_[1] ^= state_[2];
        state_[0] ^= state_[3];
        state_[2] ^= shifted;
        state_[3] = rotate_left(state_[3], 45);

        return result;
    }

    // number from 0 to bound-1
    uint64_t below(uint64_t bound) {
        if (bound <= 1) 
            return 0;

        // throw away the few top values that would make small results more likely
        uint64_t limit = UINT64_MAX - (UINT64_MAX % bound);
        uint64_t sample;

        do {
            sample = next();
        } while (sample >= limit);

        return sample % bound;
    }

    // a fraction from 0 (included) to 1 (excluded).
    double uniform() { 
        return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); 
    }

    // true with the given probability
    bool chance(double probability) { 
        return uniform() < probability; 
    }

private:
    uint64_t state_[4];

    static uint64_t rotate_left(uint64_t value, int bits) {
        return (value << bits) | (value >> (64 - bits));
    }
};

}  // namespace crdt

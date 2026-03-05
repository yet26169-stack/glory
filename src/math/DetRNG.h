#pragma once
#include <cstdint>

namespace glory {

/// xoshiro256** — passes PractRand, period 2^256-1, no platform-dependent FP.
class DetRNG {
public:
    explicit DetRNG(uint64_t seed);

    uint64_t next();

    /// Returns uniform integer in [0, n)
    uint64_t nextInt(uint64_t n);

private:
    uint64_t s[4];

    static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
    static uint64_t splitmix64(uint64_t& state);
};

} // namespace glory

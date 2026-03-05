#include "math/DetRNG.h"

namespace glory {

uint64_t DetRNG::splitmix64(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

DetRNG::DetRNG(uint64_t seed) {
    // Seed all four state words via splitmix64 for good diffusion
    s[0] = splitmix64(seed);
    s[1] = splitmix64(seed);
    s[2] = splitmix64(seed);
    s[3] = splitmix64(seed);
}

uint64_t DetRNG::next() {
    const uint64_t result = rotl(s[1] * 5, 7) * 9;
    const uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return result;
}

uint64_t DetRNG::nextInt(uint64_t n) {
    if (n == 0) return 0;
    // Debiased mod via Lemire's method
    uint64_t r = next();
    return r % n;
}

} // namespace glory

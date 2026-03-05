/// MurmurHash3 — public domain, Austin Appleby.
/// Adapted for Glory engine: adds hashSimState() over EnTT Sim* components.
#include "math/MurmurHash3.h"
#include "scene/Components.h"  // SimPosition, SimVelocity, SimRotation

#include <cstring>
#include <cstdlib>

namespace glory {

//-----------------------------------------------------------------------------
// MurmurHash3 x64 128-bit (portable)
//-----------------------------------------------------------------------------
static uint64_t fmix64(uint64_t k) {
    k ^= k >> 33;
    k *= 0xFF51AFD7ED558CCDULL;
    k ^= k >> 33;
    k *= 0xC4CEB9FE1A85EC53ULL;
    k ^= k >> 33;
    return k;
}

void MurmurHash3_x64_128(const void* key, int len, uint32_t seed, void* out) {
    const uint8_t* data  = static_cast<const uint8_t*>(key);
    const int      nblocks = len / 16;

    uint64_t h1 = seed;
    uint64_t h2 = seed;

    const uint64_t c1 = 0x87C37B91114253D5ULL;
    const uint64_t c2 = 0x4CF5AD432745937FULL;

    // Body
    const uint64_t* blocks = reinterpret_cast<const uint64_t*>(data);
    for (int i = 0; i < nblocks; i++) {
        uint64_t k1, k2;
        std::memcpy(&k1, blocks + i*2,     sizeof(k1));
        std::memcpy(&k2, blocks + i*2 + 1, sizeof(k2));

        k1 *= c1; k1 = (k1 << 31) | (k1 >> 33); k1 *= c2; h1 ^= k1;
        h1 = (h1 << 27) | (h1 >> 37); h1 += h2; h1 = h1 * 5 + 0x52DCE729ULL;

        k2 *= c2; k2 = (k2 << 33) | (k2 >> 31); k2 *= c1; h2 ^= k2;
        h2 = (h2 << 31) | (h2 >> 33); h2 += h1; h2 = h2 * 5 + 0x38495AB5ULL;
    }

    // Tail
    const uint8_t* tail = data + nblocks * 16;
    uint64_t k1 = 0, k2 = 0;
    switch (len & 15) {
    case 15: k2 ^= uint64_t(tail[14]) << 48; [[fallthrough]];
    case 14: k2 ^= uint64_t(tail[13]) << 40; [[fallthrough]];
    case 13: k2 ^= uint64_t(tail[12]) << 32; [[fallthrough]];
    case 12: k2 ^= uint64_t(tail[11]) << 24; [[fallthrough]];
    case 11: k2 ^= uint64_t(tail[10]) << 16; [[fallthrough]];
    case 10: k2 ^= uint64_t(tail[ 9]) <<  8; [[fallthrough]];
    case  9: k2 ^= uint64_t(tail[ 8]);
             k2 *= c2; k2 = (k2 << 33) | (k2 >> 31); k2 *= c1; h2 ^= k2; [[fallthrough]];
    case  8: k1 ^= uint64_t(tail[ 7]) << 56; [[fallthrough]];
    case  7: k1 ^= uint64_t(tail[ 6]) << 48; [[fallthrough]];
    case  6: k1 ^= uint64_t(tail[ 5]) << 40; [[fallthrough]];
    case  5: k1 ^= uint64_t(tail[ 4]) << 32; [[fallthrough]];
    case  4: k1 ^= uint64_t(tail[ 3]) << 24; [[fallthrough]];
    case  3: k1 ^= uint64_t(tail[ 2]) << 16; [[fallthrough]];
    case  2: k1 ^= uint64_t(tail[ 1]) <<  8; [[fallthrough]];
    case  1: k1 ^= uint64_t(tail[ 0]);
             k1 *= c1; k1 = (k1 << 31) | (k1 >> 33); k1 *= c2; h1 ^= k1;
    }

    h1 ^= static_cast<uint64_t>(len);
    h2 ^= static_cast<uint64_t>(len);
    h1 += h2;
    h2 += h1;
    h1 = fmix64(h1);
    h2 = fmix64(h2);
    h1 += h2;
    h2 += h1;

    static_cast<uint64_t*>(out)[0] = h1;
    static_cast<uint64_t*>(out)[1] = h2;
}

uint64_t hashSimState(const entt::registry& reg) {
    uint64_t rollingHash[2] = {0, 0};
    uint32_t seed = 0x12345678u;

    // Hash SimPosition components
    reg.view<const SimPosition>().each([&](entt::entity e, const SimPosition& p) {
        uint64_t buf[4];
        buf[0] = static_cast<uint64_t>(entt::to_integral(e));
#ifdef GLORY_DETERMINISTIC
        buf[1] = static_cast<uint64_t>(p.value.x.raw);
        buf[2] = static_cast<uint64_t>(p.value.y.raw);
        buf[3] = static_cast<uint64_t>(p.value.z.raw);
#else
        static_assert(sizeof(float) == 4);
        uint32_t tmp;
        std::memcpy(&tmp, &p.value.x, 4); buf[1] = tmp;
        std::memcpy(&tmp, &p.value.y, 4); buf[2] = tmp;
        std::memcpy(&tmp, &p.value.z, 4); buf[3] = tmp;
#endif
        uint64_t h[2];
        MurmurHash3_x64_128(buf, sizeof(buf), seed, h);
        rollingHash[0] ^= h[0];
        rollingHash[1] ^= h[1];
    });

    // Hash SimVelocity components
    reg.view<const SimVelocity>().each([&](entt::entity e, const SimVelocity& v) {
        uint64_t buf[4];
        buf[0] = static_cast<uint64_t>(entt::to_integral(e));
#ifdef GLORY_DETERMINISTIC
        buf[1] = static_cast<uint64_t>(v.value.x.raw);
        buf[2] = static_cast<uint64_t>(v.value.y.raw);
        buf[3] = static_cast<uint64_t>(v.value.z.raw);
#else
        uint32_t tmp;
        std::memcpy(&tmp, &v.value.x, 4); buf[1] = tmp;
        std::memcpy(&tmp, &v.value.y, 4); buf[2] = tmp;
        std::memcpy(&tmp, &v.value.z, 4); buf[3] = tmp;
#endif
        uint64_t h[2];
        MurmurHash3_x64_128(buf, sizeof(buf), seed, h);
        rollingHash[0] ^= h[0];
        rollingHash[1] ^= h[1];
    });

    return rollingHash[0] ^ (rollingHash[1] << 32) ^ (rollingHash[1] >> 32);
}

} // namespace glory

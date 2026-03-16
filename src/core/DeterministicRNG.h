#pragma once
/// Deterministic PRNG: xoshiro256** — fast, high-quality, platform-independent.
///
/// This generator produces identical sequences given the same seed on every
/// compiler/platform, because it uses only shifts, rotations, and multiplies
/// on uint64_t (no floats involved in the state machine).

#include <cstdint>
#include "core/FixedPoint.h"

namespace glory {

class DeterministicRNG {
public:
    DeterministicRNG() { seed(1); }
    explicit DeterministicRNG(uint64_t s) { seed(s); }

    /// Re-seed the generator.  Uses SplitMix64 to expand a single u64 into
    /// the four-word state required by xoshiro256**.
    void seed(uint64_t s) {
        // SplitMix64 expansion
        auto sm = [&]() -> uint64_t {
            s += 0x9E3779B97F4A7C15ULL;
            uint64_t z = s;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            return z ^ (z >> 31);
        };
        m_state[0] = sm();
        m_state[1] = sm();
        m_state[2] = sm();
        m_state[3] = sm();
    }

    /// Raw 64-bit output.
    uint64_t next() {
        const uint64_t result = rotl(m_state[1] * 5, 7) * 9;
        const uint64_t t = m_state[1] << 17;

        m_state[2] ^= m_state[0];
        m_state[3] ^= m_state[1];
        m_state[1] ^= m_state[2];
        m_state[0] ^= m_state[3];

        m_state[2] ^= t;
        m_state[3] = rotl(m_state[3], 45);

        return result;
    }

    /// Uniform integer in [0, max).
    uint32_t nextU32(uint32_t max) {
        if (max == 0) return 0;
        return static_cast<uint32_t>(next() % max);
    }

    /// Uniform Fixed32 in [0, 1).
    Fixed32 nextFixed01() {
        // Take upper 16 bits of raw → [0, 65535] → divide by 65536
        uint32_t frac = static_cast<uint32_t>(next() >> 48); // 0..65535
        return Fixed32::fromRaw(static_cast<int32_t>(frac));  // 0..0.99998
    }

    /// Uniform Fixed32 in [lo, hi).
    Fixed32 nextFixedRange(Fixed32 lo, Fixed32 hi) {
        return lo + nextFixed01() * (hi - lo);
    }

    /// Uniform float in [0, 1) — for rendering-only use (non-deterministic display).
    float nextFloat01() {
        return static_cast<float>(next() >> 11) * 0x1.0p-53f;
    }

    /// Get/set full state for snapshotting (rollback, replay).
    struct State { uint64_t s[4]; };
    State getState() const { return { { m_state[0], m_state[1], m_state[2], m_state[3] } }; }
    void setState(const State& st) {
        m_state[0] = st.s[0]; m_state[1] = st.s[1];
        m_state[2] = st.s[2]; m_state[3] = st.s[3];
    }

private:
    uint64_t m_state[4]{};

    static constexpr uint64_t rotl(uint64_t x, int k) {
        return (x << k) | (x >> (64 - k));
    }
};

} // namespace glory

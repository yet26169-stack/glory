#pragma once
#include <cstdint>
#include <cmath>
#if defined(_MSC_VER)
  #include <intrin.h>
#endif
#include <glm/glm.hpp>

namespace glory {

/// 16-bit fractional fixed-point Q47.16 type.
class Fixed64 {
public:
    static constexpr int     FRAC_BITS = 16;
    static constexpr int64_t ONE       = int64_t(1) << FRAC_BITS;

    int64_t raw = 0;

    constexpr Fixed64() = default;
    explicit constexpr Fixed64(int64_t r) : raw(r) {}

    static constexpr Fixed64 fromInt(int32_t v)  { return Fixed64(int64_t(v) << FRAC_BITS); }
    static constexpr Fixed64 fromFloat(float f)  { return Fixed64(int64_t(f * ONE)); }

    float toFloat() const { return float(raw) / float(ONE); }

    Fixed64 operator+(Fixed64 o) const { return Fixed64(raw + o.raw); }
    Fixed64 operator-(Fixed64 o) const { return Fixed64(raw - o.raw); }
    Fixed64 operator-()          const { return Fixed64(-raw); }

    Fixed64 operator*(Fixed64 o) const {
#if defined(__GNUC__) || defined(__clang__)
        __int128 temp = static_cast<__int128>(raw) * o.raw;
        return Fixed64(static_cast<int64_t>(temp >> FRAC_BITS));
#elif defined(_MSC_VER)
        bool negative = (raw < 0) != (o.raw < 0);
        uint64_t absA = static_cast<uint64_t>(raw  < 0 ? -raw  : raw);
        uint64_t absB = static_cast<uint64_t>(o.raw < 0 ? -o.raw : o.raw);
        uint64_t hi;
        uint64_t lo = _umul128(absA, absB, &hi);
        uint64_t result = (hi << (64 - FRAC_BITS)) | (lo >> FRAC_BITS);
        return Fixed64(negative ? -static_cast<int64_t>(result)
                                :  static_cast<int64_t>(result));
#else
        return Fixed64((raw * o.raw) >> FRAC_BITS);
#endif
    }

    Fixed64 operator/(Fixed64 o) const {
#if defined(__GNUC__) || defined(__clang__)
        __int128 n = static_cast<__int128>(raw) << FRAC_BITS;
        return Fixed64(static_cast<int64_t>(n / o.raw));
#elif defined(_MSC_VER)
        int64_t hi  = raw >> (64 - FRAC_BITS);
        uint64_t lo = static_cast<uint64_t>(raw) << FRAC_BITS;
        int64_t rem;
        int64_t q = _div128(hi, static_cast<int64_t>(lo), o.raw, &rem);
        return Fixed64(q);
#else
        return Fixed64((raw << FRAC_BITS) / o.raw);
#endif
    }

    bool operator==(Fixed64 o) const { return raw == o.raw; }
    bool operator!=(Fixed64 o) const { return raw != o.raw; }
    bool operator< (Fixed64 o) const { return raw <  o.raw; }
    bool operator<=(Fixed64 o) const { return raw <= o.raw; }
    bool operator> (Fixed64 o) const { return raw >  o.raw; }
    bool operator>=(Fixed64 o) const { return raw >= o.raw; }

    Fixed64& operator+=(Fixed64 o) { raw += o.raw; return *this; }
    Fixed64& operator-=(Fixed64 o) { raw -= o.raw; return *this; }
    Fixed64& operator*=(Fixed64 o) { *this = *this * o; return *this; }
    Fixed64& operator/=(Fixed64 o) { *this = *this / o; return *this; }
};

/// Fixed-point square root (Newton–Raphson, ~16-bit precision).
Fixed64 fixedSqrt(Fixed64 x);

/// Fixed-point atan2 approximation (CORDIC-style, ~0.01 rad error).
Fixed64 fixedAtan2(Fixed64 y, Fixed64 x);

struct FixedVec3 {
    Fixed64 x, y, z;
    FixedVec3 operator+(const FixedVec3& o) const { return {x+o.x, y+o.y, z+o.z}; }
    FixedVec3 operator-(const FixedVec3& o) const { return {x-o.x, y-o.y, z-o.z}; }
    FixedVec3 operator*(Fixed64 s)          const { return {x*s, y*s, z*s}; }
};

// Toggle between float (fast dev) and Fixed64 (deterministic prod) via CMake
#ifdef GLORY_DETERMINISTIC
    using SimFloat = Fixed64;
    using SimVec3  = FixedVec3;
#else
    using SimFloat = float;
    using SimVec3  = glm::vec3;
#endif

} // namespace glory

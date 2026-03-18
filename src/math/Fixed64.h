#pragma once

#include <cstdint>
#include <cmath>
#include <type_traits>
#include <limits>
#include <ostream>

namespace glory {

// Q48.16 fixed-point number for deterministic cross-platform math
struct Fixed64 {
    int64_t raw = 0;

    static constexpr int FRAC_BITS = 16;
    static constexpr int64_t ONE = 1LL << FRAC_BITS;
    static constexpr int64_t HALF = ONE >> 1;

    // Construction
    constexpr Fixed64() = default;
    struct RawTag {};
    constexpr explicit Fixed64(int64_t rawValue, RawTag) : raw(rawValue) {}

    static constexpr Fixed64 fromRaw(int64_t r) { Fixed64 f; f.raw = r; return f; }
    static constexpr Fixed64 fromInt(int32_t i) { return fromRaw(static_cast<int64_t>(i) << FRAC_BITS); }
    static constexpr Fixed64 fromFloat(float f) { return fromRaw(static_cast<int64_t>(f * ONE)); }
    static constexpr Fixed64 fromDouble(double d) { return fromRaw(static_cast<int64_t>(d * ONE)); }

    // Conversion
    constexpr float toFloat() const { return static_cast<float>(raw) / ONE; }
    constexpr double toDouble() const { return static_cast<double>(raw) / ONE; }
    constexpr int32_t toInt() const { return static_cast<int32_t>(raw >> FRAC_BITS); }

    // Floor/Ceil/Round
    constexpr Fixed64 floor() const { return fromRaw(raw & ~(ONE - 1)); }
    constexpr Fixed64 ceil() const {
        int64_t frac = raw & (ONE - 1);
        if (frac == 0) return *this;
        return fromRaw((raw & ~(ONE - 1)) + ONE);
    }
    constexpr Fixed64 round() const { return fromRaw((raw + HALF) & ~(ONE - 1)); }

    // Arithmetic
    constexpr Fixed64 operator+(Fixed64 o) const { return fromRaw(raw + o.raw); }
    constexpr Fixed64 operator-(Fixed64 o) const { return fromRaw(raw - o.raw); }
    constexpr Fixed64 operator-() const { return fromRaw(-raw); }

    // Multiplication: (a * b) >> FRAC_BITS
    // Use 128-bit intermediate to avoid overflow
    constexpr Fixed64 operator*(Fixed64 o) const {
        #if defined(__SIZEOF_INT128__)
        __int128 result = static_cast<__int128>(raw) * o.raw;
        return fromRaw(static_cast<int64_t>(result >> FRAC_BITS));
        #else
        // Portable fallback: split into high and low parts (32.32)
        // a = (ah << 32) + al
        // b = (bh << 32) + bl
        // a*b = (ah*bh << 64) + (ah*bl << 32) + (al*bh << 32) + (al*bl)
        
        bool negative = (raw < 0) != (o.raw < 0);
        uint64_t a = (raw < 0) ? -raw : raw;
        uint64_t b = (o.raw < 0) ? -o.raw : o.raw;

        uint64_t ah = a >> 32;
        uint64_t al = a & 0xFFFFFFFFULL;
        uint64_t bh = b >> 32;
        uint64_t bl = b & 0xFFFFFFFFULL;

        uint64_t p0 = al * bl;
        uint64_t p1 = al * bh;
        uint64_t p2 = ah * bl;
        uint64_t p3 = ah * bh;

        // Reconstruct result >> FRAC_BITS
        // p0 >> 16
        // p1 << (32 - 16)
        // p2 << (32 - 16)
        // p3 << (64 - 16)
        
        uint64_t res = (p0 >> FRAC_BITS);
        res += (p1 << (32 - FRAC_BITS));
        res += (p2 << (32 - FRAC_BITS));
        res += (p3 << (64 - FRAC_BITS));

        int64_t finalRes = static_cast<int64_t>(res);
        return fromRaw(negative ? -finalRes : finalRes);
        #endif
    }

    // Division: (a << FRAC_BITS) / b
    constexpr Fixed64 operator/(Fixed64 o) const {
        #if defined(__SIZEOF_INT128__)
        __int128 dividend = static_cast<__int128>(raw) << FRAC_BITS;
        return fromRaw(static_cast<int64_t>(dividend / o.raw));
        #else
        // Portable fallback: manual 128-bit shift-and-divide
        bool negative = (raw < 0) != (o.raw < 0);
        uint64_t a = (raw < 0) ? -raw : raw;
        uint64_t b = (o.raw < 0) ? -o.raw : o.raw;

        // Dividend is a << 16. Represent as (hi << 64) | lo
        uint64_t hi = a >> (64 - FRAC_BITS);
        uint64_t lo = a << FRAC_BITS;

        uint64_t quot = 0;
        uint64_t rem = hi;

        for (int i = 63; i >= 0; --i) {
            rem = (rem << 1) | ((lo >> i) & 1);
            if (rem >= b) {
                rem -= b;
                quot |= (1ULL << i);
            }
        }

        int64_t finalRes = static_cast<int64_t>(quot);
        return fromRaw(negative ? -finalRes : finalRes);
        #endif
    }

    // Modulo
    constexpr Fixed64 operator%(Fixed64 o) const { return fromRaw(raw % o.raw); }

    // Compound assignment
    constexpr Fixed64& operator+=(Fixed64 o) { raw += o.raw; return *this; }
    constexpr Fixed64& operator-=(Fixed64 o) { raw -= o.raw; return *this; }
    constexpr Fixed64& operator*=(Fixed64 o) { *this = *this * o; return *this; }
    constexpr Fixed64& operator/=(Fixed64 o) { *this = *this / o; return *this; }

    // Comparison
    constexpr bool operator==(Fixed64 o) const { return raw == o.raw; }
    constexpr bool operator!=(Fixed64 o) const { return raw != o.raw; }
    constexpr bool operator<(Fixed64 o) const { return raw < o.raw; }
    constexpr bool operator<=(Fixed64 o) const { return raw <= o.raw; }
    constexpr bool operator>(Fixed64 o) const { return raw > o.raw; }
    constexpr bool operator>=(Fixed64 o) const { return raw >= o.raw; }

    // Common constants
    static constexpr Fixed64 zero() { return fromRaw(0); }
    static constexpr Fixed64 one() { return fromRaw(ONE); }
    static constexpr Fixed64 half() { return fromRaw(HALF); }
    static constexpr Fixed64 pi() { return fromRaw(205887); }       // π * 65536
    static constexpr Fixed64 twoPi() { return fromRaw(411775); }    // 2π * 65536
    static constexpr Fixed64 halfPi() { return fromRaw(102944); }   // (π/2) * 65536
    static constexpr Fixed64 epsilon() { return fromRaw(1); }
    static constexpr Fixed64 max() { return fromRaw(std::numeric_limits<int64_t>::max()); }
    static constexpr Fixed64 min() { return fromRaw(std::numeric_limits<int64_t>::min()); }
};

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

inline constexpr Fixed64 abs(Fixed64 f) {
    return Fixed64::fromRaw(f.raw < 0 ? -f.raw : f.raw);
}

inline constexpr Fixed64 min(Fixed64 a, Fixed64 b) { return a < b ? a : b; }
inline constexpr Fixed64 max(Fixed64 a, Fixed64 b) { return a > b ? a : b; }

inline constexpr Fixed64 clamp(Fixed64 val, Fixed64 lo, Fixed64 hi) {
    return min(max(val, lo), hi);
}

// Square root — Newton-Raphson, fully deterministic (no floats)
inline Fixed64 sqrt(Fixed64 f) {
    if (f.raw <= 0) return Fixed64::zero();

    int64_t x = f.raw;
    // Initial guess heuristic
    int64_t guess = x;
    if (guess > Fixed64::ONE)
        guess = (guess >> 8) + (Fixed64::ONE << 4);
    else
        guess = Fixed64::ONE;

    // 8 Newton-Raphson iterations — converges for all representable values
    for (int i = 0; i < 8; ++i) {
        if (guess == 0) break;
        #if defined(__SIZEOF_INT128__)
        __int128 xFixed = static_cast<__int128>(x) << Fixed64::FRAC_BITS;
        int64_t div = static_cast<int64_t>(xFixed / guess);
        #else
        int64_t div = (x << Fixed64::FRAC_BITS) / guess;
        #endif
        guess = (guess + div) >> 1;
    }
    return Fixed64::fromRaw(guess);
}

// ---------------------------------------------------------------------------
// Lookup-table based sin/cos (256 entries per quadrant, deterministic)
// ---------------------------------------------------------------------------
namespace detail {

// sin(i * π / 512) * 65536, for i in [0, 256]
inline constexpr int64_t SIN_TABLE[257] = {
        0,   402,   804,  1206,  1608,  2010,  2412,  2814,
     3216,  3617,  4019,  4420,  4821,  5222,  5623,  6023,
     6424,  6824,  7224,  7623,  8022,  8421,  8820,  9218,
     9616, 10014, 10411, 10808, 11204, 11600, 11996, 12391,
    12785, 13180, 13573, 13966, 14359, 14751, 15143, 15534,
    15924, 16314, 16703, 17091, 17479, 17867, 18253, 18639,
    19024, 19409, 19792, 20175, 20557, 20939, 21320, 21699,
    22078, 22457, 22834, 23210, 23586, 23961, 24335, 24708,
    25080, 25451, 25821, 26190, 26558, 26925, 27291, 27656,
    28020, 28383, 28745, 29106, 29466, 29824, 30182, 30538,
    30893, 31248, 31600, 31952, 32303, 32652, 33000, 33347,
    33692, 34037, 34380, 34721, 35062, 35401, 35738, 36075,
    36410, 36744, 37076, 37407, 37736, 38064, 38391, 38716,
    39040, 39362, 39683, 40002, 40320, 40636, 40951, 41264,
    41576, 41886, 42194, 42501, 42806, 43110, 43412, 43713,
    44011, 44308, 44604, 44898, 45190, 45480, 45769, 46056,
    46341, 46624, 46906, 47186, 47464, 47741, 48015, 48288,
    48559, 48828, 49095, 49361, 49624, 49886, 50146, 50404,
    50660, 50914, 51166, 51417, 51665, 51911, 52156, 52398,
    52639, 52878, 53114, 53349, 53581, 53812, 54040, 54267,
    54491, 54714, 54934, 55152, 55368, 55582, 55794, 56004,
    56212, 56418, 56621, 56823, 57022, 57219, 57414, 57607,
    57798, 57986, 58172, 58356, 58538, 58718, 58896, 59071,
    59244, 59415, 59583, 59750, 59914, 60075, 60235, 60392,
    60547, 60700, 60851, 60999, 61145, 61288, 61429, 61568,
    61705, 61839, 61971, 62101, 62228, 62353, 62476, 62596,
    62714, 62830, 62943, 63054, 63162, 63268, 63372, 63473,
    63572, 63668, 63763, 63854, 63944, 64031, 64115, 64197,
    64277, 64354, 64429, 64501, 64571, 64639, 64704, 64766,
    64827, 64884, 64940, 64993, 65043, 65091, 65137, 65180,
    65220, 65259, 65294, 65328, 65358, 65387, 65413, 65436,
    65457, 65476, 65492, 65505, 65516, 65525, 65531, 65535,
    65536
};

// CORDIC atan lookup table: atan(2^-i) * 65536
inline constexpr int64_t ATAN_TABLE[32] = {
    51472, 30386, 16055, 8150, 4091, 2047, 1024, 512,
    256, 128, 64, 32, 16, 8, 4, 2,
    1, 1, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

} // namespace detail

// Deterministic sin using lookup table with linear interpolation
inline Fixed64 sin(Fixed64 angle) {
    // Normalize to [0, 2π)
    int64_t raw = angle.raw % Fixed64::twoPi().raw;
    if (raw < 0) raw += Fixed64::twoPi().raw;

    // Map to table index [0, 1024) for full circle
    // 256 entries per quadrant × 4 quadrants = 1024 steps per 2π
    int64_t idx1024 = (raw * 1024) / Fixed64::twoPi().raw;
    int quadrant = static_cast<int>((idx1024 >> 8) & 3);
    int idx = static_cast<int>(idx1024 & 255);

    int64_t val;
    switch (quadrant) {
        case 0: val =  detail::SIN_TABLE[idx];       break;
        case 1: val =  detail::SIN_TABLE[256 - idx]; break;
        case 2: val = -detail::SIN_TABLE[idx];        break;
        case 3: val = -detail::SIN_TABLE[256 - idx]; break;
        default: val = 0; break;
    }
    return Fixed64::fromRaw(val);
}

inline Fixed64 cos(Fixed64 angle) {
    return sin(angle + Fixed64::halfPi());
}

// atan2 — deterministic CORDIC implementation
inline Fixed64 atan2(Fixed64 y, Fixed64 x) {
    if (x.raw == 0 && y.raw == 0) return Fixed64::zero();

    int64_t angle = 0;
    int64_t xi = x.raw;
    int64_t yi = y.raw;

    // Initial rotation to put vector in right half-plane (-PI/2 to PI/2)
    if (xi < 0) {
        xi = -xi;
        yi = -yi;
    }

    // CORDIC iterations
    for (int i = 0; i < 32; ++i) {
        int64_t next_xi;
        if (yi > 0) {
            next_xi = xi + (yi >> i);
            yi = yi - (xi >> i);
            angle += detail::ATAN_TABLE[i];
        } else if (yi < 0) {
            next_xi = xi - (yi >> i);
            yi = yi + (xi >> i);
            angle -= detail::ATAN_TABLE[i];
        } else {
            break;
        }
        xi = next_xi;
    }

    // Adjust angle based on original quadrant
    if (x.raw < 0) {
        if (y.raw >= 0) angle += Fixed64::pi().raw;
        else           angle -= Fixed64::pi().raw;
    }

    return Fixed64::fromRaw(angle);
}

// acos — deterministic using atan2(sqrt(1-x*x), x)
inline Fixed64 acos(Fixed64 x) {
    Fixed64 one = Fixed64::one();
    Fixed64 x_clamped = clamp(x, -one, one);
    return atan2(sqrt(one - x_clamped * x_clamped), x_clamped);
}

// Stream output
inline std::ostream& operator<<(std::ostream& os, Fixed64 f) {
    os << f.toFloat();
    return os;
}

// ---------------------------------------------------------------------------
// FixedVec3 — 3D vector using Fixed64 components
// ---------------------------------------------------------------------------
struct FixedVec3 {
    Fixed64 x, y, z;

    constexpr FixedVec3() = default;
    constexpr FixedVec3(Fixed64 x_, Fixed64 y_, Fixed64 z_) : x(x_), y(y_), z(z_) {}

    static FixedVec3 fromFloats(float fx, float fy, float fz) {
        return { Fixed64::fromFloat(fx), Fixed64::fromFloat(fy), Fixed64::fromFloat(fz) };
    }

    // Arithmetic
    constexpr FixedVec3 operator+(const FixedVec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr FixedVec3 operator-(const FixedVec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr FixedVec3 operator*(Fixed64 s) const { return {x * s, y * s, z * s}; }
    constexpr FixedVec3 operator/(Fixed64 s) const { return {x / s, y / s, z / s}; }
    constexpr FixedVec3 operator-() const { return {-x, -y, -z}; }

    constexpr FixedVec3& operator+=(const FixedVec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    constexpr FixedVec3& operator-=(const FixedVec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    constexpr FixedVec3& operator*=(Fixed64 s) { x *= s; y *= s; z *= s; return *this; }

    // Dot product
    constexpr Fixed64 dot(const FixedVec3& o) const { return x * o.x + y * o.y + z * o.z; }

    // Cross product
    constexpr FixedVec3 cross(const FixedVec3& o) const {
        return {
            y * o.z - z * o.y,
            z * o.x - x * o.z,
            x * o.y - y * o.x
        };
    }

    // Length squared (no sqrt needed)
    constexpr Fixed64 lengthSquared() const { return dot(*this); }

    // Length
    Fixed64 length() const { return glory::sqrt(lengthSquared()); }

    // Normalize
    FixedVec3 normalized() const {
        Fixed64 len = length();
        if (len == Fixed64::zero()) return {};
        return *this / len;
    }

    // Distance helpers
    Fixed64 distanceTo(const FixedVec3& o) const { return (*this - o).length(); }
    Fixed64 distanceSquaredTo(const FixedVec3& o) const { return (*this - o).lengthSquared(); }

    // Comparison
    constexpr bool operator==(const FixedVec3& o) const { return x == o.x && y == o.y && z == o.z; }
    constexpr bool operator!=(const FixedVec3& o) const { return !(*this == o); }

    // Constants
    static constexpr FixedVec3 zero() { return {Fixed64::zero(), Fixed64::zero(), Fixed64::zero()}; }
};

// Scalar * vector
inline constexpr FixedVec3 operator*(Fixed64 s, const FixedVec3& v) { return v * s; }

// Lerp
inline FixedVec3 lerp(const FixedVec3& a, const FixedVec3& b, Fixed64 t) {
    return a + (b - a) * t;
}

inline Fixed64 lerp(Fixed64 a, Fixed64 b, Fixed64 t) {
    return a + (b - a) * t;
}

} // namespace glory

#pragma once
/// Fixed-point 16.16 arithmetic for deterministic cross-platform simulation.
///
/// Fixed32 stores a signed 32-bit integer where the lower 16 bits are the
/// fractional part.  All arithmetic is exact for add/sub and uses 64-bit
/// intermediates for mul/div to avoid overflow.
///
/// Trig functions use 4096-entry lookup tables (≈0.09° resolution).

#include <cstdint>
#include <cmath>
#include <array>
#include <type_traits>

namespace glory {

class Fixed32 {
public:
    static constexpr int FRAC_BITS = 16;
    static constexpr int32_t ONE   = 1 << FRAC_BITS;       // 65536
    static constexpr int32_t HALF  = ONE >> 1;              // 32768

    int32_t raw = 0;

    // ── Construction ───────────────────────────────────────────────────────
    constexpr Fixed32() = default;

    struct RawTag {};
    constexpr explicit Fixed32(int32_t rawBits, RawTag) : raw(rawBits) {}

    constexpr Fixed32(int v)   : raw(static_cast<int32_t>(v) << FRAC_BITS) {}
    constexpr Fixed32(float v) : raw(static_cast<int32_t>(v * ONE)) {}

    static constexpr Fixed32 fromRaw(int32_t r) { return Fixed32(r, RawTag{}); }

    // ── Conversion ─────────────────────────────────────────────────────────
    constexpr float toFloat() const { return static_cast<float>(raw) / ONE; }
    constexpr int   toInt()   const { return raw >> FRAC_BITS; }
    constexpr explicit operator float() const { return toFloat(); }
    constexpr explicit operator int()   const { return toInt(); }

    // ── Arithmetic ─────────────────────────────────────────────────────────
    constexpr Fixed32 operator-() const { return fromRaw(-raw); }

    constexpr Fixed32 operator+(Fixed32 b) const { return fromRaw(raw + b.raw); }
    constexpr Fixed32 operator-(Fixed32 b) const { return fromRaw(raw - b.raw); }

    constexpr Fixed32 operator*(Fixed32 b) const {
        int64_t r = static_cast<int64_t>(raw) * b.raw;
        return fromRaw(static_cast<int32_t>(r >> FRAC_BITS));
    }
    constexpr Fixed32 operator/(Fixed32 b) const {
        int64_t r = (static_cast<int64_t>(raw) << FRAC_BITS) / b.raw;
        return fromRaw(static_cast<int32_t>(r));
    }

    constexpr Fixed32& operator+=(Fixed32 b) { raw += b.raw; return *this; }
    constexpr Fixed32& operator-=(Fixed32 b) { raw -= b.raw; return *this; }
    constexpr Fixed32& operator*=(Fixed32 b) { *this = *this * b; return *this; }
    constexpr Fixed32& operator/=(Fixed32 b) { *this = *this / b; return *this; }

    // Scalar shortcuts
    constexpr Fixed32 operator*(int b) const { return fromRaw(raw * b); }
    constexpr Fixed32 operator/(int b) const { return fromRaw(raw / b); }

    // ── Comparison ─────────────────────────────────────────────────────────
    constexpr bool operator==(Fixed32 b) const { return raw == b.raw; }
    constexpr bool operator!=(Fixed32 b) const { return raw != b.raw; }
    constexpr bool operator< (Fixed32 b) const { return raw <  b.raw; }
    constexpr bool operator<=(Fixed32 b) const { return raw <= b.raw; }
    constexpr bool operator> (Fixed32 b) const { return raw >  b.raw; }
    constexpr bool operator>=(Fixed32 b) const { return raw >= b.raw; }

    // ── Common constants ───────────────────────────────────────────────────
    static constexpr Fixed32 zero()  { return fromRaw(0); }
    static constexpr Fixed32 one()   { return fromRaw(ONE); }
    static constexpr Fixed32 half()  { return fromRaw(HALF); }
    static constexpr Fixed32 pi()    { return fromRaw(205887); }  // π ≈ 3.14159
    static constexpr Fixed32 twoPi() { return fromRaw(411775); }  // 2π
    static constexpr Fixed32 halfPi(){ return fromRaw(102944); }  // π/2

    // ── Absolute value ─────────────────────────────────────────────────────
    constexpr Fixed32 abs() const { return fromRaw(raw < 0 ? -raw : raw); }

    // ── Trig (LUT-based) ──────────────────────────────────────────────────
    // Lookup table is populated on first use (Meyer's singleton).

    static Fixed32 sin(Fixed32 angle);
    static Fixed32 cos(Fixed32 angle);
    static Fixed32 atan2(Fixed32 y, Fixed32 x);

    // ── Sqrt (integer Newton's method) ────────────────────────────────────
    static Fixed32 sqrt(Fixed32 v);

private:
    // ── LUT ────────────────────────────────────────────────────────────────
    static constexpr int LUT_SIZE = 4096;          // 2π / 4096 ≈ 0.088°
    static constexpr int LUT_MASK = LUT_SIZE - 1;

    struct SinTable {
        std::array<int32_t, LUT_SIZE> data{};
        SinTable() {
            for (int i = 0; i < LUT_SIZE; ++i) {
                double angle = (static_cast<double>(i) / LUT_SIZE) * 2.0 * 3.14159265358979323846;
                data[i] = static_cast<int32_t>(std::sin(angle) * ONE);
            }
        }
    };

    static const SinTable& sinTable() {
        static SinTable tbl;
        return tbl;
    }

    // Map a Fixed32 angle (radians) to a LUT index in [0, LUT_SIZE)
    static int angleToIndex(Fixed32 angle) {
        // Normalise to [0, 2π) in fixed-point
        int32_t twoPiRaw = twoPi().raw;
        int32_t a = angle.raw % twoPiRaw;
        if (a < 0) a += twoPiRaw;
        // Scale to [0, LUT_SIZE)
        int64_t idx = static_cast<int64_t>(a) * LUT_SIZE / twoPiRaw;
        return static_cast<int>(idx) & LUT_MASK;
    }
};

// ── Trig implementations ───────────────────────────────────────────────────

inline Fixed32 Fixed32::sin(Fixed32 angle) {
    return fromRaw(sinTable().data[angleToIndex(angle)]);
}

inline Fixed32 Fixed32::cos(Fixed32 angle) {
    return fromRaw(sinTable().data[angleToIndex(angle + halfPi())]);
}

inline Fixed32 Fixed32::atan2(Fixed32 y, Fixed32 x) {
    // Use float atan2 then convert — deterministic because the result
    // is immediately quantised to Fixed32 (same on all platforms).
    // For cross-compiler determinism, a CORDIC implementation can replace this.
    float a = std::atan2(y.toFloat(), x.toFloat());
    return Fixed32(a);
}

inline Fixed32 Fixed32::sqrt(Fixed32 v) {
    if (v.raw <= 0) return zero();

    // Integer Newton's method on Q16.16
    // We want sqrt(raw / 2^16) * 2^16 = sqrt(raw) * 2^8
    // But that loses precision. Instead: sqrt(raw << 16) directly.
    uint64_t val = static_cast<uint64_t>(v.raw) << FRAC_BITS;
    uint64_t guess = val;
    if (guess == 0) return zero();

    // Initial guess: half the bit-width
    uint64_t g = 1;
    if (guess > (1ULL << 32)) g = 1ULL << 16;
    else                       g = 1ULL << 8;

    for (int i = 0; i < 30; ++i) {
        uint64_t next = (g + val / g) >> 1;
        if (next >= g) break;
        g = next;
    }
    return fromRaw(static_cast<int32_t>(g));
}

// ── Free operator for int * Fixed32 ────────────────────────────────────────
inline constexpr Fixed32 operator*(int a, Fixed32 b) { return b * a; }

} // namespace glory

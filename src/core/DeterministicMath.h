#pragma once
/// Deterministic vector/math types built on Fixed32.
///
/// FVec2 / FVec3 are the gameplay-state equivalents of glm::vec2 / glm::vec3.
/// Rendering code converts to glm types only at the display boundary.

#include "core/FixedPoint.h"
#include <glm/glm.hpp>

namespace glory {

// ── FVec2 ──────────────────────────────────────────────────────────────────

struct FVec2 {
    Fixed32 x, y;

    constexpr FVec2() = default;
    constexpr FVec2(Fixed32 x_, Fixed32 y_) : x(x_), y(y_) {}
    constexpr FVec2(float fx, float fy) : x(fx), y(fy) {}
    explicit FVec2(const glm::vec2& v) : x(v.x), y(v.y) {}

    glm::vec2 toGlm() const { return { x.toFloat(), y.toFloat() }; }

    constexpr FVec2 operator+(FVec2 b) const { return { x + b.x, y + b.y }; }
    constexpr FVec2 operator-(FVec2 b) const { return { x - b.x, y - b.y }; }
    constexpr FVec2 operator*(Fixed32 s) const { return { x * s, y * s }; }
    constexpr FVec2 operator/(Fixed32 s) const { return { x / s, y / s }; }
    constexpr FVec2 operator-() const { return { -x, -y }; }

    constexpr FVec2& operator+=(FVec2 b) { x += b.x; y += b.y; return *this; }
    constexpr FVec2& operator-=(FVec2 b) { x -= b.x; y -= b.y; return *this; }
    constexpr FVec2& operator*=(Fixed32 s) { x *= s; y *= s; return *this; }

    constexpr bool operator==(FVec2 b) const { return x == b.x && y == b.y; }
    constexpr bool operator!=(FVec2 b) const { return !(*this == b); }

    constexpr Fixed32 dot(FVec2 b) const { return x * b.x + y * b.y; }
    constexpr Fixed32 lengthSq() const { return dot(*this); }
    Fixed32 length() const { return Fixed32::sqrt(lengthSq()); }

    FVec2 normalized() const {
        Fixed32 len = length();
        if (len.raw == 0) return { Fixed32::zero(), Fixed32::zero() };
        return *this / len;
    }

    Fixed32 distanceTo(FVec2 b) const { return (*this - b).length(); }
    Fixed32 distanceSqTo(FVec2 b) const { return (*this - b).lengthSq(); }

    static constexpr FVec2 zero() { return { Fixed32::zero(), Fixed32::zero() }; }
};

inline constexpr FVec2 operator*(Fixed32 s, FVec2 v) { return v * s; }

// ── FVec3 ──────────────────────────────────────────────────────────────────

struct FVec3 {
    Fixed32 x, y, z;

    constexpr FVec3() = default;
    constexpr FVec3(Fixed32 x_, Fixed32 y_, Fixed32 z_) : x(x_), y(y_), z(z_) {}
    constexpr FVec3(float fx, float fy, float fz) : x(fx), y(fy), z(fz) {}
    explicit FVec3(const glm::vec3& v) : x(v.x), y(v.y), z(v.z) {}

    glm::vec3 toGlm() const { return { x.toFloat(), y.toFloat(), z.toFloat() }; }

    constexpr FVec3 operator+(FVec3 b) const { return { x + b.x, y + b.y, z + b.z }; }
    constexpr FVec3 operator-(FVec3 b) const { return { x - b.x, y - b.y, z - b.z }; }
    constexpr FVec3 operator*(Fixed32 s) const { return { x * s, y * s, z * s }; }
    constexpr FVec3 operator/(Fixed32 s) const { return { x / s, y / s, z / s }; }
    constexpr FVec3 operator-() const { return { -x, -y, -z }; }

    constexpr FVec3& operator+=(FVec3 b) { x += b.x; y += b.y; z += b.z; return *this; }
    constexpr FVec3& operator-=(FVec3 b) { x -= b.x; y -= b.y; z -= b.z; return *this; }
    constexpr FVec3& operator*=(Fixed32 s) { x *= s; y *= s; z *= s; return *this; }

    constexpr bool operator==(FVec3 b) const { return x == b.x && y == b.y && z == b.z; }
    constexpr bool operator!=(FVec3 b) const { return !(*this == b); }

    constexpr Fixed32 dot(FVec3 b) const { return x * b.x + y * b.y + z * b.z; }

    constexpr FVec3 cross(FVec3 b) const {
        return { y * b.z - z * b.y,
                 z * b.x - x * b.z,
                 x * b.y - y * b.x };
    }

    constexpr Fixed32 lengthSq() const { return dot(*this); }
    Fixed32 length() const { return Fixed32::sqrt(lengthSq()); }

    FVec3 normalized() const {
        Fixed32 len = length();
        if (len.raw == 0) return zero();
        return *this / len;
    }

    Fixed32 distanceTo(FVec3 b) const { return (*this - b).length(); }
    Fixed32 distanceSqTo(FVec3 b) const { return (*this - b).lengthSq(); }

    /// XZ projection to FVec2 (ignoring Y — common for ground-plane ops).
    constexpr FVec2 xz() const { return { x, z }; }

    static constexpr FVec3 zero() { return { Fixed32::zero(), Fixed32::zero(), Fixed32::zero() }; }
    static constexpr FVec3 up()   { return { Fixed32::zero(), Fixed32::one(), Fixed32::zero() }; }
};

inline constexpr FVec3 operator*(Fixed32 s, FVec3 v) { return v * s; }

// ── Utility free functions ─────────────────────────────────────────────────

inline Fixed32 fDistance(FVec3 a, FVec3 b) { return a.distanceTo(b); }
inline Fixed32 fDistanceSq(FVec3 a, FVec3 b) { return a.distanceSqTo(b); }
inline Fixed32 fDot(FVec3 a, FVec3 b) { return a.dot(b); }
inline FVec3   fCross(FVec3 a, FVec3 b) { return a.cross(b); }
inline FVec3   fNormalize(FVec3 v) { return v.normalized(); }
inline Fixed32 fLength(FVec3 v) { return v.length(); }

inline Fixed32 fDistance(FVec2 a, FVec2 b) { return a.distanceTo(b); }
inline Fixed32 fDot(FVec2 a, FVec2 b) { return a.dot(b); }
inline FVec2   fNormalize(FVec2 v) { return v.normalized(); }

/// Linearly interpolate between two FVec3.
inline FVec3 fLerp(FVec3 a, FVec3 b, Fixed32 t) {
    return a + (b - a) * t;
}

/// Fixed32 min / max / clamp
inline constexpr Fixed32 fMin(Fixed32 a, Fixed32 b) { return a < b ? a : b; }
inline constexpr Fixed32 fMax(Fixed32 a, Fixed32 b) { return a > b ? a : b; }
inline constexpr Fixed32 fClamp(Fixed32 v, Fixed32 lo, Fixed32 hi) {
    return fMin(fMax(v, lo), hi);
}

/// Convert glm ↔ fixed
inline FVec3 toFixed(const glm::vec3& v) { return FVec3(v); }
inline glm::vec3 toGlm(FVec3 v) { return v.toGlm(); }
inline FVec2 toFixed(const glm::vec2& v) { return FVec2(v); }
inline glm::vec2 toGlm(FVec2 v) { return v.toGlm(); }

} // namespace glory

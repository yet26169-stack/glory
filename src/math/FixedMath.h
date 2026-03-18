#pragma once

#include "Fixed64.h"

namespace glory {

// Inverse square root: 1 / sqrt(x), deterministic
inline Fixed64 inverseSqrt(Fixed64 f) {
    if (f.raw <= 0) return Fixed64::zero();
    Fixed64 s = sqrt(f);
    if (s.raw == 0) return Fixed64::zero();
    return Fixed64::one() / s;
}

// Reflect incident vector off a surface with the given normal
// reflect(I, N) = I - 2 * dot(I, N) * N
inline FixedVec3 reflect(const FixedVec3& incident, const FixedVec3& normal) {
    Fixed64 two = Fixed64::fromInt(2);
    Fixed64 d = incident.dot(normal);
    return incident - normal * (two * d);
}

// Hermite smoothstep interpolation: 3t² - 2t³, clamped to [0, 1]
inline Fixed64 smoothStep(Fixed64 edge0, Fixed64 edge1, Fixed64 x) {
    Fixed64 t = clamp((x - edge0) / (edge1 - edge0), Fixed64::zero(), Fixed64::one());
    Fixed64 three = Fixed64::fromInt(3);
    Fixed64 two = Fixed64::fromInt(2);
    return t * t * (three - two * t);
}

// Sign: returns -1, 0, or +1
inline constexpr Fixed64 sign(Fixed64 f) {
    if (f.raw > 0) return Fixed64::one();
    if (f.raw < 0) return Fixed64::fromRaw(-Fixed64::ONE);
    return Fixed64::zero();
}

// Move toward a target value by at most maxDelta per step
inline Fixed64 moveToward(Fixed64 current, Fixed64 target, Fixed64 maxDelta) {
    Fixed64 diff = target - current;
    if (abs(diff) <= maxDelta) return target;
    return current + sign(diff) * maxDelta;
}

// Angle between two FixedVec3 (returns Fixed64 in radians)
inline Fixed64 angleBetween(const FixedVec3& a, const FixedVec3& b) {
    Fixed64 lenProduct = a.length() * b.length();
    if (lenProduct == Fixed64::zero()) return Fixed64::zero();
    Fixed64 cosValue = clamp(a.dot(b) / lenProduct,
                             Fixed64::fromInt(-1), Fixed64::one());
    return acos(cosValue);
}

// Project vector a onto vector b
inline FixedVec3 project(const FixedVec3& a, const FixedVec3& b) {
    Fixed64 bLenSq = b.lengthSquared();
    if (bLenSq == Fixed64::zero()) return FixedVec3::zero();
    return b * (a.dot(b) / bLenSq);
}

// Reject: component of a perpendicular to b
inline FixedVec3 reject(const FixedVec3& a, const FixedVec3& b) {
    return a - project(a, b);
}

} // namespace glory

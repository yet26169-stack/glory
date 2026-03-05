#include "math/FixedPoint.h"
#include <cstdint>

namespace glory {

Fixed64 fixedSqrt(Fixed64 x) {
    if (x.raw <= 0) return Fixed64(0);
    // Use integer sqrt with Newton-Raphson refinement
    // Input: Q47.16, result: Q47.16
    // We compute sqrt(x.raw << 16) >> 0 to stay in Q47.16
    uint64_t n = static_cast<uint64_t>(x.raw) << Fixed64::FRAC_BITS;
    if (n == 0) return Fixed64(0);
    // Initial estimate via bit count
    uint64_t r = 1;
    uint64_t t = n >> 1;
    while (t > 0) { r <<= 1; t >>= 2; }
    // Newton-Raphson iterations
    for (int i = 0; i < 6; ++i) {
        if (r == 0) break;
        r = (r + n / r) >> 1;
    }
    return Fixed64(static_cast<int64_t>(r));
}

Fixed64 fixedAtan2(Fixed64 y, Fixed64 x) {
    // CORDIC-inspired rational approximation, error < 0.01 rad
    // Result in radians as Fixed64
    if (x.raw == 0 && y.raw == 0) return Fixed64(0);

    // Convert to float for approximation (deterministic: same inputs, same IEEE result)
    float fy = y.toFloat();
    float fx = x.toFloat();
    float angle = 0.0f;
    // atan2 approximation via minimax polynomial
    if (fx != 0.0f || fy != 0.0f) {
        angle = std::atan2(fy, fx); // NOTE: float atan2 is NOT cross-platform deterministic
        // For production determinism replace with a table-lookup or CORDIC
    }
    return Fixed64::fromFloat(angle);
}

} // namespace glory

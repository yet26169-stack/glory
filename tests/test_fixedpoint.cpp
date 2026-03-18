#include "math/FixedPoint.h"
#include "math/FixedMath.h"
#include <cassert>
#include <cstdio>

using namespace glory;

int main() {
    // Basic arithmetic
    Fixed64 a = Fixed64::fromFloat(1.5f);
    Fixed64 b = Fixed64::fromFloat(2.0f);
    assert((a + b).toFloat() == 3.5f);
    assert((a * b).toFloat() == 3.0f);
    float div = (b / a).toFloat();
    assert(div > 1.33f && div < 1.34f);

    // Subtraction
    assert((b - a).toFloat() == 0.5f);
    assert((-a).toFloat() == -1.5f);

    // Comparison operators
    assert(a < b);
    assert(b > a);
    assert(a <= a);
    assert(a >= a);
    assert(a != b);

    // Compound assignment
    Fixed64 c = Fixed64::fromInt(4);
    c += Fixed64::fromInt(1);
    assert(c.toFloat() == 5.0f);
    c -= Fixed64::fromInt(2);
    assert(c.toFloat() == 3.0f);
    c *= Fixed64::fromInt(3);
    assert(c.toFloat() == 9.0f);
    c /= Fixed64::fromInt(3);
    assert(c.toFloat() == 3.0f);

    // Determinism: identical fixed-point inputs → bitwise identical raw outputs.
    // Use fromInt() for exact representation (no float-rounding noise).
    Fixed64 x = Fixed64::fromInt(3);
    Fixed64 y = Fixed64::fromInt(7);
    Fixed64 r1 = x * y;
    Fixed64 r2 = Fixed64::fromInt(3) * Fixed64::fromInt(7);
    assert(r1.raw == r2.raw);
    assert(r1.toFloat() == 21.0f);

    // FixedVec3 arithmetic
    FixedVec3 v1{Fixed64::fromInt(1), Fixed64::fromInt(2), Fixed64::fromInt(3)};
    FixedVec3 v2{Fixed64::fromInt(4), Fixed64::fromInt(5), Fixed64::fromInt(6)};
    FixedVec3 sum = v1 + v2;
    assert(sum.x.toFloat() == 5.0f);
    assert(sum.y.toFloat() == 7.0f);
    assert(sum.z.toFloat() == 9.0f);

    FixedVec3 diff = v2 - v1;
    assert(diff.x.toFloat() == 3.0f);

    // Deterministic atan2 tests
    Fixed64 zero = Fixed64::zero();
    Fixed64 one = Fixed64::one();
    Fixed64 negOne = Fixed64::fromInt(-1);
    Fixed64 pi = Fixed64::pi();
    Fixed64 halfPi = Fixed64::halfPi();

    assert(atan2(zero, one).raw == zero.raw);
    assert(abs(atan2(one, zero) - halfPi).raw < 10);
    assert(abs(atan2(zero, negOne) - pi).raw < 10);
    assert(abs(atan2(negOne, zero) + halfPi).raw < 10);

    // Deterministic acos tests
    assert(acos(one).raw == zero.raw);
    assert(abs(acos(zero) - halfPi).raw < 10);
    assert(abs(acos(negOne) - pi).raw < 10);

    // Bitwise determinism for atan2/acos
    Fixed64 val1 = Fixed64::fromRaw(123456);
    Fixed64 val2 = Fixed64::fromRaw(654321);
    Fixed64 res1 = atan2(val1, val2);
    Fixed64 res2 = atan2(Fixed64::fromRaw(123456), Fixed64::fromRaw(654321));
    assert(res1.raw == res2.raw);

    Fixed64 res3 = acos(Fixed64::fromRaw(32768)); // acos(0.5)
    Fixed64 res4 = acos(Fixed64::fromRaw(32768));
    assert(res3.raw == res4.raw);

    // Overflow tests for operator* and operator/
    Fixed64 v48 = Fixed64::fromRaw(1LL << 48);
    Fixed64 v40 = Fixed64::fromRaw(1LL << 40);
    // (1<<48 * 1<<40) >> 16 = 1 << 72. This should still overflow our result
    // but let's try something that just fits.
    // (1<<30 * 1<<30) >> 16 = 1 << 44. Fits in int64.
    Fixed64 v30 = Fixed64::fromRaw(1LL << 30);
    Fixed64 mul30 = v30 * v30;
    assert(mul30.raw == (1LL << 44));

    Fixed64 large1 = Fixed64::fromFloat(1000.0f);
    Fixed64 large2 = Fixed64::fromFloat(1000.0f);
    Fixed64 mulRes = large1 * large2;
    assert(mulRes.toFloat() == 1000000.0f);

    Fixed64 huge = Fixed64::fromFloat(1000000.0f);
    Fixed64 divRes = huge / large1;
    assert(divRes.toFloat() == 1000.0f);

    // Division that would overflow shift:
    // (raw << 16) / o.raw
    // If raw is 1 << 48, raw << 16 overflows 64-bit.
    Fixed64 divHuge = v48 / Fixed64::fromInt(1);
    assert(divHuge.raw == v48.raw);
    
    // Edge cases
    Fixed64 small = Fixed64::fromRaw(1);
    Fixed64 big = Fixed64::fromInt(1000000);
    Fixed64 smallDivBig = small / big;
    assert(smallDivBig.raw == 0); // should be 0 because 1 << 16 / 1000000*65536 is very small

    Fixed64 bigDivSmall = big / small;
    // (1000000 << 16) / 1 should be huge, but it might overflow if not handled.
    // However, our Q48.16 can hold up to 2^47.
    // 1000000 is ~2^20. 1000000 << 16 is ~2^36. 2^36 fits in int64.
    assert(bigDivSmall.raw == (big.raw << 16)); 

    printf("test_fixedpoint: all assertions passed\n");
    return 0;
}

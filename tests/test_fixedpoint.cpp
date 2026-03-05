#include "math/FixedPoint.h"
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

    printf("test_fixedpoint: all assertions passed\n");
    return 0;
}

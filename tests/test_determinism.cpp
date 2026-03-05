/// Determinism test — only compiled with -DGLORY_DETERMINISTIC=ON (Phase 6.3).
/// Runs 300 simulation ticks on two isolated EnTT registries with identical
/// starting state and asserts that StateChecksum matches every tick.
#ifdef GLORY_DETERMINISTIC

#include "core/StateChecksumSystem.h"
#include "scene/Components.h"
#include "math/FixedPoint.h"
#include <entt.hpp>
#include <cassert>
#include <cstdio>

using namespace glory;

static void populateRegistry(entt::registry& reg, int entityCount) {
    for (int i = 0; i < entityCount; ++i) {
        auto e = reg.create();
        reg.emplace<SimPosition>(e, SimPosition{FixedVec3{
            Fixed64::fromInt(i * 3),
            Fixed64::fromInt(0),
            Fixed64::fromInt(i * 7)}});
        reg.emplace<SimVelocity>(e, SimVelocity{FixedVec3{
            Fixed64::fromFloat(0.5f),
            Fixed64(0),
            Fixed64::fromFloat(0.3f)}});
    }
}

static void stepRegistry(entt::registry& reg) {
    auto view = reg.view<SimPosition, SimVelocity>();
    for (auto e : view) {
        auto& pos = view.get<SimPosition>(e);
        auto& vel = view.get<SimVelocity>(e);
        pos.value = pos.value + vel.value * Fixed64::fromFloat(1.0f / 30.0f);
    }
}

int main() {
    const int ENTITY_COUNT = 64;
    const int TICK_COUNT   = 300;

    entt::registry reg1, reg2;
    populateRegistry(reg1, ENTITY_COUNT);
    populateRegistry(reg2, ENTITY_COUNT);

    StateChecksumSystem scs;

    for (int tick = 0; tick < TICK_COUNT; ++tick) {
        stepRegistry(reg1);
        stepRegistry(reg2);

        auto h1 = scs.compute(reg1, static_cast<uint32_t>(tick));
        auto h2 = scs.compute(reg2, static_cast<uint32_t>(tick));

        if (h1.hash != h2.hash) {
            printf("FAIL: checksum mismatch at tick %d  (h1=%llu, h2=%llu)\n",
                   tick, (unsigned long long)h1.hash, (unsigned long long)h2.hash);
            return 1;
        }
    }

    printf("test_determinism: %d ticks × %d entities — all checksums matched\n",
           TICK_COUNT, ENTITY_COUNT);
    return 0;
}

#else

#include <cstdio>
int main() {
    printf("test_determinism: skipped (GLORY_DETERMINISTIC not set)\n");
    return 0;
}

#endif // GLORY_DETERMINISTIC

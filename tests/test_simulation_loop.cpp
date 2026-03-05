/// Tests that two registries ticked with identical inputs produce the same
/// StateChecksum (determinism invariant for rollback networking).
#include "core/StateChecksumSystem.h"
#include "scene/Components.h"
#include <entt.hpp>
#include <cassert>
#include <cstdio>

using namespace glory;

static void populateRegistry(entt::registry& reg) {
    for (int i = 0; i < 8; ++i) {
        auto e = reg.create();
#ifdef GLORY_DETERMINISTIC
        SimPosition p{FixedVec3{Fixed64::fromInt(i * 10), Fixed64::fromInt(0), Fixed64::fromInt(i * 5)}};
        SimVelocity v{FixedVec3{Fixed64::fromFloat(1.5f), Fixed64(0), Fixed64::fromFloat(0.5f)}};
#else
        SimPosition p{glm::vec3(i * 10.0f, 0.0f, i * 5.0f)};
        SimVelocity v{glm::vec3(1.5f, 0.0f, 0.5f)};
#endif
        reg.emplace<SimPosition>(e, p);
        reg.emplace<SimVelocity>(e, v);
    }
}

int main() {
    entt::registry reg1, reg2;
    populateRegistry(reg1);
    populateRegistry(reg2);

    StateChecksumSystem scs;
    auto h1 = scs.compute(reg1, 0);
    auto h2 = scs.compute(reg2, 0);

    assert(h1.hash == h2.hash && "Identical registries must produce the same checksum");
    assert(h1.tickN == 0);

    // Modify one registry and verify checksums diverge
    auto view = reg1.view<SimPosition>();
    for (auto e : view) {
#ifdef GLORY_DETERMINISTIC
        reg1.get<SimPosition>(e).value.x = Fixed64::fromInt(999);
#else
        reg1.get<SimPosition>(e).value.x = 999.0f;
#endif
        break; // modify just the first entity
    }
    auto h3 = scs.compute(reg1, 0);
    assert(h3.hash != h2.hash && "Modified registry must produce a different checksum");

    printf("test_simulation_loop: all assertions passed\n");
    return 0;
}

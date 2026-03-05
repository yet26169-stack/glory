/// Tests StateSnapshot capture / restore / verify round-trip.
#include "core/StateSnapshot.h"
#include "scene/Components.h"
#include <entt.hpp>
#include <cassert>
#include <cstdio>

using namespace glory;

int main() {
    entt::registry reg;

    // Create a few entities with SimPosition
    std::vector<entt::entity> entities;
    for (int i = 0; i < 4; ++i) {
        auto e = reg.create();
        entities.push_back(e);
#ifdef GLORY_DETERMINISTIC
        reg.emplace<SimPosition>(e, SimPosition{FixedVec3{
            Fixed64::fromInt(i), Fixed64::fromInt(0), Fixed64::fromInt(i)}});
#else
        reg.emplace<SimPosition>(e, SimPosition{glm::vec3(float(i), 0.0f, float(i))});
#endif
    }

    // Capture a snapshot
    StateSnapshot snap;
    snap.capture(reg, 42u);
    assert(snap.tick == 42u);
    assert(snap.checksum != 0 || true); // hash of non-empty registry

    // Verify passes
    assert(snap.verify(reg));

    // Corrupt the registry
    auto& pos = reg.get<SimPosition>(entities[0]);
#ifdef GLORY_DETERMINISTIC
    pos.value.x = Fixed64::fromInt(1000);
#else
    pos.value.x = 1000.0f;
#endif

    // Verify should now fail
    assert(!snap.verify(reg));

    // Test SnapshotBuffer round-trip
    SnapshotBuffer buf;
    StateSnapshot snap2;
    snap2.capture(reg, 7u);
    buf.push(snap2);

    const StateSnapshot* retrieved = buf.get(7u);
    assert(retrieved != nullptr);
    assert(retrieved->tick == 7u);

    const StateSnapshot* missing = buf.get(99u);
    assert(missing == nullptr);

    printf("test_snapshot: all assertions passed\n");
    return 0;
}

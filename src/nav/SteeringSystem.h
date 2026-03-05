#pragma once
#include <entt.hpp>
#include <vector>
#include "nav/FlowField.h"
#include "scene/Components.h"

namespace glory {

/// Applies flow-field directions + separation to entities with FlowFieldAgent.
/// NOT applied to lane minions (those use LaneFollower).
/// Applied to: heroes (group commands), jungle monsters, RTS group moves.
class SteeringSystem {
public:
    void update(entt::registry& reg,
                const std::vector<FlowField*>& activeFields,
                SimFloat dt);

private:
    /// Compute weighted separation force for one entity vs its neighbours.
    static SimVec3 separation(entt::registry& reg, entt::entity self,
                              SimFloat radius);
};

} // namespace glory

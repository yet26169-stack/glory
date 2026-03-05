#pragma once

#include <entt.hpp>
#include <glm/glm.hpp>

namespace glory {

class TargetingSystem {
public:
  /// Cast a ray and return the closest targetable entity hit, or entt::null.
  entt::entity pickTarget(entt::registry &registry,
                          const glm::vec3 &rayOrigin,
                          const glm::vec3 &rayDir,
                          float maxDist = 200.0f);
};

} // namespace glory

#include "input/TargetingSystem.h"
#include "minion/MinionComponents.h"
#include "scene/Components.h"

#include <cmath>

namespace glory {

entt::entity TargetingSystem::pickTarget(entt::registry &registry,
                                         const glm::vec3 &rayOrigin,
                                         const glm::vec3 &rayDir,
                                         float maxDist) {
  entt::entity closest = entt::null;
  float closestT = maxDist;

  auto view = registry.view<TargetableComponent, TransformComponent,
                             MinionHealthComponent>();
  for (auto entity : view) {
    auto &hp = view.get<MinionHealthComponent>(entity);
    if (hp.isDead)
      continue;

    auto &transform = view.get<TransformComponent>(entity);
    auto &targetable = view.get<TargetableComponent>(entity);

    // Ray-sphere intersection
    glm::vec3 oc = rayOrigin - transform.position;
    float a = glm::dot(rayDir, rayDir);
    float b = 2.0f * glm::dot(oc, rayDir);
    float c = glm::dot(oc, oc) - targetable.hitRadius * targetable.hitRadius;
    float discriminant = b * b - 4.0f * a * c;

    if (discriminant < 0.0f)
      continue;

    float t = (-b - std::sqrt(discriminant)) / (2.0f * a);
    if (t < 0.0f)
      t = (-b + std::sqrt(discriminant)) / (2.0f * a);
    if (t < 0.0f || t > closestT)
      continue;

    closestT = t;
    closest = entity;
  }

  return closest;
}

} // namespace glory

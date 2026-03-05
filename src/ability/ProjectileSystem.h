#pragma once

#include "scene/Scene.h"

namespace glory {

// ── ProjectileSystem ─────────────────────────────────────────────────────────
// Moves and destroys projectile entities each frame.
// Integrates with Scene (for entity creation/destruction) and the mesh/material
// shared state already set up during buildScene.
class ProjectileSystem {
public:
  // Call once after buildScene – caches the shared sphere mesh / material
  // indices
  void init(Scene &scene, uint32_t sphereMeshIndex, uint32_t defaultTexIndex,
            uint32_t flatNormIndex);

  entt::entity spawnSkillshot(Scene &scene, entt::entity caster,
                              const std::string &abilityId, int level,
                              const glm::vec3 &origin,
                              const glm::vec3 &direction, float speed,
                              float maxRange, const glm::vec4 &colour,
                              float collisionRadius = 0.5f);

  // Spawn a point-targeted AoE indicator that moves to the ground target and
  // pops.
  entt::entity spawnGroundAoE(Scene &scene, entt::entity caster,
                              const std::string &abilityId, int level,
                              const glm::vec3 &origin,
                              const glm::vec3 &targetPos, float speed,
                              float aoeRadius, const glm::vec4 &colour);

  entt::entity spawnTargeted(Scene &scene, entt::entity caster,
                             entt::entity target,
                             const std::string &abilityId, int level,
                             float speed, const glm::vec4 &colour);

  // Called every frame from Scene::update
  void update(Scene &scene, float dt);

private:
  uint32_t m_sphereMesh = 0;
  uint32_t m_defaultTex = 0;
  uint32_t m_flatNorm = 0;
  bool m_initialized = false;
};

} // namespace glory

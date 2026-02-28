#pragma once

#include "renderer/Frustum.h"

#include <entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>

namespace glory {

struct TagComponent {
  std::string name;
};

struct TransformComponent {
  glm::vec3 position{0.0f};
  glm::vec3 rotation{0.0f}; // Euler angles in radians
  glm::vec3 scale{1.0f};

  glm::mat4 getModelMatrix() const {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), position);
    m = glm::rotate(m, rotation.x, glm::vec3(1, 0, 0));
    m = glm::rotate(m, rotation.y, glm::vec3(0, 1, 0));
    m = glm::rotate(m, rotation.z, glm::vec3(0, 0, 1));
    m = glm::scale(m, scale);
    return m;
  }
};

struct MeshComponent {
  uint32_t meshIndex = 0;
  AABB localAABB{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
};

struct MaterialComponent {
  uint32_t materialIndex = 0;  // diffuse texture index
  uint32_t normalMapIndex = 0; // normal map texture index (0 = flat)
  float shininess = 0.0f;      // 0 = use global default
  float metallic = 0.0f;       // 0 = dielectric, 1 = metal
  float roughness = 0.5f;      // 0.04-1.0
  float emissive = 0.0f;       // 0 = no glow, >0 = self-illumination strength
};

struct LightComponent {
  glm::vec3 color{1.0f};
  float intensity = 1.0f;
  enum class Type { Point, Directional } type = Type::Point;
};

struct RotateComponent {
  glm::vec3 axis{0.0f, 1.0f, 0.0f};
  float speed = 1.0f; // radians per second
};

struct ColorComponent {
  glm::vec4 tint{1.0f}; // RGBA multiplier on vertex color
};

struct OrbitComponent {
  glm::vec3 center{0.0f};
  float radius = 2.0f;
  float speed = 1.0f;  // radians per second
  float phase = 0.0f;  // current angle
  float height = 0.0f; // Y offset from center
};

struct CharacterComponent {
  glm::vec3 targetPosition{0.0f};
  float moveSpeed = 6.0f;
  bool hasTarget = false;
};

struct LODComponent {
  static constexpr uint32_t MAX_LOD_LEVELS = 4;
  struct Level {
    uint32_t meshIndex = 0;
    float maxDistance = 0.0f; // use this level up to this distance
  };
  Level levels[MAX_LOD_LEVELS];
  uint32_t levelCount = 0;
};

// ── Projectile spawned by ability system ─────────────────────────────────────
struct ProjectileComponent {
  glm::vec3 velocity{0.0f}; // world-space units/s
  float speed = 800.0f;     // units per second (magnitude of velocity)
  float maxRange = 1100.0f; // auto-destroy after this distance
  float travelledDistance = 0.0f;
  float radius = 0.5f; // collision or AoE radius
  entt::entity caster = entt::null;
  bool hitOnArrival = false; // true = ground-targeted AoE
  std::string abilityId;     // to look up onHitEffects
  int abilityLevel = 1;      // to evaluate scaling
};

} // namespace glory

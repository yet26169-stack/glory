#pragma once

#include "animation/AnimationClip.h"
#include "animation/AnimationPlayer.h"
#include "animation/Skeleton.h"
#include "renderer/Buffer.h" // Vertex
#include "renderer/Frustum.h"

#include <entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>

#include "math/FixedPoint.h" // SimFloat / SimVec3 typedefs (defines glory namespace internally)

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
    // Y-X-Z rotation order: yaw (Y) is applied first in world space,
    // then pitch (X), then roll (Z).  This lets the character face a
    // direction via rotation.y (yaw) independently of the coordinate-
    // system correction in rotation.x (pitch).
    m = glm::rotate(m, rotation.y, glm::vec3(0, 1, 0));
    m = glm::rotate(m, rotation.x, glm::vec3(1, 0, 0));
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
  float heightOffset = 0.0f; // vertical offset so model feet sit at terrain height
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

struct TargetedProjectileTag {
    entt::entity target = entt::null;
};

struct DashComponent {
    glm::vec3 startPos{0.0f};
    glm::vec3 endPos{0.0f};
    float duration     = 0.3f;
    float elapsed      = 0.0f;
    bool  isUntargetable = false; // some dashes grant untargetability
};

// ── Map entity tag (for GLB-based MOBA map) ──────────────────────────────────
struct MapComponent {};

// ── Skeletal animation components ────────────────────────────────────────────

struct SkeletonComponent {
  Skeleton skeleton;
  std::vector<std::vector<SkinVertex>> skinVertices; // per-mesh
  std::vector<std::vector<Vertex>> bindPoseVertices; // per-mesh (CPU copy)
};

struct AnimationComponent {
  AnimationPlayer player;
  std::vector<AnimationClip> clips;    // owned clips (idle, walk, etc.)
  int activeClipIndex = -1;            // index into clips, -1 = none
  std::vector<Vertex> skinnedVertices; // scratch buffer for skinning output
};

struct DynamicMeshComponent {
  uint32_t dynamicMeshIndex = 0; // index into Scene::m_dynamicMeshes
};

// GPU-skinned character: static vertex buffer on GPU, bone matrices in SSBO
struct GPUSkinnedMeshComponent {
  uint32_t staticSkinnedMeshIndex = 0; // index into Scene::m_staticSkinnedMeshes
};

// LOD variant of GPUSkinnedMeshComponent — holds multiple quality levels
// Level 0 = highest detail (close), Level N = lowest (far away)
struct SkinnedLODComponent {
  static constexpr uint32_t MAX_LOD_LEVELS = 4;
  struct Level {
    uint32_t staticSkinnedMeshIndex = 0;
    float    maxDistance            = 0.0f; // use this level up to this camera distance
  };
  Level    levels[MAX_LOD_LEVELS]{};
  uint32_t levelCount = 0;
};

// ── Targeting & auto-attack components ──────────────────────────────────────

struct TargetableComponent {
  float hitRadius = 0.5f; // click detection sphere radius
};

struct PlayerTargetComponent {
  entt::entity targetEntity = entt::null;
};

struct AutoAttackComponent {
  float attackRange = 5.0f;
  float attackCooldown = 0.8f;        // seconds between attacks
  float timeSinceLastAttack = 0.0f;
  float attackDamage = 65.0f;
  bool isAttacking = false;
};

// ── Simulation-authoritative components (Phase 0.1b) ─────────────────────────
// These hold the canonical simulation state. TransformComponent stays as the
// render-interpolated view and is written by SimToRenderSyncSystem each frame.

struct SimPosition { SimVec3 value{}; };
struct SimVelocity { SimVec3 value{}; };
struct SimRotation { SimFloat yaw{}; }; // Y-axis rotation (MOBA top-down)

struct VisionComponent {
    SimFloat sightRadius{};
    uint8_t  teamID = 0;
};

struct FlowFieldAgent {
    uint32_t flowFieldID       = 0;
    SimFloat separationRadius{};
};

// ── Marquee selection tag ────────────────────────────────────────────────────
struct SelectedComponent {};

} // namespace glory

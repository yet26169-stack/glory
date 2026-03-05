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
  int32_t  subMeshIndex = -1; // -1 = all sub-meshes, >=0 = specific sub-mesh index
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
  float speed = 1.0f;
};

struct ColorComponent {
  glm::vec4 tint{1.0f};
};

struct OrbitComponent {
  glm::vec3 center{0.0f};
  float radius = 2.0f;
  float speed = 1.0f;
  float phase = 0.0f;
  float height = 0.0f;
};

struct CharacterComponent {
  glm::vec3 targetPosition{0.0f};
  float moveSpeed = 6.0f;
  float heightOffset = 0.0f;
  bool hasTarget = false;
};

// ── Map entity tag (for GLB-based MOBA map) ──────────────────────────────────
struct MapComponent {};

// ── Skeletal animation components ────────────────────────────────────────────

struct SkeletonComponent {
  Skeleton skeleton;
  std::vector<std::vector<SkinVertex>> skinVertices;
  std::vector<std::vector<Vertex>> bindPoseVertices;
};

struct AnimationComponent {
  AnimationPlayer player;
  std::vector<AnimationClip> clips;
  int activeClipIndex = -1;
  std::vector<Vertex> skinnedVertices;
};

struct DynamicMeshComponent {
  uint32_t dynamicMeshIndex = 0;
};

struct GPUSkinnedMeshComponent {
  uint32_t staticSkinnedMeshIndex = 0;
};

} // namespace glory

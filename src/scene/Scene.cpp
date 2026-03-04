#include "scene/Scene.h"
#include "animation/CPUSkinning.h"
#include "terrain/TerrainSystem.h"
#include "minion/MinionComponents.h"
#include "minion/MinionTypes.h"

#include "ability/AbilitySystem.h"
#include "ability/CooldownSystem.h"
#include "ability/EffectSystem.h"
#include "ability/ProjectileSystem.h"
#include "ability/StatusEffectSystem.h"

#include <cmath>
#include <spdlog/spdlog.h>

namespace glory {

entt::entity Scene::createEntity(const std::string &name) {
  auto entity = m_registry.create();
  m_registry.emplace<TagComponent>(entity, TagComponent{name});
  m_registry.emplace<TransformComponent>(entity);
  return entity;
}

void Scene::destroyEntity(entt::entity entity) { m_registry.destroy(entity); }

uint32_t Scene::addMesh(Model model) {
  uint32_t idx = static_cast<uint32_t>(m_meshes.size());
  m_meshes.push_back(std::move(model));
  return idx;
}

uint32_t Scene::addTexture(Texture texture) {
  uint32_t idx = static_cast<uint32_t>(m_textures.size());
  m_textures.push_back(std::move(texture));
  return idx;
}

uint32_t Scene::addMaterial(Material material) {
  uint32_t idx = static_cast<uint32_t>(m_materials.size());
  m_materials.push_back(std::move(material));
  return idx;
}

uint32_t Scene::addDynamicMesh(DynamicMesh mesh) {
  uint32_t idx = static_cast<uint32_t>(m_dynamicMeshes.size());
  m_dynamicMeshes.push_back(std::move(mesh));
  return idx;
}

uint32_t Scene::addStaticSkinnedMesh(StaticSkinnedMesh mesh) {
  uint32_t idx = static_cast<uint32_t>(m_staticSkinnedMeshes.size());
  m_staticSkinnedMeshes.push_back(std::move(mesh));
  return idx;
}

void Scene::update(float deltaTime, uint32_t currentFrame) {
  // Update ability systems
  glory::StatusEffectSystem().update(m_registry, deltaTime);
  glory::CooldownSystem().update(m_registry, deltaTime);
  glory::AbilitySystem().update(m_registry, deltaTime);
  glory::EffectSystem().apply(m_registry);

  // Rotation animation
  auto rotView = m_registry.view<TransformComponent, RotateComponent>();
  for (auto entity : rotView) {
    auto &transform = rotView.get<TransformComponent>(entity);
    auto &rotate = rotView.get<RotateComponent>(entity);
    transform.rotation += rotate.axis * rotate.speed * deltaTime;
  }

  // Orbit animation
  auto orbitView = m_registry.view<TransformComponent, OrbitComponent>();
  for (auto entity : orbitView) {
    auto &transform = orbitView.get<TransformComponent>(entity);
    auto &orbit = orbitView.get<OrbitComponent>(entity);
    orbit.phase += orbit.speed * deltaTime;
    transform.position.x =
        orbit.center.x + orbit.radius * std::cos(orbit.phase);
    transform.position.z =
        orbit.center.z + orbit.radius * std::sin(orbit.phase);
    transform.position.y = orbit.center.y + orbit.height;
  }

  // Character click-to-move
  auto charView = m_registry.view<TransformComponent, CharacterComponent>();
  for (auto entity : charView) {
    auto &transform = charView.get<TransformComponent>(entity);
    auto &character = charView.get<CharacterComponent>(entity);

    // Always snap to terrain height (even when idle)
    if (m_terrain) {
      transform.position.y =
          m_terrain->GetHeightAt(transform.position.x, transform.position.z) +
          character.heightOffset;
    }

    if (!character.hasTarget)
      continue;

    glm::vec3 toTarget = character.targetPosition - transform.position;
    toTarget.y = 0.0f; // move on XZ plane only
    float dist = glm::length(toTarget);

    if (dist < 0.1f) {
      character.hasTarget = false;
      continue;
    }

    glm::vec3 direction = toTarget / dist;
    float step = character.moveSpeed * deltaTime;
    if (step > dist)
      step = dist;

    transform.position.x += direction.x * step;
    transform.position.z += direction.z * step;

    // Rotate to face movement direction
    float targetYaw = std::atan2(direction.x, direction.z);
    float currentYaw = transform.rotation.y;
    // Shortest-angle lerp
    float diff = targetYaw - currentYaw;
    while (diff > 3.14159265f)
      diff -= 6.28318530f;
    while (diff < -3.14159265f)
      diff += 6.28318530f;
    float lerpSpeed = 10.0f * deltaTime;
    if (lerpSpeed > 1.0f)
      lerpSpeed = 1.0f;
    transform.rotation.y = currentYaw + diff * lerpSpeed;
  }

  // ── Animation update: state transitions + skinning ──────────────────────
  auto animView =
      m_registry
          .view<SkeletonComponent, AnimationComponent, DynamicMeshComponent>();
  for (auto entity : animView) {
    auto &skelComp = animView.get<SkeletonComponent>(entity);
    auto &animComp = animView.get<AnimationComponent>(entity);
    auto &dynComp = animView.get<DynamicMeshComponent>(entity);

    // State transition for minions: 0=walk, 1=attack
    auto *minionState = m_registry.try_get<MinionStateComponent>(entity);
    if (minionState && !animComp.clips.empty()) {
      int desiredClip = 0;
      if (animComp.clips.size() >= 2 &&
          minionState->state == MinionState::Attacking) {
        desiredClip = 1;
      }
      if (desiredClip != animComp.activeClipIndex) {
        animComp.activeClipIndex = desiredClip;
        animComp.player.setClip(&animComp.clips[desiredClip]);
      }
    }

    // State transition: pick clip — 0=idle, 1=walk, 2=auto-attack
    auto *charComp = m_registry.try_get<CharacterComponent>(entity);
    if (charComp && !animComp.clips.empty()) {
      int desiredClip = 0; // default idle
      auto *atkComp = m_registry.try_get<AutoAttackComponent>(entity);

      if (animComp.clips.size() >= 3 && atkComp && atkComp->isAttacking) {
        desiredClip = 2; // auto-attack
      } else if (animComp.clips.size() >= 2 && charComp->hasTarget) {
        desiredClip = 1; // walk
      }

      if (desiredClip != animComp.activeClipIndex) {
        animComp.activeClipIndex = desiredClip;
        animComp.player.setClip(&animComp.clips[desiredClip]);
      }
    }

    animComp.player.update(deltaTime);

    // CPU skinning for each sub-mesh (typically just 1)
    if (dynComp.dynamicMeshIndex < m_dynamicMeshes.size()) {
      auto &dynMesh = m_dynamicMeshes[dynComp.dynamicMeshIndex];
      const auto &skinningMats = animComp.player.getSkinningMatrices();

      // Skin mesh 0 (most characters have a single mesh)
      if (!skelComp.bindPoseVertices.empty() &&
          !skelComp.skinVertices.empty()) {
        applyCPUSkinning(skelComp.bindPoseVertices[0], skelComp.skinVertices[0],
                         skinningMats, animComp.skinnedVertices);
        dynMesh.updateVertices(currentFrame, animComp.skinnedVertices);
      }
    }
  }

  // ── GPU-skinned animation update: advance animation only, no CPU skinning ──
  // Bone matrices are uploaded to the SSBO by the Renderer each frame.
  auto gpuAnimView =
      m_registry
          .view<SkeletonComponent, AnimationComponent, GPUSkinnedMeshComponent>();
  for (auto entity : gpuAnimView) {
    auto &animComp = gpuAnimView.get<AnimationComponent>(entity);

    // State transition for minions: 0=walk, 1=attack
    auto *minionState = m_registry.try_get<MinionStateComponent>(entity);
    if (minionState && !animComp.clips.empty()) {
      int desiredClip = 0;
      if (animComp.clips.size() >= 2 &&
          minionState->state == MinionState::Attacking) {
        desiredClip = 1;
      }
      if (desiredClip != animComp.activeClipIndex) {
        animComp.activeClipIndex = desiredClip;
        animComp.player.setClip(&animComp.clips[desiredClip]);
      }
    }

    // State transition: pick clip — 0=idle, 1=walk, 2=auto-attack
    auto *charComp = m_registry.try_get<CharacterComponent>(entity);
    if (charComp && !animComp.clips.empty()) {
      int desiredClip = 0;
      auto *atkComp = m_registry.try_get<AutoAttackComponent>(entity);

      if (animComp.clips.size() >= 3 && atkComp && atkComp->isAttacking) {
        desiredClip = 2; // auto-attack
      } else if (animComp.clips.size() >= 2 && charComp->hasTarget) {
        desiredClip = 1; // walk
      }

      if (desiredClip != animComp.activeClipIndex) {
        animComp.activeClipIndex = desiredClip;
        animComp.player.setClip(desiredClip >= 0 ? &animComp.clips[desiredClip] : nullptr);
      }
    }

    animComp.player.update(deltaTime);
  }
}

bool Scene::getFirstLight(glm::vec3 &outPos, glm::vec3 &outColor) const {
  auto view = m_registry.view<TransformComponent, LightComponent>();
  for (auto entity : view) {
    auto &t = view.get<TransformComponent>(entity);
    auto &l = view.get<LightComponent>(entity);
    outPos = t.position;
    outColor = l.color * l.intensity;
    return true;
  }
  return false;
}

uint32_t Scene::getAllLights(
    std::vector<std::pair<glm::vec3, glm::vec3>> &outLights) const {
  outLights.clear();
  auto view = m_registry.view<TransformComponent, LightComponent>();
  for (auto entity : view) {
    auto &t = view.get<TransformComponent>(entity);
    auto &l = view.get<LightComponent>(entity);
    outLights.emplace_back(t.position, l.color * l.intensity);
  }
  return static_cast<uint32_t>(outLights.size());
}

} // namespace glory

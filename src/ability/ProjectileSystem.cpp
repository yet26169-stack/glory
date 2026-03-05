#include "ability/ProjectileSystem.h"

#include "ability/AbilityComponents.h"
#include "ability/AbilityDef.h"
#include "ability/VFXEventQueue.h"
#include "scene/Components.h"
#include <cmath>
#include <glm/glm.hpp>
#include <spdlog/spdlog.h>

namespace glory {

void ProjectileSystem::init(Scene &scene, uint32_t sphereMeshIndex,
                            uint32_t defaultTexIndex, uint32_t flatNormIndex) {
  m_sphereMesh = sphereMeshIndex;
  m_defaultTex = defaultTexIndex;
  m_flatNorm = flatNormIndex;
  m_initialized = true;
}

entt::entity ProjectileSystem::spawnSkillshot(
    Scene &scene, entt::entity caster, const std::string &abilityId, int level,
    const glm::vec3 &origin, const glm::vec3 &direction, float speed,
    float maxRange, const glm::vec4 &colour, float collisionRadius) {
  if (!m_initialized) {
    spdlog::warn("ProjectileSystem::spawnSkillshot called before init()");
    return entt::null;
  }

  glm::vec3 normDir = glm::length(direction) > 0.001f
                          ? glm::normalize(direction)
                          : glm::vec3(0.0f, 0.0f, 1.0f);

  auto entity = scene.createEntity("Projectile");
  auto &reg = scene.getRegistry();

  auto &t = reg.get<TransformComponent>(entity);
  t.position = origin;
  t.scale = glm::vec3(collisionRadius);

  reg.emplace<MeshComponent>(entity, MeshComponent{m_sphereMesh});
  reg.emplace<MaterialComponent>(
      entity, MaterialComponent{
                  m_defaultTex, m_flatNorm,
                  /*shininess=*/0.0f,
                  /*metallic=*/0.0f,
                  /*roughness=*/0.3f,
                  /*emissive=*/3.0f // glow makes it stand out
              });
  reg.emplace<ColorComponent>(entity, ColorComponent{colour});

  ProjectileComponent proj;
  proj.velocity = normDir * speed;
  proj.speed = speed;
  proj.maxRange = maxRange;
  proj.caster = caster;
  proj.radius = collisionRadius;
  proj.hitOnArrival = false;
  proj.abilityId = abilityId;
  proj.abilityLevel = level;
  reg.emplace<ProjectileComponent>(entity, proj);

  // Add a slow spin for visual interest
  reg.emplace<RotateComponent>(
      entity,
      RotateComponent{glm::normalize(glm::vec3(normDir.z, 0.3f, -normDir.x)),
                      4.0f});

  spdlog::info(">>> Spawned skillshot projectile at ({:.1f},{:.1f},{:.1f}) dir "
               "({:.2f},{:.2f},{:.2f})",
               origin.x, origin.y, origin.z, normDir.x, normDir.y, normDir.z);
  return entity;
}

entt::entity ProjectileSystem::spawnGroundAoE(
    Scene &scene, entt::entity caster, const std::string &abilityId, int level,
    const glm::vec3 &origin, const glm::vec3 &targetPos, float speed,
    float aoeRadius, const glm::vec4 &colour) {
  if (!m_initialized) {
    spdlog::warn("ProjectileSystem::spawnGroundAoE called before init()");
    return entt::null;
  }

  glm::vec3 toTarget = targetPos - origin;
  toTarget.y = 0.0f;
  float dist = glm::length(toTarget);
  if (dist < 0.001f)
    toTarget = glm::vec3(0.0f, 0.0f, 1.0f);
  glm::vec3 normDir = glm::normalize(toTarget);

  auto entity = scene.createEntity("AoEProjectile");
  auto &reg = scene.getRegistry();

  auto &t = reg.get<TransformComponent>(entity);
  t.position = origin;
  t.scale =
      glm::vec3(aoeRadius * 0.18f); // small while flying, "expands" on hit

  reg.emplace<MeshComponent>(entity, MeshComponent{m_sphereMesh});
  reg.emplace<MaterialComponent>(
      entity,
      MaterialComponent{m_defaultTex, m_flatNorm, 0.0f, 0.0f, 0.3f, 4.0f});
  reg.emplace<ColorComponent>(entity, ColorComponent{colour});

  ProjectileComponent proj;
  proj.velocity = normDir * speed;
  proj.speed = speed;
  proj.maxRange = dist;
  proj.caster = caster;
  proj.radius = aoeRadius;
  proj.hitOnArrival = true;
  proj.abilityId = abilityId;
  proj.abilityLevel = level;
  reg.emplace<ProjectileComponent>(entity, proj);

  reg.emplace<RotateComponent>(
      entity, RotateComponent{glm::vec3(0.4f, 1.0f, 0.3f), 6.0f});

  spdlog::info(
      ">>> Spawned AoE projectile toward ({:.1f},{:.1f},{:.1f}) range {:.1f}",
      targetPos.x, targetPos.y, targetPos.z, dist);
  return entity;
}

entt::entity ProjectileSystem::spawnTargeted(
    Scene &scene, entt::entity caster, entt::entity target,
    const std::string &abilityId, int level,
    float speed, const glm::vec4 &colour)
{
    auto &reg = scene.getRegistry();
    auto &casterT = reg.get<TransformComponent>(caster);

    auto e = scene.createEntity("TargetedProjectile");
    auto &t = reg.get<TransformComponent>(e);
    t.position = casterT.position + glm::vec3(0, 1.0f, 0);
    t.scale = glm::vec3(0.3f);

    // Direction will be updated each frame to track the target
    reg.emplace<MeshComponent>(e, MeshComponent{m_sphereMesh});
    reg.emplace<MaterialComponent>(e, MaterialComponent{m_defaultTex, m_flatNorm, 0, 0, 0.3f, 3.0f});
    reg.emplace<ColorComponent>(e, ColorComponent{colour});

    ProjectileComponent pc{};
    pc.velocity = glm::vec3(0); // updated per frame
    pc.speed = speed;
    pc.maxRange = 9999.0f; // targeted projectiles always hit
    pc.caster = caster;
    pc.hitOnArrival = false;
    pc.abilityId = abilityId;
    pc.abilityLevel = level;
    reg.emplace<ProjectileComponent>(e, pc);

    reg.emplace<TargetedProjectileTag>(e, TargetedProjectileTag{target});
    reg.emplace<RotateComponent>(e, RotateComponent{{0, 1, 0}, 8.0f});

    return e;
}

void ProjectileSystem::update(Scene &scene, float dt) {
  auto &reg = scene.getRegistry();
  auto view = reg.view<TransformComponent, ProjectileComponent>();

  std::vector<entt::entity> toDestroy;

  for (auto entity : view) {
    auto &t = view.get<TransformComponent>(entity);
    auto &proj = view.get<ProjectileComponent>(entity);

    bool destroyed = false;
    const AbilityDefinition *def = AbilityDatabase::get().find(proj.abilityId);

    // 0. Handle targeted homing
    if (reg.all_of<TargetedProjectileTag>(entity)) {
        auto &tag = reg.get<TargetedProjectileTag>(entity);
        if (tag.target == entt::null || !reg.valid(tag.target) || !reg.all_of<TransformComponent>(tag.target)) {
            destroyed = true;
        } else {
            auto &targetT = reg.get<TransformComponent>(tag.target);
            glm::vec3 dir = targetT.position - t.position;
            float dist = glm::length(dir);
            if (dist < 0.5f) {
                // Hit target
                // Let the normal collision logic handle it or do it here. Let's do it here.
                if (def) {
                    auto queueView = reg.view<EffectQueueComponent>();
                    entt::entity queueEntity = queueView.empty() ? entt::null : queueView.front();
                    if (queueEntity != entt::null) {
                        auto &queue = reg.get<EffectQueueComponent>(queueEntity);
                        for (const auto &effect : def->onHitEffects) {
                            queue.enqueue(proj.caster, tag.target, &effect, proj.abilityLevel);
                        }
                    }
                    if (!def->impactVFX.empty()) {
                        VFXEventQueue::get().enqueue(VFXEventType::IMPACT, t.position, glm::vec3(0,1,0), def->impactVFX, def->impactSFX);
                    }
                }
                destroyed = true;
            } else {
                proj.velocity = glm::normalize(dir) * proj.speed;
            }
        }
    }

    // 1. Move projectile
    float stepDist = proj.speed * dt;
    t.position += proj.velocity * dt;
    if (!reg.all_of<TargetedProjectileTag>(entity)) {
        t.position.y = std::max(t.position.y, 0.4f); // Stay around ground level for non-targeted
    }
    proj.travelledDistance += stepDist;

    // 2. Collision checks against characters (only if not already destroyed by targeted logic)
    if (def && !destroyed && !reg.all_of<TargetedProjectileTag>(entity)) {
      auto charView = reg.view<TransformComponent, CharacterComponent>();

      // We'll store the global EffectQueue on the first Character for now,
      // or just create a dummy "Level" entity if needed. Let's find one.
      entt::entity queueEntity = entt::null;
      auto queueView = reg.view<EffectQueueComponent>();
      if (queueView.begin() != queueView.end()) {
        queueEntity = queueView.front();
      } else if (charView.begin() != charView.end()) {
        // Just attach the queue to the first character as a global state holder
        queueEntity = charView.front();
        reg.emplace<EffectQueueComponent>(queueEntity);
      }

      // A) Travelling Skillshot - collides with characters in path
      if (!proj.hitOnArrival) {
        for (auto target : charView) {
          if (target == proj.caster)
            continue; // don't hit self

          auto &targetPos = charView.get<TransformComponent>(target).position;
          float dist = glm::distance(t.position, targetPos);

          // Simple sphere overlap
          float collisionDist =
              proj.radius + 1.0f; // 1.0f = roughly char radius
          if (dist < collisionDist) {
            // HIT! Queue effects
            if (queueEntity != entt::null) {
              auto &queue = reg.get<EffectQueueComponent>(queueEntity);
              for (const auto &effect : def->onHitEffects) {
                queue.enqueue(proj.caster, target, &effect, proj.abilityLevel);
              }
            }

            // Queue VFX impact
            if (!def->impactVFX.empty()) {
              VFXEventQueue::get().enqueue(VFXEventType::IMPACT, t.position,
                                           proj.velocity, def->impactVFX,
                                           def->impactSFX);
            }

            // For now, assume all projectiles are non-piercing
            destroyed = true;
            break;
          }
        }
      }

      // B) Ground AoE - detonates when max range is reached
      if (proj.hitOnArrival && proj.travelledDistance >= proj.maxRange) {
        for (auto target : charView) {
          if (target == proj.caster)
            continue;

          auto &targetPos = charView.get<TransformComponent>(target).position;
          float dist = glm::distance(t.position, targetPos);

          if (dist < proj.radius) {
            if (queueEntity != entt::null) {
              auto &queue = reg.get<EffectQueueComponent>(queueEntity);
              for (const auto &effect : def->onHitEffects) {
                queue.enqueue(proj.caster, target, &effect, proj.abilityLevel);
              }
            }
          }
        }

        // Queue VFX impact for the AoE
        if (!def->impactVFX.empty()) {
          VFXEventQueue::get().enqueue(VFXEventType::IMPACT, t.position,
                                       glm::vec3(0, 1, 0), def->impactVFX,
                                       def->impactSFX);
        }

        destroyed = true;
      }
    }

    // 3. Lifetime check
    if (!destroyed && proj.travelledDistance >= proj.maxRange) {
      destroyed = true;
    }

    if (destroyed) {
      toDestroy.push_back(entity);
    }
  }

  for (auto e : toDestroy) {
    reg.destroy(e);
  }
}

} // namespace glory

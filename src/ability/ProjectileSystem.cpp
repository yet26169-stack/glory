#include "ability/ProjectileSystem.h"
#include "ability/AbilitySystem.h"
#include "combat/GpuCollisionSystem.h"
#include "vfx/TrailRenderer.h"
#include "scene/Components.h"
#include "combat/CombatComponents.h"
#include "core/FixedPoint.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace glory {

static constexpr float PROJECTILE_COLLISION_RADIUS = 0.8f;

void ProjectileSystem::registerAbilityMesh(const std::string& abilityID,
                                            ProjectileMeshInfo info) {
    m_abilityMeshes[abilityID] = info;
}

void ProjectileSystem::update(entt::registry& reg, float dt,
                               VFXEventQueue& vfxQueue, AbilitySystem& abilitySystem,
                               TrailRenderer* trailRenderer,
                               const GpuCollisionSystem* gpuCollision) {
    m_landedPositions.clear();
    std::vector<entt::entity> toDestroy;

    auto view = reg.view<ProjectileComponent, TransformComponent>();
    for (auto [entity, pc, tc] : view.each()) {

        // ── Homing (Targeted) projectile ───────────────────────────────────
        if (pc.targetEntity != NULL_ENTITY) {
            entt::entity target = static_cast<entt::entity>(pc.targetEntity);
            if (!reg.valid(target) || !reg.all_of<TransformComponent>(target)) {
                toDestroy.push_back(entity);
                continue;
            }

            auto& ttc = reg.get<TransformComponent>(target);
            glm::vec3 targetCenter = ttc.position + glm::vec3(0, 1.0f, 0); // target chest
            glm::vec3 toTarget = targetCenter - tc.position;
            float dist = glm::length(toTarget);

            if (dist < 0.5f) {
                // HIT!
                if (pc.isAutoAttack) {
                    // Apply auto-attack damage (deterministic fixed-point)
                    if (reg.all_of<StatsComponent>(target)) {
                        auto& stats = reg.get<StatsComponent>(target);
                        Fixed32 fDamage(pc.damage);
                        Fixed32 fArmor(stats.total().armor);
                        Fixed32 fFinal = fDamage * (Fixed32(100) / (Fixed32(100) + fArmor));
                        stats.base.currentHP = std::max(0.0f, stats.base.currentHP - fFinal.toFloat());
                        
                        // Hit VFX
                        VFXEvent ev{};
                        ev.type = VFXEventType::Spawn;
                        std::strncpy(ev.effectID, "vfx_melee_hit", sizeof(ev.effectID) - 1);
                        ev.position = tc.position;
                        vfxQueue.push(ev);
                    }
                } else if (pc.sourceDef) {
                    TargetInfo ti{};
                    ti.type = TargetingType::TARGETED;
                    ti.targetEntity = pc.targetEntity;
                    ti.targetPosition = ttc.position;
                    abilitySystem.resolveHit(reg, static_cast<entt::entity>(pc.casterEntity),
                                             *pc.sourceDef, ti);
                }

                toDestroy.push_back(entity);
                destroyProjectile(reg, entity, pc, vfxQueue, &abilitySystem,
                                  tc.position, true, trailRenderer);
                continue;
            }

            glm::vec3 dir = toTarget / dist;
            pc.velocity = dir * pc.speed;
            tc.position += pc.velocity * dt;

            // Orient model
            tc.rotation.y = std::atan2(dir.x, dir.z);
            tc.rotation.x = -std::asin(dir.y);

            // Keep VFX trail locked to projectile position
            for (auto vfxH : pc.vfxHandles) {
                VFXEvent mv{};
                mv.type     = VFXEventType::Move;
                mv.handle   = vfxH;
                mv.position = tc.position;
                vfxQueue.push(mv);
            }
            if (pc.trailHandle != 0 && trailRenderer) {
                trailRenderer->updateHead(pc.trailHandle, tc.position);
            }
            continue;
        }

        // ── Lob (arc) projectile — quadratic Bezier ────────────────────────
        if (pc.isLob) {
            pc.lobElapsed += dt;
            float t  = glm::clamp(pc.lobElapsed / pc.lobFlightTime, 0.0f, 1.0f);
            float t1 = 1.0f - t;

            // Quadratic Bezier: B(t) = t1²·P0 + 2·t1·t·P1 + t²·P2
            tc.position = t1*t1 * pc.lobOrigin
                        + 2.0f*t1*t * pc.lobApex
                        + t*t * pc.lobTarget;

            // Tilt the bomb forward on descent for visual flair
            if (t > 0.5f)
                tc.rotation.x = glm::mix(0.0f, 1.6f, (t - 0.5f) * 2.0f);

            // Keep trail VFX locked to bomb position
            if (!pc.vfxHandles.empty()) {
                for (auto vfxH : pc.vfxHandles) {
                    VFXEvent mv{};
                    mv.type     = VFXEventType::Move;
                    mv.handle   = vfxH;
                    mv.position = tc.position;
                    vfxQueue.push(mv);
                }
            }

            if (pc.trailHandle != 0 && trailRenderer) {
                trailRenderer->updateHead(pc.trailHandle, tc.position);
            }

            if (t >= 1.0f) {
                // Landed: AoE hit + explosion VFX + record landing position
                TargetInfo ti{};
                ti.type           = TargetingType::POINT;
                ti.targetPosition = pc.lobTarget;
                abilitySystem.resolveHit(reg, static_cast<entt::entity>(pc.casterEntity),
                                         *pc.sourceDef, ti);

                m_landedPositions.push_back(pc.lobTarget);

                toDestroy.push_back(entity);
                destroyProjectile(reg, entity, pc, vfxQueue, &abilitySystem,
                                  pc.lobTarget, true, trailRenderer);
            }
            continue;
        }
        // Attach mesh on first frame if we have a model registered for this ability
        if (!reg.all_of<MeshComponent>(entity) && pc.sourceDef) {
            auto it = m_abilityMeshes.find(pc.sourceDef->id);
            if (it != m_abilityMeshes.end()) {
                const auto& mi = it->second;
                reg.emplace<MeshComponent>(entity, MeshComponent{ mi.meshIndex });
                reg.emplace<MaterialComponent>(entity,
                    MaterialComponent{ mi.texIndex, mi.normalIndex, 0.f, 0.5f, 0.5f, 0.3f });
                tc.scale = mi.scale;

                // Orient the model so its Z-forward aligns with the velocity direction
                const float speed = glm::length(pc.velocity);
                if (speed > 0.001f) {
                    glm::vec3 dir = pc.velocity / speed;
                    tc.rotation.y = std::atan2(dir.x, dir.z);
                    tc.rotation.x = 0.f;
                    tc.rotation.z = 0.f;
                }
            }
        }

        // Accelerate projectile each frame (e.g. Q bolt speeds up in flight)
        if (pc.acceleration > 0.0f) {
            float curSpeed = glm::length(pc.velocity);
            if (curSpeed > 0.001f) {
                float newSpeed = std::min(curSpeed + pc.acceleration * dt, pc.maxSpeed);
                pc.velocity = (pc.velocity / curSpeed) * newSpeed;
            }
        }

        const float speed    = glm::length(pc.velocity);
        const float moveDist = speed * dt;
        tc.position         += pc.velocity * dt;
        pc.traveledDist     += moveDist;

        // Keep VFX trail locked to projectile position
        for (auto vfxH : pc.vfxHandles) {
            VFXEvent mv{};
            mv.type     = VFXEventType::Move;
            mv.handle   = vfxH;
            mv.position = tc.position + glm::vec3(0.f, 0.5f, 0.f);
            vfxQueue.push(mv);
        }

        if (pc.trailHandle != 0 && trailRenderer) {
            trailRenderer->updateHead(pc.trailHandle, tc.position + glm::vec3(0.f, 0.5f, 0.f));
        }

        // Expired: exceeded max range
        if (pc.traveledDist >= pc.maxRange) {
            toDestroy.push_back(entity);
            destroyProjectile(reg, entity, pc, vfxQueue, nullptr,
                              tc.position, false, trailRenderer);
            continue;
        }

        // Collision against enemies — use GPU spatial hash results if available,
        // else fall back to CPU O(N) scan
        Team casterTeam = Team::NEUTRAL;
        auto casterEntt = static_cast<entt::entity>(pc.casterEntity);
        if (reg.valid(casterEntt) && reg.all_of<TeamComponent>(casterEntt))
            casterTeam = reg.get<TeamComponent>(casterEntt).team;

        const float hitRadius = (pc.sourceDef && pc.sourceDef->projectile.width > 0.f)
                                ? pc.sourceDef->projectile.width * 0.5f
                                : PROJECTILE_COLLISION_RADIUS;

        bool hitSomething = false;

        if (gpuCollision && !gpuCollision->getCollisionResults().empty()) {
            // GPU path: check cached collision pairs for this projectile
            for (const auto& pair : gpuCollision->getCollisionResults()) {
                entt::entity targetEnt = entt::null;
                if (pair.entityA == entity) targetEnt = pair.entityB;
                else if (pair.entityB == entity) targetEnt = pair.entityA;
                else continue;

                if (!reg.valid(targetEnt)) continue;
                if (targetEnt == casterEntt) continue;
                if (!reg.all_of<TeamComponent, StatsComponent, TransformComponent>(targetEnt)) continue;
                if (reg.get<TeamComponent>(targetEnt).team == casterTeam) continue;

                // Refine with exact distance check
                const float dist = glm::length(tc.position - reg.get<TransformComponent>(targetEnt).position);
                if (dist <= hitRadius + PROJECTILE_COLLISION_RADIUS) {
                    TargetInfo ti{};
                    ti.type           = TargetingType::TARGETED;
                    ti.targetEntity   = static_cast<EntityID>(entt::to_integral(targetEnt));
                    ti.targetPosition = reg.get<TransformComponent>(targetEnt).position;

                    abilitySystem.resolveHit(reg, casterEntt, *pc.sourceDef, ti);
                    pc.hitCount++;
                    hitSomething = true;

                    if (!pc.piercing || pc.hitCount >= pc.maxTargets)
                        break;
                }
            }
        } else {
            // CPU fallback: O(N) scan
            auto targets = reg.view<TransformComponent, TeamComponent>();
            for (auto [targetEnt, ttc, team] : targets.each()) {
                if (targetEnt == entity)    continue;
                if (targetEnt == casterEntt) continue;
                if (team.team == casterTeam) continue;
                if (!reg.all_of<StatsComponent>(targetEnt)) continue;

                const float dist = glm::length(tc.position - ttc.position);
                if (dist <= hitRadius + PROJECTILE_COLLISION_RADIUS) {
                    TargetInfo ti{};
                    ti.type           = TargetingType::TARGETED;
                    ti.targetEntity   = static_cast<EntityID>(entt::to_integral(targetEnt));
                    ti.targetPosition = ttc.position;

                    abilitySystem.resolveHit(reg, casterEntt, *pc.sourceDef, ti);
                    pc.hitCount++;
                    hitSomething = true;

                    if (!pc.piercing || pc.hitCount >= pc.maxTargets)
                        break;
                }
            }
        }

        if (hitSomething && (!pc.piercing || pc.hitCount >= pc.maxTargets)) {
            toDestroy.push_back(entity);
            destroyProjectile(reg, entity, pc, vfxQueue, &abilitySystem,
                              tc.position, true, trailRenderer);
        }
    }

    for (auto e : toDestroy)
        if (reg.valid(e))
            reg.destroy(e);
}

void ProjectileSystem::destroyProjectile(entt::registry& reg, entt::entity /*e*/,
                                          const ProjectileComponent& pc,
                                          VFXEventQueue& vfxQueue,
                                          AbilitySystem* abilitySystem,
                                          const glm::vec3& hitPos,
                                          bool applyHit,
                                          TrailRenderer* trailRenderer) {
    // Stop looping trail VFX
    for (auto vfxH : pc.vfxHandles) {
        VFXEvent ev{};
        ev.type   = VFXEventType::Destroy;
        ev.handle = vfxH;
        vfxQueue.push(ev);
    }

    if (pc.trailHandle != 0 && trailRenderer) {
        trailRenderer->detach(pc.trailHandle);
    }

    // Impact VFX
    if (applyHit && abilitySystem && pc.sourceDef && !pc.sourceDef->impactVFX.empty()) {
        const glm::vec3 dir = glm::length(pc.velocity) > 0.001f
                              ? glm::normalize(pc.velocity)
                              : glm::vec3(0, 1, 0);
        for (const auto& impactID : pc.sourceDef->impactVFX) {
            abilitySystem->emitVFXPublic(impactID, hitPos, dir);
        }
    }
}

} // namespace glory


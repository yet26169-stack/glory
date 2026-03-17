#include "combat/CombatSystem.h"
#include "combat/EconomySystem.h"
#include "audio/GameAudioEvents.h"
#include "scene/Components.h"
#include "ability/AbilityComponents.h"
#include "ability/AbilityTypes.h"
#include "core/FixedPoint.h"

#include <spdlog/spdlog.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstring>

namespace glory {

CombatSystem::CombatSystem(VFXEventQueue& vfxQueue) : m_vfxQueue(vfxQueue) {}

// ── Input entry-points ──────────────────────────────────────────────────────

void CombatSystem::requestAutoAttack(entt::entity attacker, entt::entity target) {
    // Caller is responsible for range/cooldown/state checks
    // We just set state and windup timer
    // (CombatComponent must already exist on attacker)
    // NOTE: registry access is deferred — we only store the target entity
}

void CombatSystem::requestShield(entt::entity entity, entt::registry& reg) {
    if (!reg.all_of<CombatComponent, TransformComponent>(entity)) return;
    auto& combat = reg.get<CombatComponent>(entity);
    if (combat.state != CombatState::IDLE || combat.shieldCooldown > 0.0f) return;

    combat.state = CombatState::SHIELDING;
    combat.stateTimer = combat.shieldDuration;

    auto& t = reg.get<TransformComponent>(entity);
    combat.shieldVfxHandle = static_cast<uint32_t>(entity) | 0x80000000;
    emitVFX("vfx_shield", t.position, glm::vec3(0, 1, 0), 1.0f, combat.shieldVfxHandle);
    emitVFX("vfx_molten_shield_apply", t.position, glm::vec3(0, 1, 0));
    emitVFX("vfx_molten_shield_embers", t.position, glm::vec3(0, 1, 0));
}

void CombatSystem::releaseShield(entt::entity entity, entt::registry& reg) {
    if (!reg.all_of<CombatComponent>(entity)) return;
    auto& combat = reg.get<CombatComponent>(entity);
    if (combat.state != CombatState::SHIELDING) return;

    combat.state = CombatState::IDLE;
    combat.shieldCooldown = combat.shieldCooldownBase * 0.5f; // shorter CD for manual release

    if (combat.shieldVfxHandle != 0) {
        VFXEvent ev{};
        ev.type = VFXEventType::Destroy;
        ev.handle = combat.shieldVfxHandle;
        m_vfxQueue.push(ev);
        combat.shieldVfxHandle = 0;
    }
}

void CombatSystem::requestTrick(entt::entity attacker, entt::entity target,
                                 entt::registry& reg) {
    if (!reg.all_of<CombatComponent>(attacker)) return;
    auto& combat = reg.get<CombatComponent>(attacker);
    if (combat.state != CombatState::IDLE || combat.trickCooldown > 0.0f) return;

    combat.state = CombatState::TRICKING;
    combat.stateTimer = combat.trickWindup;
    combat.targetEntity = target;

    auto& t = reg.get<TransformComponent>(attacker);
    glm::vec3 dir = glm::normalize(reg.get<TransformComponent>(target).position - t.position);
    emitVFX("vfx_trick_cast", t.position, dir);
    emitVFX("vfx_trick", t.position, dir);
}

// ── Per-frame update ────────────────────────────────────────────────────────

void CombatSystem::update(entt::registry& registry, float dt) {
    auto view = registry.view<CombatComponent>();
    for (auto [entity, combat] : view.each()) {
        // Tick cooldowns
        combat.attackCooldown = std::max(0.0f, combat.attackCooldown - dt);
        combat.shieldCooldown = std::max(0.0f, combat.shieldCooldown - dt);
        combat.trickCooldown  = std::max(0.0f, combat.trickCooldown  - dt);

        switch (combat.state) {
        case CombatState::ATTACK_WINDUP:
            processAttackWindup(registry, entity, combat, dt);
            break;
        case CombatState::ATTACK_FIRE:
            processAttackFire(registry, entity, combat, dt);
            break;
        case CombatState::ATTACK_WINDDOWN:
            processAttackWinddown(registry, entity, combat, dt);
            break;
        case CombatState::SHIELDING:
            processShield(registry, entity, combat, dt);
            break;
        case CombatState::TRICKING:
            processTrick(registry, entity, combat, dt);
            break;
        case CombatState::STUNNED:
            processStun(combat, dt);
            break;
        case CombatState::IDLE:
            break;
        }
    }
}

// ── State processors ────────────────────────────────────────────────────────

void CombatSystem::processAttackWindup(entt::registry& reg, entt::entity entity,
                                        CombatComponent& combat, float dt) {
    combat.stateTimer -= dt;
    if (combat.stateTimer <= 0.0f) {
        combat.state = CombatState::ATTACK_FIRE;
        combat.stateTimer = 0.0f; // Instant transition to FIRE phase
    }
}

void CombatSystem::processAttackFire(entt::registry& reg, entt::entity entity,
                                      CombatComponent& combat, float dt) {
    // Determine the fire point logic (damage vs projectile)
    if (combat.isRanged) {
        spawnAutoAttackProjectile(reg, entity, combat.targetEntity, combat);
    } else {
        applyAutoAttackHit(reg, entity, combat.targetEntity);
    }

    // Set internal attack cooldown based on AttackSpeed (deterministic)
    Fixed32 fAttackSpeed(combat.attackSpeed);
    Fixed32 fCycleTime = Fixed32::one() / fAttackSpeed;
    combat.attackCooldown = fCycleTime.toFloat();

    // Transition to Wind-down
    combat.state = CombatState::ATTACK_WINDDOWN;
    Fixed32 fWindupPercent(combat.windupPercent);
    combat.stateTimer = (fCycleTime * (Fixed32::one() - fWindupPercent)).toFloat();
}

void CombatSystem::processAttackWinddown(entt::registry& reg, entt::entity entity,
                                          CombatComponent& combat, float dt) {
    combat.stateTimer -= dt;
    if (combat.stateTimer <= 0.0f) {
        combat.state = CombatState::IDLE;
        combat.targetEntity = entt::null;
    }
}

void CombatSystem::processShield(entt::registry& reg, entt::entity entity,
                                  CombatComponent& combat, float dt) {
    combat.stateTimer -= dt;
    if (combat.stateTimer <= 0.0f) {
        // Shield expired from duration
        combat.state = CombatState::IDLE;
        combat.shieldCooldown = combat.shieldCooldownBase;
        
        if (combat.shieldVfxHandle != 0) {
            VFXEvent ev{};
            ev.type = VFXEventType::Destroy;
            ev.handle = combat.shieldVfxHandle;
            m_vfxQueue.push(ev);
            combat.shieldVfxHandle = 0;
        }
    } else {
        if (combat.shieldVfxHandle != 0 && reg.all_of<TransformComponent>(entity)) {
            auto& t = reg.get<TransformComponent>(entity);
            VFXEvent ev{};
            ev.type = VFXEventType::Move;
            ev.handle = combat.shieldVfxHandle;
            ev.position = t.position;
            m_vfxQueue.push(ev);
        }
    }
}

void CombatSystem::processTrick(entt::registry& reg, entt::entity entity,
                                 CombatComponent& combat, float dt) {
    combat.stateTimer -= dt;
    if (combat.stateTimer <= 0.0f) {
        applyTrickHit(reg, entity, combat.targetEntity, combat);
        combat.trickCooldown = combat.trickCooldownBase;
        // State is set inside applyTrickHit (STUNNED if failed, IDLE if success)
        if (combat.state == CombatState::TRICKING) {
            combat.state = CombatState::IDLE;
        }
        combat.targetEntity = entt::null;
    }
}

void CombatSystem::processStun(CombatComponent& combat, float dt) {
    combat.stateTimer -= dt;
    if (combat.stateTimer <= 0.0f) {
        combat.state = CombatState::IDLE;
    }
}

// ── Hit resolution ──────────────────────────────────────────────────────────

void CombatSystem::spawnAutoAttackProjectile(entt::registry& reg, entt::entity attacker,
                                              entt::entity target, CombatComponent& combat) {
    if (!reg.valid(target) || !reg.all_of<TransformComponent>(target))
        return;

    auto& attackerPos = reg.get<TransformComponent>(attacker).position;
    auto& targetPos   = reg.get<TransformComponent>(target).position;
    glm::vec3 spawnPos = attackerPos + glm::vec3(0, 1.0f, 0); // spawn at chest height

    entt::entity proj = reg.create();
    reg.emplace<TransformComponent>(proj, TransformComponent{spawnPos, glm::vec3(0), glm::vec3(1.0f)});
    
    auto& pc = reg.emplace<ProjectileComponent>(proj);
    pc.casterEntity = static_cast<EntityID>(entt::to_integral(attacker));
    pc.targetEntity = static_cast<EntityID>(entt::to_integral(target));
    pc.speed        = combat.projectileSpeed;
    pc.damage       = combat.attackDamage;
    pc.isAutoAttack = true;

    // Spawn flight VFX
    if (!combat.projectileVfx.empty()) {
        uint32_t vfxHandle = (static_cast<uint32_t>(entt::to_integral(proj)) + 1) * 1000;
        pc.vfxHandles.push_back(vfxHandle);
        emitVFX(combat.projectileVfx, spawnPos, glm::normalize(targetPos - attackerPos), 1.0f, vfxHandle);
    }
}

void CombatSystem::applyAutoAttackHit(entt::registry& reg, entt::entity attacker,
                                       entt::entity target) {
    if (!reg.valid(target) || !reg.all_of<CombatComponent, TransformComponent>(target))
        return;

    auto& targetCombat = reg.get<CombatComponent>(target);
    auto& attackerPos  = reg.get<TransformComponent>(attacker).position;
    auto& targetPos    = reg.get<TransformComponent>(target).position;
    glm::vec3 dir      = glm::normalize(targetPos - attackerPos);

    // Emit melee slash + hit VFX at midpoint
    glm::vec3 midpoint = (attackerPos + targetPos) * 0.5f;
    emitVFX("vfx_melee_slash", midpoint, dir);
    emitVFX("vfx_melee_hit", midpoint, dir);

    // Audio: auto-attack sound at impact position
    if (m_audio) m_audio->onAutoAttack(midpoint, false);

    if (targetCombat.state == CombatState::SHIELDING) {
        // BLOCKED
        emitVFX("vfx_attack_blocked", targetPos, dir);
        spdlog::debug("Auto-attack blocked by shield");
    } else {
        // HIT — apply damage (deterministic fixed-point arithmetic)
        if (reg.all_of<StatsComponent>(target)) {
            auto& stats = reg.get<StatsComponent>(target);
            auto& attackerCombat = reg.get<CombatComponent>(attacker);
            Fixed32 fDamage(attackerCombat.attackDamage);
            Fixed32 fArmor(stats.total().armor);
            Fixed32 fFinal = fDamage * (Fixed32(100) / (Fixed32(100) + fArmor));
            float finalDamage = fFinal.toFloat();
            stats.base.currentHP = std::max(0.0f, stats.base.currentHP - finalDamage);
            spdlog::debug("Auto-attack hit for {:.1f} damage (HP: {:.1f})",
                          finalDamage, stats.base.currentHP);

            // Kill detection: award gold/xp if target died
            if (stats.base.currentHP <= 0.0f && m_economy) {
                m_economy->awardKill(reg, attacker, target);
            }
        }
    }
}

void CombatSystem::applyTrickHit(entt::registry& reg, entt::entity attacker,
                                  entt::entity target, CombatComponent& attackerCombat) {
    if (!reg.valid(target) || !reg.all_of<CombatComponent, TransformComponent>(target))
        return;

    auto& targetCombat = reg.get<CombatComponent>(target);
    auto& attackerPos  = reg.get<TransformComponent>(attacker).position;
    auto& targetPos    = reg.get<TransformComponent>(target).position;
    glm::vec3 dir      = glm::normalize(targetPos - attackerPos);

    if (targetCombat.state == CombatState::SHIELDING) {
        // Trick beats Shield — stun the defender
        targetCombat.state      = CombatState::STUNNED;
        targetCombat.stateTimer = attackerCombat.stunDuration;
        attackerCombat.state    = CombatState::IDLE;
        emitVFX("vfx_trick_success", targetPos, dir);
        spdlog::debug("Trick broke shield — defender stunned for {:.1f}s",
                      attackerCombat.stunDuration);
    } else {
        // Trick fails — stun the attacker
        attackerCombat.state      = CombatState::STUNNED;
        attackerCombat.stateTimer = attackerCombat.stunDuration;
        emitVFX("vfx_trick_fail", attackerPos, -dir);
        spdlog::debug("Trick failed — attacker stunned for {:.1f}s",
                      attackerCombat.stunDuration);
    }
}

// ── Utility ─────────────────────────────────────────────────────────────────

entt::entity CombatSystem::findNearestEnemy(entt::registry& reg, entt::entity attacker,
                                             float range) {
    if (!reg.all_of<TransformComponent, TeamComponent>(attacker)) return entt::null;

    auto& attackerPos  = reg.get<TransformComponent>(attacker).position;
    auto  attackerTeam = reg.get<TeamComponent>(attacker).team;

    float bestDistSq = range * range;
    entt::entity best = entt::null;

    auto view = reg.view<TransformComponent, TeamComponent>();
    for (auto [entity, transform, team] : view.each()) {
        if (entity == attacker) continue;
        if (team.team == attackerTeam) continue;

        glm::vec2 a2d(attackerPos.x, attackerPos.z);
        glm::vec2 e2d(transform.position.x, transform.position.z);
        glm::vec2 diff = e2d - a2d;
        float distSq = diff.x * diff.x + diff.y * diff.y;
        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            best = entity;
        }
    }
    return best;
}

void CombatSystem::emitVFX(const std::string& effectId, const glm::vec3& pos,
                            const glm::vec3& dir, float scale, uint32_t handle) {
    VFXEvent ev{};
    ev.type = VFXEventType::Spawn;
    std::strncpy(ev.effectID, effectId.c_str(), sizeof(ev.effectID) - 1);
    ev.position  = pos;
    ev.direction = dir;
    ev.scale     = scale;
    ev.handle    = handle;
    ev.lifetime  = -1.0f; // use EmitterDef duration

    if (!m_vfxQueue.push(ev)) {
        spdlog::warn("CombatSystem: VFX queue full, dropped '{}'", effectId);
    }
}

} // namespace glory

#include "combat/CombatSystem.h"
#include "scene/Components.h"
#include "ability/AbilityComponents.h"
#include "ability/AbilityTypes.h"

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
    emitVFX("vfx_trick", t.position,
            glm::normalize(reg.get<TransformComponent>(target).position - t.position));
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
        case CombatState::AUTO_ATTACKING:
            processAutoAttack(registry, entity, combat, dt);
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

void CombatSystem::processAutoAttack(entt::registry& reg, entt::entity entity,
                                      CombatComponent& combat, float dt) {
    combat.stateTimer -= dt;
    if (combat.stateTimer <= 0.0f) {
        applyAutoAttackHit(reg, entity, combat.targetEntity);
        combat.attackCooldown = 1.0f / combat.attackSpeed;
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

void CombatSystem::applyAutoAttackHit(entt::registry& reg, entt::entity attacker,
                                       entt::entity target) {
    if (!reg.valid(target) || !reg.all_of<CombatComponent, TransformComponent>(target))
        return;

    auto& targetCombat = reg.get<CombatComponent>(target);
    auto& attackerPos  = reg.get<TransformComponent>(attacker).position;
    auto& targetPos    = reg.get<TransformComponent>(target).position;
    glm::vec3 dir      = glm::normalize(targetPos - attackerPos);

    // Emit slash VFX at midpoint
    glm::vec3 midpoint = (attackerPos + targetPos) * 0.5f;
    emitVFX("vfx_auto_attack", midpoint, dir);

    if (targetCombat.state == CombatState::SHIELDING) {
        // BLOCKED
        emitVFX("vfx_attack_blocked", targetPos, dir);
        spdlog::debug("Auto-attack blocked by shield");
    } else {
        // HIT — apply damage
        if (reg.all_of<StatsComponent>(target)) {
            auto& stats = reg.get<StatsComponent>(target);
            auto& attackerCombat = reg.get<CombatComponent>(attacker);
            float damage = attackerCombat.attackDamage;
            float effectiveArmor = stats.total().armor;
            float finalDamage = damage * (100.0f / (100.0f + effectiveArmor));
            stats.base.currentHP = std::max(0.0f, stats.base.currentHP - finalDamage);
            spdlog::debug("Auto-attack hit for {:.1f} damage (HP: {:.1f})",
                          finalDamage, stats.base.currentHP);
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

#pragma once

#include "combat/CombatComponents.h"
#include "vfx/VFXEventQueue.h"

#include <entt.hpp>
#include <glm/glm.hpp>
#include <string>
#include <cstring>

namespace glory {

class CombatSystem {
public:
    explicit CombatSystem(VFXEventQueue& vfxQueue);

    // ── Input requests (called from Renderer in response to player keys) ──
    void requestAutoAttack(entt::entity attacker, entt::entity target);
    void requestShield(entt::entity entity, entt::registry& reg);
    void releaseShield(entt::entity entity, entt::registry& reg);
    void requestTrick(entt::entity attacker, entt::entity target, entt::registry& reg);

    // ── Per-frame tick ────────────────────────────────────────────────────
    void update(entt::registry& registry, float dt);

    // ── Utility ───────────────────────────────────────────────────────────
    entt::entity findNearestEnemy(entt::registry& reg, entt::entity attacker, float range);

private:
    VFXEventQueue& m_vfxQueue;

    void processAttackWindup(entt::registry& reg, entt::entity entity, CombatComponent& combat, float dt);
    void processAttackFire(entt::registry& reg, entt::entity entity, CombatComponent& combat, float dt);
    void processAttackWinddown(entt::registry& reg, entt::entity entity, CombatComponent& combat, float dt);
    void processShield(entt::registry& reg, entt::entity entity, CombatComponent& combat, float dt);
    void processTrick(entt::registry& reg, entt::entity entity, CombatComponent& combat, float dt);
    void processStun(CombatComponent& combat, float dt);

    void applyAutoAttackHit(entt::registry& reg, entt::entity attacker, entt::entity target);
    void spawnAutoAttackProjectile(entt::registry& reg, entt::entity attacker, entt::entity target, CombatComponent& combat);
    void applyTrickHit(entt::registry& reg, entt::entity attacker,
                       entt::entity target, CombatComponent& attackerCombat);

    void emitVFX(const std::string& effectId, const glm::vec3& pos,
                 const glm::vec3& dir = glm::vec3(0, 1, 0), float scale = 1.0f, uint32_t handle = 0);
};

} // namespace glory

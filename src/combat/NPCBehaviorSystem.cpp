#include "combat/NPCBehaviorSystem.h"

#include "ability/AbilitySystem.h"
#include "ability/AbilityComponents.h"
#include "ability/AbilityTypes.h"
#include "combat/MinionWaveSystem.h"
#include "scene/Components.h"

#include <glm/glm.hpp>
#include <spdlog/spdlog.h>
#include <cmath>

namespace glory {

// ── lazyInitAbilities ─────────────────────────────────────────────────────────
void NPCBehaviorSystem::lazyInitAbilities(entt::registry& reg, entt::entity e,
                                          AbilitySystem& abilities,
                                          uint8_t /*abilitySetId*/) {
    // Give the entity a Q ability (fireball works as a generic projectile).
    // All four slots are passed; W/E/R are empty strings → skipped by initEntity.
    abilities.initEntity(reg, e, {"fire_mage_fireball", "", "", ""});
    abilities.setAbilityLevel(reg, e, AbilitySlot::Q, 1);

    // Stagger initial cooldown so the whole wave doesn't fire simultaneously.
    // Each successive minion gets an extra 0.3 s offset (wraps at 3 s).
    if (reg.all_of<AbilityBookComponent>(e)) {
        auto& inst = reg.get<AbilityBookComponent>(e)
                        .abilities[static_cast<size_t>(AbilitySlot::Q)];
        inst.cooldownRemaining = std::fmod(m_initCounter * 0.3f, 3.0f);
    }
    ++m_initCounter;
}

// ── update ────────────────────────────────────────────────────────────────────
void NPCBehaviorSystem::update(entt::registry& reg, float dt,
                               AbilitySystem& abilities) {
    auto view = reg.view<WaveMinionComponent, TransformComponent>();

    for (auto [entity, minion, transform] : view.each()) {
        // Lazy ability init — runs once per entity on first encounter
        if (!reg.all_of<AbilityBookComponent>(entity)) {
            lazyInitAbilities(reg, entity, abilities, minion.abilitySetId);
            // Skip the decision this tick; the ability is on its stagger cooldown
            continue;
        }

        // Tick per-entity decision cooldown
        if (minion.decisionCooldown > 0.0f) {
            minion.decisionCooldown -= dt;
            continue;
        }
        minion.decisionCooldown = DECISION_INTERVAL;

        // Resolve aggro target (prefer hero-aggro override when active)
        entt::entity target = (minion.heroAggroTimer > 0.0f && reg.valid(minion.heroAggroTarget))
                              ? minion.heroAggroTarget
                              : minion.aggroTarget;

        if (!reg.valid(target)) continue;

        // Q ability readiness check
        const auto& book = reg.get<AbilityBookComponent>(entity);
        const auto& qInst = book.abilities[static_cast<size_t>(AbilitySlot::Q)];
        if (!qInst.isReady()) continue;
        if (!qInst.def)       continue;

        // Compute direction to target
        const auto* targetTc = reg.try_get<TransformComponent>(target);
        if (!targetTc) continue;

        glm::vec3 delta = targetTc->position - transform.position;
        float dist = glm::length(delta);
        if (dist < 0.001f) continue;

        // Only fire if target is within the ability's declared cast range
        if (dist > qInst.def->castRange) continue;

        glm::vec3 dir = delta / dist;

        TargetInfo ti{};
        ti.type           = TargetingType::SKILLSHOT;
        ti.targetEntity   = static_cast<EntityID>(entt::to_integral(target));
        ti.targetPosition = targetTc->position;
        ti.direction      = dir;

        abilities.enqueueRequest(entity, AbilitySlot::Q, ti);
    }
}

} // namespace glory

#pragma once

// ── NPCBehaviorSystem ─────────────────────────────────────────────────────────
// Drives ability usage for wave minions.
//
// Each entity with WaveMinionComponent gets an AbilityBookComponent on first
// encounter (lazy init).  Every DECISION_INTERVAL seconds it checks whether
// the entity has a live aggroTarget in range and, if so, fires the ability.
//
// Initial cooldowns are staggered per-entity so an entire wave does not burst
// its abilities on the same tick.

#include <entt.hpp>

namespace glory {

class AbilitySystem;

class NPCBehaviorSystem {
public:
    // Interval between ability decisions (seconds, simulation-tick granular)
    static constexpr float DECISION_INTERVAL = 1.5f;

    // Tick all entities with WaveMinionComponent.
    // Must be called every simulation tick after MinionWaveUpdateSystem so that
    // aggroTarget is already resolved for this tick.
    void update(entt::registry& reg, float dt, AbilitySystem& abilities);

private:
    // Ensure the entity has an AbilityBookComponent with its Q ability
    // initialized and levelled to 1.  Stagger the starting cooldown.
    void lazyInitAbilities(entt::registry& reg, entt::entity e,
                           AbilitySystem& abilities, uint8_t abilitySetId);

    // Per-entity stagger counter — incremented each time lazyInit is called
    // so successive minions get progressively offset starting cooldowns.
    uint32_t m_initCounter = 0;
};

} // namespace glory

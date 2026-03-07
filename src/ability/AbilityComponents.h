#pragma once

// ── Ability System: ECS Components ────────────────────────────────────────
// Components attached to character entities via entt registry.

#include "ability/AbilityTypes.h"
#include "vfx/VFXTypes.h"      // VFXEvent

#include <entt.hpp>
#include <array>
#include <vector>

namespace glory {

// ── TargetInfo ─────────────────────────────────────────────────────────────
// Captures targeting data at the moment the ability key is pressed.
struct TargetInfo {
    TargetingType type           = TargetingType::NONE;
    EntityID      targetEntity   = NULL_ENTITY;
    glm::vec3     targetPosition {0.f};
    glm::vec3     direction      {0.f, 0.f, 1.f};  // for skillshots
};

// ── AbilityInstance ────────────────────────────────────────────────────────
// Mutable runtime state for a single ability slot.
struct AbilityInstance {
    const AbilityDefinition* def              = nullptr;
    int                      level            = 0;       // 0 = not learned
    float                    cooldownRemaining = 0.0f;
    AbilityPhase             currentPhase     = AbilityPhase::READY;
    float                    phaseTimer       = 0.0f;    // counts down within phase
    TargetInfo               currentTarget;
    uint32_t                 activeVFXHandle  = INVALID_VFX_HANDLE;

    bool isReady() const {
        return level > 0 && currentPhase == AbilityPhase::READY;
    }
};

// ── AbilityBookComponent ──────────────────────────────────────────────────
// Attached to every character entity.
struct AbilityBookComponent {
    std::array<AbilityInstance,
               static_cast<size_t>(AbilitySlot::COUNT)> abilities{};
};

// ── ActiveStatusEffect ─────────────────────────────────────────────────────
struct ActiveStatusEffect {
    const EffectDef* def              = nullptr;
    EntityID         sourceEntity     = NULL_ENTITY;
    float            remainingDuration = 0.0f;
    float            tickAccumulator  = 0.0f;
    float            totalValue       = 0.0f;     // pre-computed at apply time
    uint32_t         vfxHandle        = INVALID_VFX_HANDLE;
};

// ── StatusEffectsComponent ─────────────────────────────────────────────────
struct StatusEffectsComponent {
    std::vector<ActiveStatusEffect> activeEffects;

    // CC query helpers
    bool isStunned()   const { return hasCCType(EffectType::STUN);    }
    bool isSilenced()  const { return hasCCType(EffectType::SILENCE);  }
    bool isRooted()    const { return hasCCType(EffectType::ROOT);     }
    bool isSuppressed()const { return hasCCType(EffectType::SUPPRESS); }
    bool canCast()     const { return !isStunned() && !isSilenced() && !isSuppressed(); }
    bool canMove()     const { return !isStunned() && !isRooted()   && !isSuppressed(); }

private:
    bool hasCCType(EffectType t) const {
        for (const auto& e : activeEffects)
            if (e.def && e.def->type == t) return true;
        return false;
    }
};

// ── ProjectileComponent ────────────────────────────────────────────────────
// Attached to projectile entities spawned by AbilitySystem.
struct ProjectileComponent {
    const AbilityDefinition* sourceDef      = nullptr;
    EntityID                 casterEntity   = NULL_ENTITY;
    glm::vec3                velocity       {0.f};
    float                    maxRange       = 1100.0f;
    float                    traveledDist   = 0.0f;
    bool                     piercing       = false;
    int                      maxTargets     = 1;
    int                      hitCount       = 0;
    uint32_t                 vfxHandle      = INVALID_VFX_HANDLE;
};

// ── ResourceComponent ─────────────────────────────────────────────────────
struct ResourceComponent {
    ResourceType type        = ResourceType::MANA;
    float        current     = 500.0f;
    float        maximum     = 500.0f;
    float        regenPerSec = 8.0f;

    bool spend(float amount) {
        if (amount > current) return false;
        current -= amount;
        return true;
    }
};

// ── StatsComponent ────────────────────────────────────────────────────────
struct StatsComponent {
    Stats base;
    Stats bonus;   // from items / buffs

    Stats total() const {
        Stats t;
        t.attackDamage      = base.attackDamage      + bonus.attackDamage;
        t.abilityPower       = base.abilityPower       + bonus.abilityPower;
        t.armor              = base.armor              + bonus.armor;
        t.magicResist        = base.magicResist        + bonus.magicResist;
        t.maxHP              = base.maxHP              + bonus.maxHP;
        t.currentHP          = base.currentHP;         // don't add bonus
        t.cooldownReduction  = std::min(0.45f,
                               base.cooldownReduction + bonus.cooldownReduction);
        return t;
    }
};

// ── AbilityRequest ─────────────────────────────────────────────────────────
// Pushed to the AbilitySystem queue when a player presses an ability key.
struct AbilityRequest {
    entt::entity  casterEntity;
    AbilitySlot   slot;
    TargetInfo    target;
};

} // namespace glory

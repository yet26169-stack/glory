#pragma once

#include "ability/AbilityDef.h"
#include "ability/AbilityTypes.h"

#include <entt.hpp>
#include <glm/glm.hpp>

#include <array>
#include <vector>

namespace glory {

// ── Entity alias ────────────────────────────────────────────────────────────
using EntityID = entt::entity;

// ── Target information for ability casts ────────────────────────────────────
struct TargetInfo {
  TargetingType type = TargetingType::NONE;
  EntityID targetEntity = entt::null;    // for point-and-click
  glm::vec3 targetPosition{0.0f};        // for ground-targeted / skillshots
  glm::vec3 direction{0.0f, 0.0f, 1.0f}; // for skillshots / vector targeting
};

// ── Per-ability runtime state ───────────────────────────────────────────────
struct AbilityInstance {
  const AbilityDefinition *def = nullptr;
  int level = 0; // 0 = not learned
  float cooldownRemaining = 0.0f;
  AbilityPhase currentPhase = AbilityPhase::READY;
  float phaseTimer = 0.0f;
  TargetInfo currentTarget;
};

// ── Ability book (attached to every champion entity) ────────────────────────
struct AbilityBookComponent {
  std::array<AbilityInstance, static_cast<size_t>(AbilitySlot::COUNT)>
      abilities;

  AbilityInstance &get(AbilitySlot slot) {
    return abilities[static_cast<size_t>(slot)];
  }
  const AbilityInstance &get(AbilitySlot slot) const {
    return abilities[static_cast<size_t>(slot)];
  }
};

// ── Active status effect instance ───────────────────────────────────────────
struct ActiveStatusEffect {
  const EffectDef *def = nullptr;
  EntityID sourceEntity = entt::null;
  float remainingDuration = 0.0f;
  float tickAccumulator = 0.0f;
  float totalValue = 0.0f; // pre-computed at apply time
};

// ── Status effects container ────────────────────────────────────────────────
struct StatusEffectsComponent {
  std::vector<ActiveStatusEffect> activeEffects;

  bool hasCC() const {
    for (const auto &e : activeEffects) {
      if (e.def && preventsCasting(e.def->type))
        return true;
    }
    return false;
  }

  bool hasHardCC() const {
    for (const auto &e : activeEffects) {
      if (!e.def)
        continue;
      auto t = e.def->type;
      if (t == EffectType::STUN || t == EffectType::SUPPRESS ||
          t == EffectType::KNOCKUP || t == EffectType::KNOCKBACK)
        return true;
    }
    return false;
  }

  bool isRooted() const {
    for (const auto &e : activeEffects) {
      if (e.def && preventsMovement(e.def->type))
        return true;
    }
    return false;
  }

  bool isSilenced() const {
    for (const auto &e : activeEffects) {
      if (e.def && preventsCasting(e.def->type))
        return true;
    }
    return false;
  }
};

// ── Combat stats for damage/healing calculations ────────────────────────────
struct CombatStatsComponent {
  // Health
  float maxHP = 1000.0f;
  float currentHP = 1000.0f;
  float hpRegen = 1.0f; // per second

  // Resource
  ResourceType resourceType = ResourceType::MANA;
  float maxResource = 500.0f;
  float currentResource = 500.0f;
  float resourceRegen = 2.0f; // per second

  // Offensive
  float attackDamage = 60.0f;
  float abilityPower = 0.0f;
  float attackSpeed = 1.0f; // attacks per second

  // Defensive
  float armor = 30.0f;
  float magicResist = 30.0f;

  // Penetration
  float armorPenFlat = 0.0f;
  float armorPenPercent = 0.0f;
  float magicPenFlat = 0.0f;
  float magicPenPercent = 0.0f;

  // Utility
  float moveSpeed = 325.0f;
  float cdr = 0.0f;      // 0.0 - 0.4 (40% cap)
  float tenacity = 0.0f; // 0.0 - 1.0

  // Sustain
  float lifeSteal = 0.0f;
  float spellVamp = 0.0f;

  // Shield
  float shield = 0.0f;

  // Level
  int level = 1;

  // ── Helpers ─────────────────────────────────────────────────────────
  bool isAlive() const { return currentHP > 0.0f; }

  float getResource() const { return currentResource; }

  bool hasResource(float cost) const { return currentResource >= cost; }

  void deductResource(float cost) {
    currentResource = std::max(0.0f, currentResource - cost);
  }

  void takeDamage(float amount) {
    // Deplete shield first
    if (shield > 0.0f) {
      if (amount <= shield) {
        shield -= amount;
        return;
      }
      amount -= shield;
      shield = 0.0f;
    }
    currentHP = std::max(0.0f, currentHP - amount);
  }

  void heal(float amount) { currentHP = std::min(maxHP, currentHP + amount); }

  void addShield(float amount) { shield += amount; }

  void regenTick(float dt) {
    currentHP = std::min(maxHP, currentHP + hpRegen * dt);
    currentResource =
        std::min(maxResource, currentResource + resourceRegen * dt);
  }
};

// ── Pending effect (queued by AbilitySystem, processed by EffectSystem) ─────
struct PendingEffect {
  EntityID source = entt::null;
  EntityID target = entt::null;
  const EffectDef *def = nullptr;
  int abilityLevel = 1;
};

// ── Global effect queue component (singleton-style on a dedicated entity) ───
struct EffectQueueComponent {
  std::vector<PendingEffect> pending;

  void enqueue(EntityID source, EntityID target, const EffectDef *def,
               int level) {
    pending.push_back({source, target, def, level});
  }

  void clear() { pending.clear(); }
};

// ── Ability cast request (queued by input, processed by AbilitySystem) ──────
struct AbilityCastRequest {
  EntityID caster = entt::null;
  AbilitySlot slot = AbilitySlot::Q;
  TargetInfo target;
};

struct AbilityInputComponent {
  std::vector<AbilityCastRequest> requests;

  void requestCast(EntityID caster, AbilitySlot slot,
                   const TargetInfo &target = {}) {
    requests.push_back({caster, slot, target});
  }

  void clear() { requests.clear(); }
};

} // namespace glory

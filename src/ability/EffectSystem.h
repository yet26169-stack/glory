#pragma once

#include "ability/AbilityTypes.h"

#include <entt.hpp>

namespace glory {

// ── EffectSystem ────────────────────────────────────────────────────────────
// Processes pending effects (damage, healing, CC, buffs) from the queue.
// Must run AFTER AbilitySystem and ProjectileSystem in the update order.
class EffectSystem {
public:
  void apply(entt::registry &registry);
  void update(entt::registry &registry, float dt);

  // Standalone damage calculation (used by the system and exposed for tests)
  static float CalculateDamage(float rawDamage, DamageType type,
                               float resistance, float flatPen,
                               float percentPen);
};

} // namespace glory

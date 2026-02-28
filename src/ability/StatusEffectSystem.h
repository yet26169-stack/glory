#pragma once

#include <entt.hpp>

namespace glory {

// ── StatusEffectSystem ──────────────────────────────────────────────────────
// Ticks active buffs/debuffs/DoTs/HoTs each frame.
// Removes expired effects. Must run BEFORE AbilitySystem in update order.
class StatusEffectSystem {
public:
  void update(entt::registry &registry, float dt);
};

} // namespace glory

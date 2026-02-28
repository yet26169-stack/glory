#pragma once

#include <entt.hpp>

namespace glory {

// ── CooldownSystem ──────────────────────────────────────────────────────────
// Decrements ability cooldown timers each frame.
// Transitions ON_COOLDOWN → READY when cooldown expires.
// Must run BEFORE AbilitySystem in the update order.
class CooldownSystem {
public:
  void update(entt::registry &registry, float dt);
};

} // namespace glory

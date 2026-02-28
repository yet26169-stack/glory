#pragma once

#include "ability/AbilityComponents.h"

#include <entt.hpp>

namespace glory {

// ── AbilitySystem ───────────────────────────────────────────────────────────
// Processes ability cast requests through the state machine.
// Must run AFTER CooldownSystem and StatusEffectSystem in the update order.
class AbilitySystem {
public:
  // Process all queued cast requests and advance active ability states.
  void update(entt::registry &registry, float dt);

  // Queue a cast request (called from input handling).
  static void requestCast(entt::registry &registry, entt::entity caster,
                          AbilitySlot slot, const TargetInfo &target = {});

private:
  // State machine transitions
  void processRequests(entt::registry &registry);
  void advancePhases(entt::registry &registry, float dt);

  // Pre-cast validation
  bool validateCast(const entt::registry &registry, entt::entity caster,
                    const AbilityInstance &ability) const;
};

} // namespace glory

#include "ability/StatusEffectSystem.h"
#include "ability/AbilityComponents.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace glory {

void StatusEffectSystem::update(entt::registry &registry, float dt) {
  auto view = registry.view<StatusEffectsComponent>();

  for (auto entity : view) {
    auto &statusComp = view.get<StatusEffectsComponent>(entity);

    auto it = statusComp.activeEffects.begin();
    while (it != statusComp.activeEffects.end()) {
      it->remainingDuration -= dt;

      // Tick DoTs/HoTs
      if (it->def && it->def->tickRate > 0.0f) {
        it->tickAccumulator += dt;
        while (it->tickAccumulator >= it->def->tickRate) {
          it->tickAccumulator -= it->def->tickRate;

          // Apply tick damage or healing
          if (registry.all_of<CombatStatsComponent>(entity)) {
            auto &stats = registry.get<CombatStatsComponent>(entity);

            if (it->def->type == EffectType::DOT) {
              // Damage per tick = totalValue / (duration / tickRate)
              float totalTicks = it->def->duration / it->def->tickRate;
              float tickDmg = it->totalValue / std::max(1.0f, totalTicks);
              stats.takeDamage(tickDmg);
              spdlog::debug("DoT tick: {:.1f} damage", tickDmg);
            } else if (it->def->type == EffectType::HOT) {
              float totalTicks = it->def->duration / it->def->tickRate;
              float tickHeal = it->totalValue / std::max(1.0f, totalTicks);
              stats.heal(tickHeal);
              spdlog::debug("HoT tick: {:.1f} healing", tickHeal);
            }
          }
        }
      }

      // Remove expired effects
      if (it->remainingDuration <= 0.0f) {
        spdlog::debug(
            "Status effect expired (type {})",
            static_cast<int>(it->def ? it->def->type : EffectType::DAMAGE));
        it = statusComp.activeEffects.erase(it);
      } else {
        ++it;
      }
    }
  }
}

} // namespace glory

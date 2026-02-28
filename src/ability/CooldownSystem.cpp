#include "ability/CooldownSystem.h"
#include "ability/AbilityComponents.h"

namespace glory {

void CooldownSystem::update(entt::registry &registry, float dt) {
  auto view = registry.view<AbilityBookComponent>();

  for (auto entity : view) {
    auto &book = view.get<AbilityBookComponent>(entity);

    for (auto &ability : book.abilities) {
      if (!ability.def || ability.level == 0)
        continue;

      if (ability.currentPhase == AbilityPhase::ON_COOLDOWN) {
        ability.cooldownRemaining -= dt;

        if (ability.cooldownRemaining <= 0.0f) {
          ability.cooldownRemaining = 0.0f;
          ability.currentPhase = AbilityPhase::READY;
        }
      }
    }
  }
}

} // namespace glory

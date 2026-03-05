#include "combat/AutoAttackSystem.h"
#include "minion/MinionComponents.h"
#include "scene/Components.h"

namespace glory {

void AutoAttackSystem::update(entt::registry &registry,
                              MinionSystem &minionSystem, float dt) {
  auto playerView = registry.view<TransformComponent, CharacterComponent,
                                   AutoAttackComponent, PlayerTargetComponent>();
  for (auto player : playerView) {
    auto &transform = playerView.get<TransformComponent>(player);
    auto &character = playerView.get<CharacterComponent>(player);
    auto &attack = playerView.get<AutoAttackComponent>(player);
    auto &target = playerView.get<PlayerTargetComponent>(player);

    attack.timeSinceLastAttack += dt;

    // Validate current target
    if (target.targetEntity != entt::null) {
      if (!registry.valid(target.targetEntity) ||
          !registry.all_of<MinionHealthComponent>(target.targetEntity)) {
        target.targetEntity = entt::null;
        attack.isAttacking = false;
      } else {
        auto &targetHp =
            registry.get<MinionHealthComponent>(target.targetEntity);
        if (targetHp.isDead) {
          target.targetEntity = entt::null;
          attack.isAttacking = false;
        }
      }
    }

    // Auto-acquire nearest enemy if no target and player is not actively
    // moving (explicit right-click movement takes priority over auto-attack).
    if (target.targetEntity == entt::null && !character.hasTarget) {
      float bestDist = attack.attackRange * 1.5f;
      entt::entity bestTarget = entt::null;

      auto minionView = registry.view<MinionTag, MinionHealthComponent,
                                       TransformComponent>();
      for (auto minion : minionView) {
        auto &minionHp = minionView.get<MinionHealthComponent>(minion);
        if (minionHp.isDead)
          continue;
        auto &minionTransform = minionView.get<TransformComponent>(minion);
        float dist = glm::length(minionTransform.position - transform.position);
        if (dist < bestDist) {
          bestDist = dist;
          bestTarget = minion;
        }
      }
      target.targetEntity = bestTarget;
    }

    if (target.targetEntity == entt::null) {
      attack.isAttacking = false;
      continue;
    }

    // We have a valid target — get its position
    auto &targetTransform =
        registry.get<TransformComponent>(target.targetEntity);
    float dist = glm::length(targetTransform.position - transform.position);

    if (dist > attack.attackRange) {
      // Walk toward target
      character.targetPosition = targetTransform.position;
      character.hasTarget = true;
      attack.isAttacking = false;
    } else {
      // In range — stop movement and attack
      character.hasTarget = false;
      attack.isAttacking = true;

      if (attack.timeSinceLastAttack >= attack.attackCooldown) {
        // Deal damage: damage * 100 / (100 + armor)
        auto &targetHp =
            registry.get<MinionHealthComponent>(target.targetEntity);
        float armor = 0.0f;
        if (registry.all_of<MinionCombatComponent>(target.targetEntity)) {
          armor = registry.get<MinionCombatComponent>(target.targetEntity).armor;
        }
        float effectiveDamage =
            attack.attackDamage * 100.0f / (100.0f + armor);
        targetHp.currentHP -= effectiveDamage;
        targetHp.lastAttacker = player;

        if (targetHp.currentHP <= 0.0f) {
          targetHp.currentHP = 0.0f;
          targetHp.isDead = true;
          target.targetEntity = entt::null;
          attack.isAttacking = false;
        }

        // Trigger minion aggro draw
        minionSystem.notifyChampionAttack(player, target.targetEntity,
                                          targetTransform.position);

        attack.timeSinceLastAttack = 0.0f;
      }
    }
  }
}

} // namespace glory

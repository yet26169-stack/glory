#include "ability/AbilitySystem.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace glory {

// ── Public API ──────────────────────────────────────────────────────────────

void AbilitySystem::update(entt::registry &registry, float dt) {
  processRequests(registry);
  advancePhases(registry, dt);
}

void AbilitySystem::requestCast(entt::registry &registry, entt::entity caster,
                                AbilitySlot slot, const TargetInfo &target) {
  // Ensure the entity has an input component
  if (!registry.all_of<AbilityInputComponent>(caster)) {
    registry.emplace<AbilityInputComponent>(caster);
  }
  auto &input = registry.get<AbilityInputComponent>(caster);
  input.requestCast(caster, slot, target);
}

// ── Process queued cast requests ────────────────────────────────────────────

void AbilitySystem::processRequests(entt::registry &registry) {
  auto view = registry.view<AbilityInputComponent, AbilityBookComponent>();

  for (auto entity : view) {
    auto &input = view.get<AbilityInputComponent>(entity);
    auto &book = view.get<AbilityBookComponent>(entity);

    for (const auto &req : input.requests) {
      auto &ability = book.get(req.slot);

      if (!validateCast(registry, entity, ability)) {
        continue;
      }

      // Deduct resource cost
      if (ability.def->resourceType != ResourceType::NONE) {
        if (registry.all_of<CombatStatsComponent>(entity)) {
          auto &stats = registry.get<CombatStatsComponent>(entity);
          int idx = std::clamp(ability.level - 1, 0, 4);
          float cost = ability.def->costPerLevel[idx];
          stats.deductResource(cost);
        }
      }

      // Set target info
      ability.currentTarget = req.target;

      // Transition: READY → CASTING
      if (ability.def->castTime > 0.0f) {
        ability.currentPhase = AbilityPhase::CASTING;
        ability.phaseTimer = ability.def->castTime;
        spdlog::info(">>> Ability {} entering CASTING ({}s)", ability.def->id,
                     ability.def->castTime);
      } else {
        // Instant cast → skip to EXECUTING
        ability.currentPhase = AbilityPhase::EXECUTING;
        ability.phaseTimer = 0.0f;
        spdlog::info(">>> Ability {} instant-cast -> EXECUTING",
                     ability.def->id);
      }
    }

    input.clear();
  }
}

// ── Advance active ability phases ───────────────────────────────────────────

void AbilitySystem::advancePhases(entt::registry &registry, float dt) {
  auto view = registry.view<AbilityBookComponent>();

  for (auto entity : view) {
    auto &book = view.get<AbilityBookComponent>(entity);

    // Check if the entity has hard CC (for interrupt checks)
    bool hasHardCC = false;
    if (registry.all_of<StatusEffectsComponent>(entity)) {
      hasHardCC = registry.get<StatusEffectsComponent>(entity).hasHardCC();
    }

    for (auto &ability : book.abilities) {
      if (!ability.def || ability.level == 0)
        continue;

      switch (ability.currentPhase) {

      case AbilityPhase::CASTING:
        // Check for interrupt by hard CC
        if (hasHardCC && ability.def->canBeInterrupted) {
          ability.currentPhase = AbilityPhase::INTERRUPTED;
          spdlog::debug("Ability {} INTERRUPTED by hard CC", ability.def->id);
          break;
        }

        ability.phaseTimer -= dt;
        if (ability.phaseTimer <= 0.0f) {
          if (ability.def->channelDuration > 0.0f) {
            // CASTING → CHANNELING
            ability.currentPhase = AbilityPhase::CHANNELING;
            ability.phaseTimer = ability.def->channelDuration;
            spdlog::debug("Ability {} → CHANNELING ({}s)", ability.def->id,
                          ability.def->channelDuration);
          } else {
            // CASTING → EXECUTING
            ability.currentPhase = AbilityPhase::EXECUTING;
            ability.phaseTimer = 0.0f;
            spdlog::info(
                ">>> Ability {} -> EXECUTING at vec3({:.1f}, {:.1f}, {:.1f})",
                ability.def->id, ability.currentTarget.targetPosition.x,
                ability.currentTarget.targetPosition.y,
                ability.currentTarget.targetPosition.z);
          }
        }
        break;

      case AbilityPhase::CHANNELING:
        // Check for interrupt
        if (hasHardCC && ability.def->canBeInterrupted) {
          ability.currentPhase = AbilityPhase::INTERRUPTED;
          spdlog::debug("Ability {} channel INTERRUPTED", ability.def->id);
          break;
        }

        ability.phaseTimer -= dt;
        if (ability.phaseTimer <= 0.0f) {
          // CHANNELING → EXECUTING
          ability.currentPhase = AbilityPhase::EXECUTING;
          ability.phaseTimer = 0.0f;
          spdlog::info(">>> Ability {} channel complete -> EXECUTING",
                       ability.def->id);
        }
        break;

      case AbilityPhase::EXECUTING: {
        // Dispatch effects and enter cooldown
        // Queue on-hit effects for the EffectSystem to process
        if (!ability.def->onHitEffects.empty() ||
            !ability.def->onSelfEffects.empty()) {

          // Find or create the effect queue
          auto queueView = registry.view<EffectQueueComponent>();
          entt::entity queueEntity = entt::null;
          for (auto qe : queueView) {
            queueEntity = qe;
            break;
          }
          if (queueEntity == entt::null) {
            queueEntity = registry.create();
            registry.emplace<EffectQueueComponent>(queueEntity);
          }
          auto &queue = registry.get<EffectQueueComponent>(queueEntity);

          // Queue self-effects
          for (const auto &eff : ability.def->onSelfEffects) {
            queue.enqueue(entity, entity, &eff, ability.level);
          }

          // For non-projectile abilities, apply on-hit to target
          if (ability.def->targeting != TargetingType::SKILLSHOT) {
            EntityID target = ability.currentTarget.targetEntity;
            if (target != entt::null) {
              for (const auto &eff : ability.def->onHitEffects) {
                queue.enqueue(entity, target, &eff, ability.level);
              }
            }
          }
          // Skillshots are handled by ProjectileSystem (future phase)
        }

        // Transition: EXECUTING → ON_COOLDOWN
        int idx = std::clamp(ability.level - 1, 0, 4);
        float baseCooldown = ability.def->cooldownPerLevel[idx];
        float cdr = 0.0f;
        if (registry.all_of<CombatStatsComponent>(entity)) {
          cdr = registry.get<CombatStatsComponent>(entity).cdr;
        }
        ability.cooldownRemaining = baseCooldown * (1.0f - cdr);
        ability.currentPhase = AbilityPhase::ON_COOLDOWN;
        spdlog::info(">>> Ability {} -> ON_COOLDOWN ({}s)", ability.def->id,
                     ability.cooldownRemaining);
        break;
      }

      case AbilityPhase::INTERRUPTED: {
        // 75% of base cooldown
        int idx = std::clamp(ability.level - 1, 0, 4);
        float baseCooldown = ability.def->cooldownPerLevel[idx];
        float cdr = 0.0f;
        if (registry.all_of<CombatStatsComponent>(entity)) {
          cdr = registry.get<CombatStatsComponent>(entity).cdr;
        }
        ability.cooldownRemaining = baseCooldown * 0.75f * (1.0f - cdr);
        ability.currentPhase = AbilityPhase::ON_COOLDOWN;
        spdlog::debug("Ability {} interrupted → ON_COOLDOWN (75%: {}s)",
                      ability.def->id, ability.cooldownRemaining);
        break;
      }

      case AbilityPhase::ON_COOLDOWN:
        // Handled by CooldownSystem
        break;

      case AbilityPhase::READY:
        // Waiting for input
        break;
      }
    }
  }
}

// ── Pre-cast validation ─────────────────────────────────────────────────────

bool AbilitySystem::validateCast(const entt::registry &registry,
                                 entt::entity caster,
                                 const AbilityInstance &ability) const {
  if (!ability.def)
    return false;

  // Ability must be learned
  if (ability.level == 0) {
    spdlog::debug("Cast failed: ability not learned");
    return false;
  }

  // Must be READY
  if (ability.currentPhase != AbilityPhase::READY) {
    spdlog::debug("Cast failed: ability not ready (phase={})",
                  toString(ability.currentPhase));
    return false;
  }

  // Resource check
  if (ability.def->resourceType != ResourceType::NONE) {
    if (registry.all_of<CombatStatsComponent>(caster)) {
      const auto &stats = registry.get<CombatStatsComponent>(caster);
      int idx = std::clamp(ability.level - 1, 0, 4);
      float cost = ability.def->costPerLevel[idx];
      if (!stats.hasResource(cost)) {
        spdlog::debug("Cast failed: insufficient resource ({}/{})",
                      stats.getResource(), cost);
        return false;
      }
    }
  }

  // CC check — cannot cast if silenced/stunned/suppressed
  if (registry.all_of<StatusEffectsComponent>(caster)) {
    const auto &status = registry.get<StatusEffectsComponent>(caster);
    if (status.isSilenced()) {
      spdlog::debug("Cast failed: caster is CC'd");
      return false;
    }
  }

  return true;
}

} // namespace glory

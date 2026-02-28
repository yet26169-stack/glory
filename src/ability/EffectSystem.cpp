#include "ability/EffectSystem.h"
#include "ability/AbilityComponents.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace glory {

// ── Damage formula (ABILITIES.md section 6.2) ───────────────────────────────
float EffectSystem::CalculateDamage(float rawDamage, DamageType type,
                                    float resistance, float flatPen,
                                    float percentPen) {
  if (type == DamageType::TRUE_DMG)
    return rawDamage;

  float effectiveResist = resistance * (1.0f - percentPen) - flatPen;
  effectiveResist = std::max(0.0f, effectiveResist);

  float multiplier = 100.0f / (100.0f + effectiveResist);
  return rawDamage * multiplier;
}

// ── Process pending effects ─────────────────────────────────────────────────
void EffectSystem::apply(entt::registry &registry) {
  auto queueView = registry.view<EffectQueueComponent>();
  for (auto qe : queueView) {
    auto &queue = registry.get<EffectQueueComponent>(qe);

    for (const auto &pe : queue.pending) {
      if (!pe.def)
        continue;
      if (!registry.valid(pe.target))
        continue;

      const auto &effectDef = *pe.def;

      switch (effectDef.type) {

      case EffectType::DAMAGE:
      case EffectType::DOT: {
        if (!registry.all_of<CombatStatsComponent>(pe.target))
          break;
        auto &targetStats = registry.get<CombatStatsComponent>(pe.target);

        // Evaluate scaling
        float ad = 0.0f, ap = 0.0f, hp = 0.0f, armor = 0.0f, mr = 0.0f;
        if (registry.valid(pe.source) &&
            registry.all_of<CombatStatsComponent>(pe.source)) {
          const auto &srcStats = registry.get<CombatStatsComponent>(pe.source);
          ad = srcStats.attackDamage;
          ap = srcStats.abilityPower;
          hp = srcStats.currentHP;
          armor = srcStats.armor;
          mr = srcStats.magicResist;
        }

        float rawDmg =
            effectDef.scaling.evaluate(pe.abilityLevel, ad, ap, hp, armor, mr);

        // Get penetration from source
        float flatPen = 0.0f, percentPen = 0.0f;
        if (registry.valid(pe.source) &&
            registry.all_of<CombatStatsComponent>(pe.source)) {
          const auto &srcStats = registry.get<CombatStatsComponent>(pe.source);
          if (effectDef.damageType == DamageType::PHYSICAL) {
            flatPen = srcStats.armorPenFlat;
            percentPen = srcStats.armorPenPercent;
          } else if (effectDef.damageType == DamageType::MAGICAL) {
            flatPen = srcStats.magicPenFlat;
            percentPen = srcStats.magicPenPercent;
          }
        }

        float resistance = (effectDef.damageType == DamageType::PHYSICAL)
                               ? targetStats.armor
                               : targetStats.magicResist;

        float finalDmg = CalculateDamage(rawDmg, effectDef.damageType,
                                         resistance, flatPen, percentPen);
        targetStats.takeDamage(finalDmg);
        spdlog::debug("Applied {:.1f} damage (raw {:.1f})", finalDmg, rawDmg);

        // For DoTs, also add a status effect for ticking
        if (effectDef.type == EffectType::DOT && effectDef.duration > 0.0f) {
          if (!registry.all_of<StatusEffectsComponent>(pe.target))
            registry.emplace<StatusEffectsComponent>(pe.target);
          auto &status = registry.get<StatusEffectsComponent>(pe.target);
          status.activeEffects.push_back(
              {&effectDef, pe.source, effectDef.duration, 0.0f, rawDmg});
        }
        break;
      }

      case EffectType::HEAL:
      case EffectType::HOT: {
        if (!registry.all_of<CombatStatsComponent>(pe.target))
          break;
        auto &targetStats = registry.get<CombatStatsComponent>(pe.target);

        float ad = 0.0f, ap = 0.0f, hp = 0.0f, armor = 0.0f, mr = 0.0f;
        if (registry.valid(pe.source) &&
            registry.all_of<CombatStatsComponent>(pe.source)) {
          const auto &srcStats = registry.get<CombatStatsComponent>(pe.source);
          ad = srcStats.attackDamage;
          ap = srcStats.abilityPower;
          hp = srcStats.currentHP;
          armor = srcStats.armor;
          mr = srcStats.magicResist;
        }

        float healAmt =
            effectDef.scaling.evaluate(pe.abilityLevel, ad, ap, hp, armor, mr);

        if (effectDef.type == EffectType::HEAL) {
          targetStats.heal(healAmt);
          spdlog::debug("Healed for {:.1f}", healAmt);
        }

        if (effectDef.type == EffectType::HOT && effectDef.duration > 0.0f) {
          if (!registry.all_of<StatusEffectsComponent>(pe.target))
            registry.emplace<StatusEffectsComponent>(pe.target);
          auto &status = registry.get<StatusEffectsComponent>(pe.target);
          status.activeEffects.push_back(
              {&effectDef, pe.source, effectDef.duration, 0.0f, healAmt});
        }
        break;
      }

      case EffectType::SHIELD: {
        if (!registry.all_of<CombatStatsComponent>(pe.target))
          break;
        auto &targetStats = registry.get<CombatStatsComponent>(pe.target);

        float ad = 0.0f, ap = 0.0f, hp = 0.0f, armor = 0.0f, mr = 0.0f;
        if (registry.valid(pe.source) &&
            registry.all_of<CombatStatsComponent>(pe.source)) {
          const auto &srcStats = registry.get<CombatStatsComponent>(pe.source);
          ad = srcStats.attackDamage;
          ap = srcStats.abilityPower;
          hp = srcStats.currentHP;
          armor = srcStats.armor;
          mr = srcStats.magicResist;
        }

        float shieldAmt =
            effectDef.scaling.evaluate(pe.abilityLevel, ad, ap, hp, armor, mr);
        targetStats.addShield(shieldAmt);
        spdlog::debug("Shield for {:.1f}", shieldAmt);
        break;
      }

      // CC effects — add to status effects component
      case EffectType::STUN:
      case EffectType::SLOW:
      case EffectType::ROOT:
      case EffectType::KNOCKBACK:
      case EffectType::KNOCKUP:
      case EffectType::SILENCE:
      case EffectType::SUPPRESS:
      case EffectType::BLIND:
      case EffectType::CHARM:
      case EffectType::FEAR:
      case EffectType::TAUNT: {
        if (!registry.all_of<StatusEffectsComponent>(pe.target))
          registry.emplace<StatusEffectsComponent>(pe.target);
        auto &status = registry.get<StatusEffectsComponent>(pe.target);

        float duration = effectDef.duration;

        // Apply tenacity (reduces CC duration, except knockup/knockback)
        if (effectDef.type != EffectType::KNOCKUP &&
            effectDef.type != EffectType::KNOCKBACK &&
            effectDef.type != EffectType::SUPPRESS) {
          if (registry.all_of<CombatStatsComponent>(pe.target)) {
            float tenacity =
                registry.get<CombatStatsComponent>(pe.target).tenacity;
            duration *= (1.0f - tenacity);
          }
        }

        status.activeEffects.push_back(
            {&effectDef, pe.source, duration, 0.0f, effectDef.value});
        spdlog::debug("Applied CC {} for {:.1f}s",
                      static_cast<int>(effectDef.type), duration);
        break;
      }

      case EffectType::BUFF_STAT:
      case EffectType::DEBUFF_STAT: {
        if (!registry.all_of<StatusEffectsComponent>(pe.target))
          registry.emplace<StatusEffectsComponent>(pe.target);
        auto &status = registry.get<StatusEffectsComponent>(pe.target);
        status.activeEffects.push_back(
            {&effectDef, pe.source, effectDef.duration, 0.0f, effectDef.value});
        spdlog::debug("Applied stat mod for {:.1f}s", effectDef.duration);
        break;
      }

      case EffectType::DASH:
      case EffectType::BLINK:
        // Mobility effects — stub for now (requires physics integration)
        spdlog::debug("Mobility effect {} (stub)",
                      static_cast<int>(effectDef.type));
        break;
      }
    }

    queue.clear();
  }
}

} // namespace glory

#include "combat/EconomySystem.h"
#include "combat/CombatComponents.h"
#include "combat/HeroDefinition.h"
#include "ability/AbilityComponents.h"
#include "scene/Components.h"

#include <spdlog/spdlog.h>
#include <algorithm>

namespace glory {

// ── Kill rewards ─────────────────────────────────────────────────────────

void EconomySystem::awardKill(entt::registry& reg, entt::entity killer,
                               entt::entity victim) {
    if (!reg.valid(killer) || !reg.all_of<EconomyComponent>(killer)) return;
    auto& killerEcon = reg.get<EconomyComponent>(killer);

    int goldGain = 0;
    int xpGain   = 0;

    // Check if victim is a minion
    if (reg.valid(victim) && reg.all_of<MinionComponent>(victim)) {
        const auto& mc = reg.get<MinionComponent>(victim);
        goldGain = mc.goldReward;
        xpGain   = mc.xpReward;
        spdlog::debug("Economy: minion kill → +{}g +{}xp", goldGain, xpGain);
    }
    // Check if victim is a hero (champion kill)
    else if (reg.valid(victim) && reg.all_of<EconomyComponent>(victim)) {
        const auto& victimEcon = reg.get<EconomyComponent>(victim);
        goldGain = 300 + victimEcon.level * 20;
        xpGain   = 200 + victimEcon.level * 30;
        spdlog::info("Economy: hero kill (lvl {}) → +{}g +{}xp",
                     victimEcon.level, goldGain, xpGain);
    }

    killerEcon.gold += goldGain;
    killerEcon.xp   += xpGain;

    checkLevelUp(reg, killer);
}

// ── Passive income ───────────────────────────────────────────────────────

void EconomySystem::updatePassiveIncome(entt::registry& reg, float gameTime,
                                         float dt) {
    if (gameTime < PASSIVE_INCOME_START) return;

    m_passiveAccum += PASSIVE_GOLD_PER_SEC * dt;
    int wholeGold = static_cast<int>(m_passiveAccum);
    if (wholeGold <= 0) return;
    m_passiveAccum -= static_cast<float>(wholeGold);

    auto view = reg.view<EconomyComponent, HeroComponent>();
    for (auto [entity, econ, hero] : view.each()) {
        econ.gold += wholeGold;
    }
}

// ── Per-frame update ─────────────────────────────────────────────────────

void EconomySystem::update(entt::registry& reg, float gameTime, float dt) {
    updatePassiveIncome(reg, gameTime, dt);
}

// ── Level-up check ───────────────────────────────────────────────────────

void EconomySystem::checkLevelUp(entt::registry& reg, entt::entity entity) {
    if (!reg.all_of<EconomyComponent>(entity)) return;
    auto& econ = reg.get<EconomyComponent>(entity);

    while (econ.level < MAX_LEVEL) {
        int xpNeeded = XP_TABLE[static_cast<size_t>(econ.level)]; // threshold for NEXT level
        if (econ.xp < xpNeeded) break;

        int oldLevel = econ.level;
        econ.level++;
        spdlog::info("Economy: entity leveled up! {} → {}", oldLevel, econ.level);

        recalcStats(reg, entity, oldLevel, econ.level);
    }
}

// ── Stat recalculation on level-up ───────────────────────────────────────

void EconomySystem::recalcStats(entt::registry& reg, entt::entity entity,
                                 int oldLevel, int newLevel) {
    if (!reg.all_of<HeroComponent>(entity)) return;
    auto& hero = reg.get<HeroComponent>(entity);
    hero.level = newLevel;

    if (!hero.definition) return;
    const auto& def = *hero.definition;

    // Recalculate base stats from definition
    if (reg.all_of<StatsComponent>(entity)) {
        auto& stats = reg.get<StatsComponent>(entity);
        float oldMaxHP = stats.base.maxHP;

        stats.base.attackDamage = def.baseAttackDamage + def.damagePerLevel * (newLevel - 1);
        stats.base.armor        = def.baseArmor + def.armorPerLevel * (newLevel - 1);
        stats.base.magicResist  = def.baseMagicResist + def.magicResistPerLevel * (newLevel - 1);
        stats.base.maxHP        = def.baseHP + def.hpPerLevel * (newLevel - 1);

        // Heal the HP gained from leveling
        float hpGained = stats.base.maxHP - oldMaxHP;
        stats.base.currentHP = std::min(stats.base.maxHP,
                                         stats.base.currentHP + hpGained);
    }

    if (reg.all_of<ResourceComponent>(entity)) {
        auto& res = reg.get<ResourceComponent>(entity);
        float oldMaxMP = res.maximum;

        res.maximum = def.baseMP + def.mpPerLevel * (newLevel - 1);

        // Heal the MP gained from leveling
        float mpGained = res.maximum - oldMaxMP;
        res.current = std::min(res.maximum, res.current + mpGained);
    }

    // Update combat component attack damage to match
    if (reg.all_of<CombatComponent>(entity)) {
        auto& combat = reg.get<CombatComponent>(entity);
        combat.attackDamage = def.baseAttackDamage + def.damagePerLevel * (newLevel - 1);
    }
}

} // namespace glory

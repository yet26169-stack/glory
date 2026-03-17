#pragma once

// ── Economy System: Gold, XP, Leveling for MOBA ──────────────────────────
// EconomyComponent: per-entity gold/xp/level tracking
// MinionComponent:  tags minion type and reward values
// EconomySystem:    manages kill rewards, passive income, and level-ups

#include "combat/HeroDefinition.h"
#include <entt.hpp>
#include <array>
#include <cstdint>

namespace glory {

// ── Minion type and reward tagging ───────────────────────────────────────
enum class MinionType : uint8_t {
    MELEE,
    RANGED,
    CANNON,
};

struct MinionComponent {
    MinionType type       = MinionType::MELEE;
    int        goldReward = 20;
    int        xpReward   = 60;
};

// ── Per-entity economy state ─────────────────────────────────────────────
struct EconomyComponent {
    int   gold  = 500;  // starting gold
    int   xp    = 0;
    int   level = 1;    // 1-18
};

// ── Economy System ───────────────────────────────────────────────────────
class EconomySystem {
public:
    // Called when an attacker kills a target.  Awards gold/xp to the attacker.
    void awardKill(entt::registry& reg, entt::entity killer, entt::entity victim);

    // Passive gold income: +2 gold/sec after 90s of game time.
    void updatePassiveIncome(entt::registry& reg, float gameTime, float dt);

    // Per-frame tick (called from system scheduler).
    void update(entt::registry& reg, float gameTime, float dt);

private:
    // XP thresholds to reach level N (index 0 unused; index 1 = level 2, etc.)
    static constexpr int MAX_LEVEL = 18;
    static constexpr std::array<int, MAX_LEVEL> XP_TABLE = {{
        0,      // lvl 1 (instant)
        280,    // lvl 2
        660,    // lvl 3
        1140,   // lvl 4
        1720,   // lvl 5
        2400,   // lvl 6
        3180,   // lvl 7
        4060,   // lvl 8
        5040,   // lvl 9
        6120,   // lvl 10
        7300,   // lvl 11
        8580,   // lvl 12
        9960,   // lvl 13
        11440,  // lvl 14
        13020,  // lvl 15
        14700,  // lvl 16
        16480,  // lvl 17
        18360,  // lvl 18
    }};

    static constexpr float PASSIVE_INCOME_START = 90.0f;  // 1:30 game time
    static constexpr float PASSIVE_GOLD_PER_SEC = 2.0f;

    float m_passiveAccum = 0.0f;  // fractional gold accumulator

    void checkLevelUp(entt::registry& reg, entt::entity entity);
    void recalcStats(entt::registry& reg, entt::entity entity, int oldLevel, int newLevel);
};

} // namespace glory

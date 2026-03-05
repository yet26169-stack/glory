#pragma once

#include "minion/MinionTypes.h"

#include <array>
#include <string>

namespace glory {

// ── Per-type stats loaded from JSON ─────────────────────────────────────────

struct MinionStatsDef {
  float hp = 0.0f;
  float attackDamage = 0.0f;
  float armor = 0.0f;
  float magicResist = 0.0f;
  float moveSpeed = 325.0f;
  float attackRange = 0.0f;
  float attackCooldown = 1.0f;
  AttackStyle attackStyle = AttackStyle::Melee;
  float projectileSpeed = 0.0f;

  // Scaling per tick
  float hpPerTick = 0.0f;
  float adPerTick = 0.0f;
  float armorPerTick = 0.0f;
};

// ── Gold / XP reward definitions ────────────────────────────────────────────

struct MinionRewardDef {
  float goldBase = 0.0f;
  float goldPerMinute = 0.0f;
  float xp = 0.0f;
};

// ── Cannon wave rule ────────────────────────────────────────────────────────

struct CannonWaveRule {
  float minGameTime = 0.0f;    // seconds
  int everyNthWave = 3;
};

// ── Full minion config ──────────────────────────────────────────────────────

struct MinionConfig {
  std::array<MinionStatsDef, static_cast<size_t>(MinionType::Count)> stats;
  std::array<MinionRewardDef, static_cast<size_t>(MinionType::Count)> rewards;

  float scalingIntervalSeconds = 90.0f;

  // Wave config
  float firstWaveTime = 65.0f;
  float waveInterval = 30.0f;
  int standardMelee = 3;
  int standardCaster = 1;
  std::vector<CannonWaveRule> cannonRules;
  bool superReplaceSiege = true;
  int allInhibsDownSuperCount = 2;

  // Aggro config
  float aggroRange = 700.0f;
  float leashRange = 900.0f;
  float aggroCheckInterval = 0.25f;
  float targetReEvalInterval = 3.5f;
  float championAggroDuration = 2.5f;
  float championAggroCooldown = 2.0f;
  float xpRange = 1600.0f;
};

// ── Loader ──────────────────────────────────────────────────────────────────

class MinionConfigLoader {
public:
  /// Load all minion config files from the given directory.
  /// Expects: minion_stats.json, wave_config.json, aggro_config.json,
  ///          rewards_config.json
  static MinionConfig Load(const std::string &configDir);
};

} // namespace glory

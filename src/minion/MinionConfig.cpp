#include "minion/MinionConfig.h"

#include <fstream>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace glory {

static MinionStatsDef parseStats(const json &j) {
  MinionStatsDef s;
  s.hp = j.value("hp", 0.0f);
  s.attackDamage = j.value("attackDamage", 0.0f);
  s.armor = j.value("armor", 0.0f);
  s.magicResist = j.value("magicResist", 0.0f);
  s.moveSpeed = j.value("moveSpeed", 325.0f);
  s.attackRange = j.value("attackRange", 0.0f);
  s.attackCooldown = j.value("attackCooldown", 1.0f);
  s.projectileSpeed = j.value("projectileSpeed", 0.0f);

  std::string style = j.value("attackStyle", "melee");
  s.attackStyle = (style == "ranged") ? AttackStyle::Ranged : AttackStyle::Melee;

  if (j.contains("scaling")) {
    const auto &sc = j["scaling"];
    s.hpPerTick = sc.value("hpPerTick", 0.0f);
    s.adPerTick = sc.value("adPerTick", 0.0f);
    s.armorPerTick = sc.value("armorPerTick", 0.0f);
  }
  return s;
}

static MinionRewardDef parseReward(const json &goldJ, float xp) {
  MinionRewardDef r;
  r.goldBase = goldJ.value("base", 0.0f);
  r.goldPerMinute = goldJ.value("perMinute", 0.0f);
  r.xp = xp;
  return r;
}

MinionConfig MinionConfigLoader::Load(const std::string &configDir) {
  MinionConfig cfg;

  auto readFile = [](const std::string &path) -> json {
    std::ifstream f(path);
    if (!f.is_open()) {
      spdlog::warn("MinionConfig: cannot open '{}'", path);
      return json{};
    }
    return json::parse(f, nullptr, false);
  };

  // ── minion_stats.json ───────────────────────────────────────────────────
  {
    auto j = readFile(configDir + "minion_stats.json");
    if (j.contains("minionTypes")) {
      const auto &types = j["minionTypes"];
      const char *names[] = {"Melee", "Caster", "Siege", "Super"};
      for (int i = 0; i < 4; ++i) {
        if (types.contains(names[i])) {
          cfg.stats[i] = parseStats(types[names[i]]);
        }
      }
    }
    cfg.scalingIntervalSeconds = j.value("scalingIntervalSeconds", 90.0f);
  }

  // ── wave_config.json ────────────────────────────────────────────────────
  {
    auto j = readFile(configDir + "wave_config.json");
    cfg.firstWaveTime = j.value("firstWaveTime", 65.0f);
    cfg.waveInterval = j.value("waveInterval", 30.0f);

    if (j.contains("standardWave")) {
      cfg.standardMelee = j["standardWave"].value("melee", 3);
      cfg.standardCaster = j["standardWave"].value("caster", 1);
    }

    if (j.contains("cannonWaveRules")) {
      for (const auto &rule : j["cannonWaveRules"]) {
        CannonWaveRule r;
        r.minGameTime = rule.value("minGameTime", 0.0f);
        r.everyNthWave = rule.value("everyNthWave", 3);
        cfg.cannonRules.push_back(r);
      }
    }

    cfg.superReplaceSiege = j.value("superMinionReplaceSiege", true);
    cfg.allInhibsDownSuperCount = j.value("allInhibsDownSuperCount", 2);
  }

  // ── aggro_config.json ───────────────────────────────────────────────────
  {
    auto j = readFile(configDir + "aggro_config.json");
    cfg.aggroRange = j.value("aggroRange", 700.0f);
    cfg.leashRange = j.value("leashRange", 900.0f);
    cfg.aggroCheckInterval = j.value("aggroCheckInterval", 0.25f);
    cfg.targetReEvalInterval = j.value("targetReEvalInterval", 3.5f);
    cfg.championAggroDuration = j.value("championAggroDuration", 2.5f);
    cfg.championAggroCooldown = j.value("championAggroCooldown", 2.0f);
    cfg.xpRange = j.value("xpRange", 1600.0f);
  }

  // ── rewards_config.json ─────────────────────────────────────────────────
  {
    auto j = readFile(configDir + "rewards_config.json");
    const char *names[] = {"Melee", "Caster", "Siege", "Super"};
    if (j.contains("goldRewards") && j.contains("xpRewards")) {
      for (int i = 0; i < 4; ++i) {
        float xp = j["xpRewards"].value(names[i], 0.0f);
        if (j["goldRewards"].contains(names[i])) {
          cfg.rewards[i] = parseReward(j["goldRewards"][names[i]], xp);
        }
      }
    }
  }

  spdlog::info("MinionConfig loaded from '{}'", configDir);
  return cfg;
}

} // namespace glory

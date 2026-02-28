#include "ability/AbilityDef.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace glory {

// ── ScalingFormula evaluation ───────────────────────────────────────────────
float ScalingFormula::evaluate(int level, float ad, float ap, float hpValue,
                               float armor, float mr) const {
  int idx = std::clamp(level - 1, 0, 4);
  float total = basePerLevel[idx];
  total += ad * adRatio;
  total += ap * apRatio;
  total += hpValue * hpRatio;
  total += armor * armorRatio;
  total += mr * mrRatio;
  return total;
}

// ── JSON enum parsing helpers ───────────────────────────────────────────────

static AbilitySlot parseSlot(const std::string &s) {
  if (s == "Q")
    return AbilitySlot::Q;
  if (s == "W")
    return AbilitySlot::W;
  if (s == "E")
    return AbilitySlot::E;
  if (s == "R")
    return AbilitySlot::R;
  if (s == "PASSIVE")
    return AbilitySlot::PASSIVE;
  if (s == "SUMMONER")
    return AbilitySlot::SUMMONER;
  spdlog::warn("Unknown ability slot '{}', defaulting to Q", s);
  return AbilitySlot::Q;
}

static TargetingType parseTargeting(const std::string &s) {
  if (s == "SKILLSHOT")
    return TargetingType::SKILLSHOT;
  if (s == "POINT")
    return TargetingType::POINT;
  if (s == "SELF")
    return TargetingType::SELF;
  if (s == "TARGETED")
    return TargetingType::TARGETED;
  if (s == "VECTOR")
    return TargetingType::VECTOR;
  if (s == "NONE")
    return TargetingType::NONE;
  spdlog::warn("Unknown targeting type '{}', defaulting to NONE", s);
  return TargetingType::NONE;
}

static ResourceType parseResource(const std::string &s) {
  if (s == "MANA")
    return ResourceType::MANA;
  if (s == "ENERGY")
    return ResourceType::ENERGY;
  if (s == "HEALTH")
    return ResourceType::HEALTH;
  if (s == "RAGE")
    return ResourceType::RAGE;
  if (s == "NONE")
    return ResourceType::NONE;
  return ResourceType::NONE;
}

static AreaShape parseAreaShape(const std::string &s) {
  if (s == "CIRCLE")
    return AreaShape::CIRCLE;
  if (s == "CONE")
    return AreaShape::CONE;
  if (s == "RECTANGLE")
    return AreaShape::RECTANGLE;
  if (s == "LINE")
    return AreaShape::LINE;
  if (s == "RING")
    return AreaShape::RING;
  return AreaShape::NONE;
}

static DamageType parseDamageType(const std::string &s) {
  if (s == "PHYSICAL")
    return DamageType::PHYSICAL;
  if (s == "MAGICAL")
    return DamageType::MAGICAL;
  if (s == "TRUE")
    return DamageType::TRUE_DMG;
  return DamageType::PHYSICAL;
}

static EffectType parseEffectType(const std::string &s) {
  if (s == "DAMAGE")
    return EffectType::DAMAGE;
  if (s == "HEAL")
    return EffectType::HEAL;
  if (s == "SHIELD")
    return EffectType::SHIELD;
  if (s == "STUN")
    return EffectType::STUN;
  if (s == "SLOW")
    return EffectType::SLOW;
  if (s == "ROOT")
    return EffectType::ROOT;
  if (s == "KNOCKBACK")
    return EffectType::KNOCKBACK;
  if (s == "KNOCKUP")
    return EffectType::KNOCKUP;
  if (s == "SILENCE")
    return EffectType::SILENCE;
  if (s == "SUPPRESS")
    return EffectType::SUPPRESS;
  if (s == "BLIND")
    return EffectType::BLIND;
  if (s == "CHARM")
    return EffectType::CHARM;
  if (s == "FEAR")
    return EffectType::FEAR;
  if (s == "TAUNT")
    return EffectType::TAUNT;
  if (s == "BUFF_STAT")
    return EffectType::BUFF_STAT;
  if (s == "DEBUFF_STAT")
    return EffectType::DEBUFF_STAT;
  if (s == "DOT")
    return EffectType::DOT;
  if (s == "HOT")
    return EffectType::HOT;
  if (s == "DASH")
    return EffectType::DASH;
  if (s == "BLINK")
    return EffectType::BLINK;
  spdlog::warn("Unknown effect type '{}', defaulting to DAMAGE", s);
  return EffectType::DAMAGE;
}

static HPScalingBasis parseHPBasis(const std::string &s) {
  if (s == "MAX")
    return HPScalingBasis::MAX;
  if (s == "BONUS")
    return HPScalingBasis::BONUS;
  if (s == "CURRENT")
    return HPScalingBasis::CURRENT;
  if (s == "MISSING")
    return HPScalingBasis::MISSING;
  return HPScalingBasis::MAX;
}

static StatType parseStatType(const std::string &s) {
  if (s == "AD")
    return StatType::AD;
  if (s == "AP")
    return StatType::AP;
  if (s == "ARMOR")
    return StatType::ARMOR;
  if (s == "MAGIC_RESIST")
    return StatType::MAGIC_RESIST;
  if (s == "MOVE_SPEED")
    return StatType::MOVE_SPEED;
  if (s == "ATTACK_SPEED")
    return StatType::ATTACK_SPEED;
  if (s == "MAX_HP")
    return StatType::MAX_HP;
  if (s == "MAX_MANA")
    return StatType::MAX_MANA;
  if (s == "CDR")
    return StatType::CDR;
  if (s == "ARMOR_PEN_FLAT")
    return StatType::ARMOR_PEN_FLAT;
  if (s == "ARMOR_PEN_PERCENT")
    return StatType::ARMOR_PEN_PERCENT;
  if (s == "MAGIC_PEN_FLAT")
    return StatType::MAGIC_PEN_FLAT;
  if (s == "MAGIC_PEN_PERCENT")
    return StatType::MAGIC_PEN_PERCENT;
  if (s == "LIFE_STEAL")
    return StatType::LIFE_STEAL;
  if (s == "SPELL_VAMP")
    return StatType::SPELL_VAMP;
  return StatType::AD;
}

// ── JSON → struct parsing ───────────────────────────────────────────────────

static std::array<float, 5> parseFloatArray5(const nlohmann::json &j) {
  std::array<float, 5> arr{};
  if (j.is_array()) {
    for (size_t i = 0; i < std::min(j.size(), size_t(5)); ++i)
      arr[i] = j[i].get<float>();
  }
  return arr;
}

static ScalingFormula parseScaling(const nlohmann::json &j) {
  ScalingFormula sf;
  if (j.contains("basePerLevel"))
    sf.basePerLevel = parseFloatArray5(j["basePerLevel"]);
  if (j.contains("adRatio"))
    sf.adRatio = j["adRatio"].get<float>();
  if (j.contains("apRatio"))
    sf.apRatio = j["apRatio"].get<float>();
  if (j.contains("hpRatio"))
    sf.hpRatio = j["hpRatio"].get<float>();
  if (j.contains("hpBasis"))
    sf.hpBasis = parseHPBasis(j["hpBasis"]);
  if (j.contains("armorRatio"))
    sf.armorRatio = j["armorRatio"].get<float>();
  if (j.contains("mrRatio"))
    sf.mrRatio = j["mrRatio"].get<float>();
  return sf;
}

static StatModifier parseStatMod(const nlohmann::json &j) {
  StatModifier sm;
  if (j.contains("stat"))
    sm.stat = parseStatType(j["stat"]);
  if (j.contains("amount"))
    sm.amount = j["amount"].get<float>();
  if (j.contains("percent"))
    sm.percent = j["percent"].get<bool>();
  return sm;
}

static EffectDef parseEffect(const nlohmann::json &j) {
  EffectDef ef;
  if (j.contains("type"))
    ef.type = parseEffectType(j["type"]);
  if (j.contains("damageType"))
    ef.damageType = parseDamageType(j["damageType"]);
  if (j.contains("scaling"))
    ef.scaling = parseScaling(j["scaling"]);
  if (j.contains("duration"))
    ef.duration = j["duration"].get<float>();
  if (j.contains("tickRate"))
    ef.tickRate = j["tickRate"].get<float>();
  if (j.contains("statMod"))
    ef.statMod = parseStatMod(j["statMod"]);
  if (j.contains("value"))
    ef.value = j["value"].get<float>();
  if (j.contains("appliesGrievousWounds"))
    ef.appliesGrievousWounds = j["appliesGrievousWounds"].get<bool>();
  if (j.contains("applyVFX"))
    ef.applyVFX = j["applyVFX"].get<std::string>();
  return ef;
}

static ProjectileDef parseProjectile(const nlohmann::json &j) {
  ProjectileDef pd;
  if (j.contains("speed"))
    pd.speed = j["speed"].get<float>();
  if (j.contains("width"))
    pd.width = j["width"].get<float>();
  if (j.contains("maxRange"))
    pd.maxRange = j["maxRange"].get<float>();
  if (j.contains("piercing"))
    pd.piercing = j["piercing"].get<bool>();
  if (j.contains("maxTargets"))
    pd.maxTargets = j["maxTargets"].get<int>();
  if (j.contains("returnsToSource"))
    pd.returnsToSource = j["returnsToSource"].get<bool>();
  if (j.contains("curveAngle"))
    pd.curveAngle = j["curveAngle"].get<float>();
  if (j.contains("destroyOnWall"))
    pd.destroyOnWall = j["destroyOnWall"].get<bool>();
  return pd;
}

// ── AbilityDefinition::LoadFromFile ─────────────────────────────────────────
AbilityDefinition AbilityDefinition::LoadFromFile(const std::string &path) {
  std::ifstream file(path);
  if (!file.is_open())
    throw std::runtime_error("Failed to open ability file: " + path);

  nlohmann::json j;
  file >> j;

  AbilityDefinition def;

  // Core identity
  def.id = j.value("id", "");
  def.displayName = j.value("displayName", "");
  if (j.contains("slot"))
    def.slot = parseSlot(j["slot"]);
  if (j.contains("targeting"))
    def.targeting = parseTargeting(j["targeting"]);

  // Resource & Cooldown
  if (j.contains("resourceType"))
    def.resourceType = parseResource(j["resourceType"]);
  if (j.contains("costPerLevel"))
    def.costPerLevel = parseFloatArray5(j["costPerLevel"]);
  if (j.contains("cooldownPerLevel"))
    def.cooldownPerLevel = parseFloatArray5(j["cooldownPerLevel"]);

  // Casting
  def.castTime = j.value("castTime", 0.0f);
  def.channelDuration = j.value("channelDuration", 0.0f);
  def.canMoveWhileCasting = j.value("canMoveWhileCasting", false);
  def.canBeInterrupted = j.value("canBeInterrupted", true);

  // Range & Area
  def.castRange = j.value("castRange", 0.0f);
  if (j.contains("areaShape"))
    def.areaShape = parseAreaShape(j["areaShape"]);
  def.areaRadius = j.value("areaRadius", 0.0f);
  def.areaConeAngle = j.value("areaConeAngle", 0.0f);
  def.areaWidth = j.value("areaWidth", 0.0f);
  def.areaLength = j.value("areaLength", 0.0f);

  // Projectile
  if (j.contains("projectile"))
    def.projectile = parseProjectile(j["projectile"]);

  // Effects
  if (j.contains("onHitEffects")) {
    for (const auto &ej : j["onHitEffects"])
      def.onHitEffects.push_back(parseEffect(ej));
  }
  if (j.contains("onSelfEffects")) {
    for (const auto &ej : j["onSelfEffects"])
      def.onSelfEffects.push_back(parseEffect(ej));
  }

  // Visual & Audio
  def.castAnimation = j.value("castAnimation", "");
  def.castVFX = j.value("castVFX", "");
  def.projectileVFX = j.value("projectileVFX", "");
  def.impactVFX = j.value("impactVFX", "");
  def.castSFX = j.value("castSFX", "");
  def.impactSFX = j.value("impactSFX", "");

  // Tags
  if (j.contains("tags") && j["tags"].is_array()) {
    for (const auto &t : j["tags"])
      def.tags.insert(t.get<std::string>());
  }

  spdlog::info("Loaded ability '{}' ({})", def.displayName, def.id);
  return def;
}

// ── AbilityDefinition::LoadAllFromDirectory ─────────────────────────────────
std::vector<AbilityDefinition>
AbilityDefinition::LoadAllFromDirectory(const std::string &dir) {
  std::vector<AbilityDefinition> defs;
  namespace fs = std::filesystem;

  if (!fs::exists(dir) || !fs::is_directory(dir)) {
    spdlog::warn("Ability directory not found: {}", dir);
    return defs;
  }

  for (const auto &entry : fs::directory_iterator(dir)) {
    if (entry.path().extension() == ".json") {
      try {
        defs.push_back(LoadFromFile(entry.path().string()));
      } catch (const std::exception &e) {
        spdlog::error("Failed to load ability '{}': {}", entry.path().string(),
                      e.what());
      }
    }
  }

  spdlog::info("Loaded {} abilities from {}", defs.size(), dir);
  return defs;
}

} // namespace glory

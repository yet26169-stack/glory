#pragma once

#include "ability/AbilityTypes.h"

#include <glm/glm.hpp>

#include <array>
#include <string>
#include <unordered_set>
#include <vector>

namespace glory {

// ── Scaling formula (per-level base + stat ratios) ──────────────────────────
struct ScalingFormula {
  std::array<float, 5> basePerLevel{}; // [60, 100, 140, 180, 220]
  float adRatio = 0.0f;                // 0.0 - 2.0 typically
  float apRatio = 0.0f;
  float hpRatio = 0.0f; // % of max/bonus/current HP
  HPScalingBasis hpBasis = HPScalingBasis::MAX;
  float armorRatio = 0.0f;
  float mrRatio = 0.0f;

  float evaluate(int level, float ad, float ap, float hpValue, float armor,
                 float mr) const;
};

// ── Stat modifier (for buffs/debuffs) ───────────────────────────────────────
struct StatModifier {
  StatType stat = StatType::AD;
  float amount = 0.0f;
  bool percent = false; // true = multiplicative, false = flat
};

// ── Effect definition ───────────────────────────────────────────────────────
struct EffectDef {
  EffectType type = EffectType::DAMAGE;
  DamageType damageType = DamageType::PHYSICAL;
  ScalingFormula scaling;
  float duration = 0.0f; // for CC / buffs / debuffs / dots
  float tickRate = 0.0f; // for DoTs/HoTs (0 = no ticking)
  StatModifier statMod;
  float value = 0.0f; // slow %, knockback distance, etc.
  bool appliesGrievousWounds = false;
  std::string applyVFX; // VFX on target when active
};

// ── Projectile definition ───────────────────────────────────────────────────
struct ProjectileDef {
  float speed = 0.0f;           // units per second
  float width = 0.0f;           // collision hitbox width
  float maxRange = 0.0f;        // despawn distance
  bool piercing = false;        // passes through targets?
  int maxTargets = 1;           // max entities hit if piercing (-1 = unlimited)
  bool returnsToSource = false; // boomerang-style
  float curveAngle = 0.0f;      // 0 = straight, >0 = arced
  bool destroyOnWall = true;    // collides with terrain?
};

// ── Complete ability definition (data-driven, loaded from JSON) ─────────────
struct AbilityDefinition {
  std::string id;          // "ahri_charm"
  std::string displayName; // "Charm"
  AbilitySlot slot = AbilitySlot::Q;
  TargetingType targeting = TargetingType::NONE;

  // Resource & Cooldown
  ResourceType resourceType = ResourceType::NONE;
  std::array<float, 5> costPerLevel{};
  std::array<float, 5> cooldownPerLevel{};

  // Casting
  float castTime = 0.0f;        // 0 = instant cast
  float channelDuration = 0.0f; // 0 = not a channel
  bool canMoveWhileCasting = false;
  bool canBeInterrupted = true;

  // Range & Area
  float castRange = 0.0f;
  AreaShape areaShape = AreaShape::NONE;
  float areaRadius = 0.0f;
  float areaConeAngle = 0.0f; // degrees
  float areaWidth = 0.0f;
  float areaLength = 0.0f;

  // Projectile (if SKILLSHOT)
  ProjectileDef projectile;

  // Effects
  std::vector<EffectDef> onHitEffects;
  std::vector<EffectDef> onSelfEffects;

  // Visual & Audio references
  std::string castAnimation;
  std::string castVFX;
  std::string projectileVFX;
  std::string impactVFX;
  std::string castSFX;
  std::string impactSFX;

  // Tags
  std::unordered_set<std::string> tags;

  // ── Loading ─────────────────────────────────────────────────────────
  static AbilityDefinition LoadFromFile(const std::string &path);
  static std::vector<AbilityDefinition>
  LoadAllFromDirectory(const std::string &dir);
};

// ── Central database for loaded abilities ───────────────────────────────────
class AbilityDatabase {
public:
  static AbilityDatabase &get() {
    static AbilityDatabase instance;
    return instance;
  }

  void loadFromDirectory(const std::string &dir) {
    auto defs = AbilityDefinition::LoadAllFromDirectory(dir);
    for (auto &def : defs) {
      m_definitions[def.id] = std::move(def);
    }
  }

  void loadFromFile(const std::string &path) {
    auto def = AbilityDefinition::LoadFromFile(path);
    m_definitions[def.id] = std::move(def);
  }

  const AbilityDefinition *find(const std::string &id) const {
    auto it = m_definitions.find(id);
    return (it != m_definitions.end()) ? &it->second : nullptr;
  }

private:
  AbilityDatabase() = default;
  std::unordered_map<std::string, AbilityDefinition> m_definitions;
};

} // namespace glory

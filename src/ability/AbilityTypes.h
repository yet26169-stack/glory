#pragma once

#include <cstdint>
#include <string>

namespace glory {

// ── Ability slot identifiers ────────────────────────────────────────────────
enum class AbilitySlot : uint8_t {
  Q = 0,
  W = 1,
  E = 2,
  R = 3,
  PASSIVE = 4,
  SUMMONER = 5,
  COUNT = 6
};

// ── Ability state machine phases ────────────────────────────────────────────
enum class AbilityPhase : uint8_t {
  READY,
  CASTING,
  CHANNELING,
  EXECUTING,
  INTERRUPTED,
  ON_COOLDOWN
};

// ── Targeting modes ─────────────────────────────────────────────────────────
enum class TargetingType : uint8_t {
  SKILLSHOT, // Direction from caster → spawn projectile
  POINT,     // Ground position → AoE at target
  SELF,      // Caster-centered
  TARGETED,  // Enemy entity under cursor
  VECTOR,    // Start + end point
  NONE       // Automatic / passive
};

// ── Resource types ──────────────────────────────────────────────────────────
enum class ResourceType : uint8_t { MANA, ENERGY, HEALTH, RAGE, NONE };

// ── AoE area shapes ─────────────────────────────────────────────────────────
enum class AreaShape : uint8_t { NONE, CIRCLE, CONE, RECTANGLE, LINE, RING };

// ── Damage types ────────────────────────────────────────────────────────────
enum class DamageType : uint8_t {
  PHYSICAL,
  MAGICAL,
  TRUE_DMG // "TRUE" is a macro on some platforms
};

// ── Effect types (damage, CC, buffs, mobility) ──────────────────────────────
enum class EffectType : uint8_t {
  DAMAGE,
  HEAL,
  SHIELD,
  STUN,
  SLOW,
  ROOT,
  KNOCKBACK,
  KNOCKUP,
  SILENCE,
  SUPPRESS,
  BLIND,
  CHARM,
  FEAR,
  TAUNT,
  BUFF_STAT,
  DEBUFF_STAT,
  DOT,
  HOT,
  DASH,
  BLINK
};

// ── HP scaling basis ────────────────────────────────────────────────────────
enum class HPScalingBasis : uint8_t { MAX, BONUS, CURRENT, MISSING };

// ── Stat identifiers for buff/debuff modifiers ──────────────────────────────
enum class StatType : uint8_t {
  AD,
  AP,
  ARMOR,
  MAGIC_RESIST,
  MOVE_SPEED,
  ATTACK_SPEED,
  MAX_HP,
  MAX_MANA,
  CDR,
  ARMOR_PEN_FLAT,
  ARMOR_PEN_PERCENT,
  MAGIC_PEN_FLAT,
  MAGIC_PEN_PERCENT,
  LIFE_STEAL,
  SPELL_VAMP
};

// ── CC priority (lower number = higher priority) ────────────────────────────
inline int getCCPriority(EffectType type) {
  switch (type) {
  case EffectType::SUPPRESS:
    return 1;
  case EffectType::STUN:
    return 2;
  case EffectType::CHARM:
    return 3;
  case EffectType::FEAR:
    return 3;
  case EffectType::TAUNT:
    return 3;
  case EffectType::KNOCKUP:
    return 4;
  case EffectType::KNOCKBACK:
    return 4;
  case EffectType::ROOT:
    return 5;
  case EffectType::SILENCE:
    return 6;
  case EffectType::SLOW:
    return 7;
  case EffectType::BLIND:
    return 8;
  default:
    return 99;
  }
}

// ── CC disables checks ──────────────────────────────────────────────────────
inline bool preventsMovement(EffectType type) {
  switch (type) {
  case EffectType::STUN:
  case EffectType::SUPPRESS:
  case EffectType::ROOT:
  case EffectType::KNOCKUP:
  case EffectType::KNOCKBACK:
  case EffectType::CHARM:
  case EffectType::FEAR:
  case EffectType::TAUNT:
    return true;
  default:
    return false;
  }
}

inline bool preventsCasting(EffectType type) {
  switch (type) {
  case EffectType::STUN:
  case EffectType::SUPPRESS:
  case EffectType::SILENCE:
  case EffectType::KNOCKUP:
  case EffectType::KNOCKBACK:
  case EffectType::CHARM:
  case EffectType::FEAR:
  case EffectType::TAUNT:
    return true;
  default:
    return false;
  }
}

inline bool preventsAttacking(EffectType type) {
  switch (type) {
  case EffectType::STUN:
  case EffectType::SUPPRESS:
  case EffectType::KNOCKUP:
  case EffectType::KNOCKBACK:
  case EffectType::BLIND:
  case EffectType::CHARM:
  case EffectType::FEAR:
  case EffectType::TAUNT:
    return true;
  default:
    return false;
  }
}

// ── String conversion helpers (for logging / debug) ─────────────────────────
inline const char *toString(AbilitySlot s) {
  switch (s) {
  case AbilitySlot::Q:
    return "Q";
  case AbilitySlot::W:
    return "W";
  case AbilitySlot::E:
    return "E";
  case AbilitySlot::R:
    return "R";
  case AbilitySlot::PASSIVE:
    return "PASSIVE";
  case AbilitySlot::SUMMONER:
    return "SUMMONER";
  default:
    return "UNKNOWN";
  }
}

inline const char *toString(AbilityPhase p) {
  switch (p) {
  case AbilityPhase::READY:
    return "READY";
  case AbilityPhase::CASTING:
    return "CASTING";
  case AbilityPhase::CHANNELING:
    return "CHANNELING";
  case AbilityPhase::EXECUTING:
    return "EXECUTING";
  case AbilityPhase::INTERRUPTED:
    return "INTERRUPTED";
  case AbilityPhase::ON_COOLDOWN:
    return "ON_COOLDOWN";
  default:
    return "UNKNOWN";
  }
}

inline const char *toString(TargetingType t) {
  switch (t) {
  case TargetingType::SKILLSHOT:
    return "SKILLSHOT";
  case TargetingType::POINT:
    return "POINT";
  case TargetingType::SELF:
    return "SELF";
  case TargetingType::TARGETED:
    return "TARGETED";
  case TargetingType::VECTOR:
    return "VECTOR";
  case TargetingType::NONE:
    return "NONE";
  default:
    return "UNKNOWN";
  }
}

} // namespace glory

#pragma once

#include "map/MapTypes.h"

#include <cstdint>

namespace glory {

// ── Minion type archetypes ──────────────────────────────────────────────────

enum class MinionType : uint8_t {
  Melee = 0,
  Caster = 1,
  Siege = 2,
  Super = 3,
  Count = 4
};

inline const char *MinionTypeName(MinionType t) {
  switch (t) {
  case MinionType::Melee:  return "Melee";
  case MinionType::Caster: return "Caster";
  case MinionType::Siege:  return "Siege";
  case MinionType::Super:  return "Super";
  default:                 return "Unknown";
  }
}

// ── Minion AI state machine ─────────────────────────────────────────────────

enum class MinionState : uint8_t {
  Spawning = 0,
  Pathing,
  Engaging,
  Attacking,
  Chasing,
  Returning,
  Dying,
  Dead
};

// ── Attack style ────────────────────────────────────────────────────────────

enum class AttackStyle : uint8_t { Melee, Ranged };

} // namespace glory

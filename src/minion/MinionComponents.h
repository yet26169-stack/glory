#pragma once

#include "minion/MinionTypes.h"
#include "map/MapTypes.h"

#include <entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace glory {

// ── Identity ────────────────────────────────────────────────────────────────

struct MinionIdentityComponent {
  MinionType type = MinionType::Melee;
  TeamID team = TeamID::Blue;
  LaneType lane = LaneType::Mid;
  uint32_t waveIndex = 0;
};

// ── Health ──────────────────────────────────────────────────────────────────

struct MinionHealthComponent {
  float currentHP = 0.0f;
  float maxHP = 0.0f;
  bool isDead = false;
  entt::entity lastAttacker = entt::null; // for last-hit gold
};

// ── Combat stats ────────────────────────────────────────────────────────────

struct MinionCombatComponent {
  float attackDamage = 0.0f;
  float armor = 0.0f;
  float magicResist = 0.0f;
  float attackRange = 0.0f;
  float attackCooldown = 1.0f; // seconds between attacks
  float timeSinceLastAttack = 0.0f;
  AttackStyle attackStyle = AttackStyle::Melee;
  float projectileSpeed = 0.0f;
};

// ── Movement ────────────────────────────────────────────────────────────────

struct MinionMovementComponent {
  float moveSpeed = 325.0f;
  glm::vec3 velocity{0.0f};
  uint32_t currentWaypointIndex = 0;
  bool isReturningToLane = false;
};

// ── Aggro / targeting ───────────────────────────────────────────────────────

struct MinionAggroComponent {
  entt::entity currentTarget = entt::null;
  float aggroRange = 700.0f;
  float leashRange = 900.0f;
  float timeSinceLastTargetEval = 0.0f;
  float championAggroCooldown = 0.0f; // time before champion draw can re-trigger
  float championAggroTimer = 0.0f;    // forced champion target time remaining
  entt::entity forcedChampionTarget = entt::null;
};

// ── AI state ────────────────────────────────────────────────────────────────

struct MinionStateComponent {
  MinionState state = MinionState::Spawning;
  float stateTimer = 0.0f; // time spent in current state
};

// ── Minion projectile (separate from ability projectile) ────────────────────

struct MinionProjectileComponent {
  entt::entity target = entt::null;
  entt::entity owner = entt::null; // which minion fired
  float speed = 650.0f;
  float damage = 0.0f;
  float maxLifetime = 5.0f;
  float age = 0.0f;
};

// ── Tag for spatial hash registration ───────────────────────────────────────

struct MinionTag {};

} // namespace glory

#pragma once
#include "map/MapTypes.h"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <cstdint>

namespace glory {

// ── Tags ────────────────────────────────────────────────────────────────────

struct TowerTag {};
struct InhibitorTag {};
struct NexusTag {};

// ── Identity ────────────────────────────────────────────────────────────────

struct StructureIdentityComponent {
    TeamID    team = TeamID::Blue;
    LaneType  lane = LaneType::Mid;
    TowerTier tier = TowerTier::Outer;  // only meaningful for towers
};

// ── Health ──────────────────────────────────────────────────────────────────

struct StructureHealthComponent {
    float currentHP      = 3500.0f;
    float maxHP          = 3500.0f;
    float hpRegen        = 0.0f;      // HP/s when out of combat
    float outOfCombatTime = 0.0f;     // seconds since last damage taken
    float outOfCombatThreshold = 8.0f; // seconds before regen starts
    bool  isDead         = false;
    bool  isInvulnerable = false;      // nexus invulnerable until towers down
};

// ── Tower Attack ────────────────────────────────────────────────────────────

struct TowerAttackComponent {
    float attackDamage       = 150.0f;
    float attackRange        = 15.0f;
    float attackCooldown     = 0.833f; // ~1.2 attacks/sec
    float timeSinceLastAttack = 0.0f;
    float damageRampPercent  = 0.0f;   // +40% per consecutive hit (resets on target switch)
    float damageRampRate     = 0.40f;  // 40% per hit
    float maxDamageRamp      = 1.20f;  // cap at +120% (4 consecutive hits)
    entt::entity currentTarget = entt::null;
    float projectileSpeed    = 30.0f;  // tower shot travel speed
};

// ── Tower Plates ────────────────────────────────────────────────────────────

struct TowerPlateComponent {
    uint8_t platesRemaining = 5;       // 5 plates on outer towers
    float   plateHP         = 0.0f;    // HP threshold per plate (maxHP / 5)
    float   goldPerPlate    = 160.0f;
    float   armorPerPlate   = 0.40f;   // +40% damage reduction per remaining plate
    float   plateFalloffTime = 840.0f; // plates fall at 14 min (840s)
};

// ── Backdoor Protection ────────────────────────────────────────────────────

struct BackdoorProtectionComponent {
    float damageReduction       = 0.66f;  // 66% damage reduction
    float minionProximityRange  = 20.0f;  // minions must be within this range to disable
    bool  isProtected           = true;   // updated each frame
};

// ── Inhibitor-specific ─────────────────────────────────────────────────────

struct InhibitorComponent {
    float respawnTimer     = 0.0f;    // countdown after destruction
    float respawnTime      = 300.0f;  // 5 minutes
    bool  isRespawning     = false;
};

// ── Nexus-specific ─────────────────────────────────────────────────────────

struct NexusComponent {
    float hpRegen = 5.0f;  // HP per second when out of combat
};

// ── Protection rule: which prerequisite structure must be dead ──────────────

struct ProtectionDependencyComponent {
    entt::entity prerequisite = entt::null; // must be dead before this takes damage
};

// ── Tower projectile (visual, travels from tower to target) ────────────────

struct TowerProjectileComponent {
    entt::entity source = entt::null; // tower
    entt::entity target = entt::null; // target entity
    float speed   = 30.0f;
    float damage  = 150.0f;
    float age     = 0.0f;
    float maxAge  = 3.0f;
};

} // namespace glory

#pragma once

// ── Combat System: ECS Components ─────────────────────────────────────────
// Lightweight combat components for the auto-attack / shield / trick system.
// Separate from AbilitySystem (Q/W/E/R) — this handles the rock-paper-scissors
// melee combat triangle with its own state machine.

#include <entt.hpp>
#include <glm/glm.hpp>
#include <cstdint>

namespace glory {

// ── Team affiliation ──────────────────────────────────────────────────────
enum class Team : uint8_t { PLAYER, ENEMY, NEUTRAL };

struct TeamComponent {
    Team team = Team::NEUTRAL;
};

// Marker for debug-spawned test dummies (never moves, never attacks)
struct TestDummyTag {};

// ── Combat state machine ──────────────────────────────────────────────────
enum class CombatState : uint8_t {
    IDLE,             // Can perform any action
    AUTO_ATTACKING,   // In auto-attack windup → hit
    SHIELDING,        // Shield is active (blocks attacks)
    TRICKING,         // Performing trick windup → resolve
    STUNNED           // Cannot act (result of failed trick or broken shield)
};

struct CombatComponent {
    CombatState state = CombatState::IDLE;
    float stateTimer  = 0.0f;

    // ── Auto-attack ───────────────────────────────────────────────────────
    float attackRange      = 3.0f;
    float attackCooldown   = 0.0f;
    float attackSpeed      = 1.0f;   // attacks per second
    float attackWindup     = 0.3f;
    float attackDamage     = 60.0f;

    // ── Shield ────────────────────────────────────────────────────────────
    float shieldDuration     = 3.5f;
    float shieldCooldown     = 0.0f;
    float shieldCooldownBase = 2.0f;
    uint32_t shieldVfxHandle = 0;

    // ── Trick ─────────────────────────────────────────────────────────────
    float trickRange        = 3.5f;
    float trickCooldown     = 0.0f;
    float trickCooldownBase = 3.0f;
    float trickWindup       = 0.2f;
    float stunDuration      = 1.0f;

    // ── Target ────────────────────────────────────────────────────────────
    entt::entity targetEntity = entt::null;
};

} // namespace glory

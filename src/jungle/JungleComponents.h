#pragma once
#include "map/MapTypes.h"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <string>
#include <cstdint>

namespace glory {

struct JungleMonsterTag {};

// ── Identity ────────────────────────────────────────────────────────────────

struct MonsterIdentityComponent {
    CampType campType     = CampType::Wolves;
    uint32_t campIndex    = 0;   // index into MapData::neutralCamps
    uint32_t mobIndex     = 0;   // index within the camp (0 = big monster)
    bool     isBigMonster = false; // big monster in multi-mob camp
};

// ── Health ──────────────────────────────────────────────────────────────────

struct MonsterHealthComponent {
    float currentHP = 0.0f;
    float maxHP     = 0.0f;
    bool  isDead    = false;
    entt::entity lastAttacker = entt::null;
};

// ── Combat ──────────────────────────────────────────────────────────────────

struct MonsterCombatComponent {
    float attackDamage   = 0.0f;
    float armor          = 0.0f;
    float magicResist    = 0.0f;
    float attackRange    = 2.0f;
    float attackCooldown = 1.0f;
    float timeSinceLastAttack = 0.0f;
};

// ── Aggro & Leash ───────────────────────────────────────────────────────────

struct MonsterAggroComponent {
    entt::entity currentTarget = entt::null;
    float aggroRange    = 5.0f;   // detect attackers
    float leashRadius   = 0.0f;   // max pull distance from camp origin
    glm::vec3 homePosition{0.0f}; // camp spawn position (return here on leash)
    float patience      = 0.0f;   // time out of combat before resetting
    float patienceMax   = 8.0f;   // seconds before full reset
    bool  isResetting   = false;  // walking back to home
};

// ── AI State ────────────────────────────────────────────────────────────────

enum class MonsterState : uint8_t {
    Idle,        // Standing at camp, not aggroed
    Attacking,   // In combat, attacking target
    Chasing,     // Target fled, chasing within leash
    Resetting,   // Returning to home position (heals to full)
    Dying,       // Death animation
    Dead         // Pending cleanup
};

struct MonsterStateComponent {
    MonsterState state = MonsterState::Idle;
    float stateTimer   = 0.0f;
};

// ── Movement ────────────────────────────────────────────────────────────────

struct MonsterMovementComponent {
    float moveSpeed = 3.0f; // world units per second
    glm::vec3 velocity{0.0f};
};

// ── Buff on kill ────────────────────────────────────────────────────────────

struct MonsterBuffComponent {
    std::string buffId;       // "red_buff", "blue_buff", "baron_buff", "dragon_soul"
    float buffDuration = 0.0f; // 0 = permanent (dragon soul)
};

// ── Camp controller (one per NeutralCamp, tracks respawn) ───────────────────

struct CampControllerComponent {
    uint32_t campIndex   = 0;
    CampType campType    = CampType::Wolves;
    float respawnTimer   = 0.0f; // countdown to next spawn
    float respawnTime    = 0.0f; // configured respawn interval
    float initialSpawnTime = 0.0f; // when the camp first appears
    bool  isAlive        = false; // any mob in this camp alive?
    uint8_t mobCount     = 0;
    uint8_t aliveMobs    = 0;
    glm::vec3 position{0.0f};
};

} // namespace glory

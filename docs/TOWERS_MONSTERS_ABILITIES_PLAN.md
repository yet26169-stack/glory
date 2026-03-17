# Towers, Nexus, Inhibitors, Jungle Monsters & Expanded Abilities

## Senior Game Designer Implementation Plan — Vulkan C++ Performance-First

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [Phase 1: Generic Structure System (Towers + Inhibitors + Nexus)](#2-phase-1-generic-structure-system)
3. [Phase 2: Jungle Monster System](#3-phase-2-jungle-monster-system)
4. [Phase 3: Expanded Ability Types](#4-phase-3-expanded-ability-types)
5. [Phase 4: Renderer Integration](#5-phase-4-renderer-integration)
6. [Phase 5: Build System & Tests](#6-phase-5-build-system--tests)
7. [Performance Budget](#7-performance-budget)
8. [Implementation Order](#8-implementation-order)
9. [File Manifest](#9-file-manifest)

---

## 1. Architecture Overview

### Design Principles

1. **Data-driven**: All stats in JSON. Zero hardcoded balance numbers in C++.
2. **ECS-native**: Every gameplay entity is an entt entity with composable components. Systems are stateless iterators over component views.
3. **Zero new Vulkan pipelines**: Towers, inhibitors, nexus, and monsters reuse the existing instanced indirect draw pipeline (`triangle.vert/frag`). No new shaders, no new descriptor sets, no new render passes.
4. **Spatial coherence**: Structures are static — their positions never change. Store them in a flat array indexed by `TeamID × LaneType × TowerTier` for O(1) lookup instead of spatial hash queries. Monsters reuse the existing `SpatialHash` with a dedicated cell layer.
5. **Batch-friendly**: Group structures by mesh type. All outer towers share one mesh, inner towers another, etc. This yields at most 4-5 new instanced indirect draw calls for all 30+ structures.
6. **Follow MinionSystem pattern**: Components header + Config loader + System class + JSON config.

### Entity Budget (existing + new)

| Category | Count | Instance Cost | Total |
|---|---|---|---|
| Player character | 1 | 176 B | 176 B |
| Minions (peak ~80) | 80 | 176 B | 14 KB |
| Minion projectiles (~20) | 20 | 176 B | 3.5 KB |
| Ability projectiles (~10) | 10 | 176 B | 1.8 KB |
| **Towers (22)** | 22 | 176 B | 3.9 KB |
| **Inhibitors (6)** | 6 | 176 B | 1.1 KB |
| **Nexus (2)** | 2 | 176 B | 352 B |
| **Jungle monsters (~30)** | 30 | 176 B | 5.3 KB |
| **Tower projectiles (~5)** | 5 | 176 B | 880 B |
| Map mesh | 1 | 176 B | 176 B |
| **Total** | ~177 | — | ~31 KB |

Well under the 1024-entity instance buffer capacity (180 KB). No reallocation needed.

---

## 2. Phase 1: Generic Structure System

### 2a. New ECS Components

**File**: `src/structure/StructureComponents.h`

```cpp
#pragma once
#include "map/MapTypes.h"
#include <entt.hpp>
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
```

### 2b. Structure Config (JSON-driven)

**File**: `assets/config/structure_config.json`

```json
{
    "towers": {
        "outer": {
            "maxHP": 5000,
            "attackDamage": 152,
            "attackRange": 15.0,
            "attackCooldown": 0.833,
            "armorBase": 40,
            "magicResistBase": 40,
            "projectileSpeed": 30.0,
            "plates": {
                "count": 5,
                "goldPerPlate": 160,
                "armorPerPlate": 0.40,
                "falloffTime": 840
            },
            "backdoorReduction": 0.66
        },
        "inner": {
            "maxHP": 3500,
            "attackDamage": 170,
            "attackRange": 15.0,
            "attackCooldown": 0.833,
            "armorBase": 55,
            "magicResistBase": 55,
            "projectileSpeed": 30.0,
            "backdoorReduction": 0.66
        },
        "inhibitor_tower": {
            "maxHP": 3500,
            "attackDamage": 170,
            "attackRange": 15.0,
            "attackCooldown": 0.833,
            "armorBase": 55,
            "magicResistBase": 55,
            "projectileSpeed": 30.0,
            "backdoorReduction": 0.66
        },
        "nexus_tower": {
            "maxHP": 2700,
            "attackDamage": 180,
            "attackRange": 15.0,
            "attackCooldown": 0.833,
            "armorBase": 65,
            "magicResistBase": 65,
            "projectileSpeed": 30.0,
            "backdoorReduction": 0.66
        },
        "damageRamp": {
            "rate": 0.40,
            "max": 1.20
        }
    },
    "inhibitors": {
        "maxHP": 4000,
        "armorBase": 20,
        "magicResistBase": 20,
        "respawnTime": 300.0
    },
    "nexus": {
        "maxHP": 5500,
        "armorBase": 0,
        "magicResistBase": 0,
        "hpRegen": 5.0,
        "outOfCombatThreshold": 8.0
    },
    "targetingPriority": [
        "champion_attacking_ally_champion",
        "champion_attacking_ally_minion",
        "closest_minion",
        "closest_champion"
    ]
}
```

**File**: `src/structure/StructureConfig.h`

```cpp
#pragma once
#include <string>
#include <array>

namespace glory {

struct TowerTierConfig {
    float maxHP          = 3500.0f;
    float attackDamage   = 150.0f;
    float attackRange    = 15.0f;
    float attackCooldown = 0.833f;
    float armor          = 40.0f;
    float magicResist    = 40.0f;
    float projectileSpeed = 30.0f;
    float backdoorReduction = 0.66f;
    // Plates (outer only)
    int   plateCount     = 0;
    float goldPerPlate   = 160.0f;
    float armorPerPlate  = 0.40f;
    float plateFalloffTime = 840.0f;
};

struct InhibitorConfig {
    float maxHP       = 4000.0f;
    float armor       = 20.0f;
    float magicResist = 20.0f;
    float respawnTime = 300.0f;
};

struct NexusConfig {
    float maxHP       = 5500.0f;
    float armor       = 0.0f;
    float magicResist = 0.0f;
    float hpRegen     = 5.0f;
    float outOfCombatThreshold = 8.0f;
};

struct StructureConfig {
    TowerTierConfig towers[4]; // [Outer, Inner, Inhibitor, Nexus]
    float damageRampRate = 0.40f;
    float damageRampMax  = 1.20f;
    InhibitorConfig inhibitor;
    NexusConfig nexus;
};

namespace StructureConfigLoader {
    StructureConfig Load(const std::string &configDir);
}

} // namespace glory
```

**File**: `src/structure/StructureConfig.cpp` — JSON loading with nlohmann::json (same pattern as `MinionConfig.cpp`).

### 2c. Structure System

**File**: `src/structure/StructureSystem.h`

```cpp
#pragma once
#include "structure/StructureConfig.h"
#include "map/MapTypes.h"
#include <entt.hpp>
#include <glm/glm.hpp>
#include <vector>
#include <functional>

namespace glory {

using HeightQueryFn = std::function<float(float, float)>;

struct StructureDeathEvent {
    entt::entity entity;
    TeamID team;
    LaneType lane;
    EntityType type;     // Tower, Inhibitor, Nexus
    TowerTier tier;      // for towers
    glm::vec3 position;
    entt::entity lastAttacker;
};

class StructureSystem {
public:
    void init(const StructureConfig &config, const MapData &mapData,
              entt::registry &registry, HeightQueryFn heightFn = nullptr);

    void update(entt::registry &registry, float dt, float gameTime);

    /// Returns pending death events and clears the internal queue.
    std::vector<StructureDeathEvent> consumeDeathEvents();

    /// Check if a specific inhibitor is dead (for super minion spawning).
    bool isInhibitorDead(TeamID team, LaneType lane) const;

    /// Check if the game is over (a nexus was destroyed).
    bool isGameOver() const { return m_gameOver; }
    TeamID getWinningTeam() const { return m_winningTeam; }

    /// Get tower entity for a specific slot (O(1) lookup).
    entt::entity getTower(TeamID team, LaneType lane, TowerTier tier) const;

private:
    StructureConfig m_config;

    // O(1) tower lookup: [team][lane][tier]
    entt::entity m_towers[2][3][4] = {};    // TeamID × LaneType × TowerTier
    entt::entity m_inhibitors[2][3] = {};    // TeamID × LaneType
    entt::entity m_nexus[2] = {};            // TeamID

    std::vector<StructureDeathEvent> m_deathEvents;

    bool   m_gameOver    = false;
    TeamID m_winningTeam = TeamID::Blue;

    // ── Private helpers ──
    void spawnTowers(entt::registry &registry, const MapData &mapData,
                     HeightQueryFn heightFn);
    void spawnInhibitors(entt::registry &registry, const MapData &mapData,
                         HeightQueryFn heightFn);
    void spawnNexus(entt::registry &registry, const MapData &mapData,
                    HeightQueryFn heightFn);
    void wireProtectionDependencies(entt::registry &registry);

    void updateTargeting(entt::registry &registry, float dt);
    void updateCombat(entt::registry &registry, float dt);
    void updateProjectiles(entt::registry &registry, float dt);
    void updateBackdoorProtection(entt::registry &registry);
    void updateInhibitorRespawns(entt::registry &registry, float dt);
    void updateNexusRegen(entt::registry &registry, float dt);
    void updatePlates(entt::registry &registry, float gameTime);
    void processDeaths(entt::registry &registry);
};

} // namespace glory
```

### 2d. Tower Targeting Logic (Performance-Critical)

Tower targeting priority (LoL-accurate):
1. Enemy champion who is attacking an allied champion within tower range
2. Enemy champion who is attacking an allied minion within tower range
3. Closest enemy minion within tower range
4. Closest enemy champion within tower range

```
updateTargeting() — called every frame at 30 Hz
    For each tower with TowerAttackComponent:
        if currentTarget is valid, alive, and in range → keep it
        else → re-evaluate:
            1. Linear scan of nearby champions checking aggro conditions
            2. Linear scan of nearby minions for closest
            // With 22 towers × ~40 enemies = ~880 checks/frame
            // At 30 Hz this is ~26,400 checks/sec — trivially fast
            // No spatial hash needed for towers (static positions, small range)
```

**Why no spatial hash for towers**: Towers are static (22 total, never move). Each tower checks at most ~40 entities in range. A branchless distance check is `3 FMAs + 1 compare = ~4 cycles`. 22 × 40 × 4 cycles = 3,520 cycles = **~1.2 microseconds** at 3 GHz. Spatial hash overhead (rebuild, bucket lookup) would cost more than the linear scan.

### 2e. Protection Dependency Wiring

After spawning all structures, wire `ProtectionDependencyComponent`:

```
For each team:
    inner_top.prerequisite  = outer_top
    inner_mid.prerequisite  = outer_mid
    inner_bot.prerequisite  = outer_bot
    inhib_tower_top.prerequisite = inner_top
    inhib_tower_mid.prerequisite = inner_mid
    inhib_tower_bot.prerequisite = inner_bot
    inhibitor_top.prerequisite   = inhib_tower_top
    inhibitor_mid.prerequisite   = inhib_tower_mid
    inhibitor_bot.prerequisite   = inhib_tower_bot
    nexus_tower_0.prerequisite   = any inhibitor  (special: all 3 inhib towers must be dead)
    nexus_tower_1.prerequisite   = any inhibitor
    nexus.prerequisite = both nexus towers (special: both must be dead)
```

Implementation: `StructureHealthComponent::isInvulnerable` is set true when prerequisite is alive. Damage is rejected when invulnerable. The `processDeaths()` method updates downstream dependencies when a structure dies.

---

## 3. Phase 2: Jungle Monster System

### 3a. New ECS Components

**File**: `src/jungle/JungleComponents.h`

```cpp
#pragma once
#include "map/MapTypes.h"
#include <entt.hpp>
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
```

### 3b. Monster Config (JSON-driven)

**File**: `assets/config/monster_config.json`

```json
{
    "camps": {
        "RedBuff": {
            "mobs": [
                { "name": "Red Brambleback", "hp": 1800, "ad": 75, "armor": 20, "mr": 20, "range": 2.5, "cooldown": 0.8, "big": true },
                { "name": "Cinderling", "hp": 400, "ad": 15, "armor": 5, "mr": 5, "range": 2.0, "cooldown": 1.5, "big": false },
                { "name": "Cinderling", "hp": 400, "ad": 15, "armor": 5, "mr": 5, "range": 2.0, "cooldown": 1.5, "big": false }
            ],
            "buff": { "id": "red_buff", "duration": 120.0 },
            "xpReward": 200,
            "goldReward": 100
        },
        "BlueBuff": {
            "mobs": [
                { "name": "Blue Sentinel", "hp": 1800, "ad": 75, "armor": 20, "mr": 20, "range": 2.5, "cooldown": 0.8, "big": true },
                { "name": "Sentry", "hp": 400, "ad": 15, "armor": 5, "mr": 5, "range": 2.0, "cooldown": 1.5, "big": false },
                { "name": "Sentry", "hp": 400, "ad": 15, "armor": 5, "mr": 5, "range": 2.0, "cooldown": 1.5, "big": false }
            ],
            "buff": { "id": "blue_buff", "duration": 120.0 },
            "xpReward": 200,
            "goldReward": 100
        },
        "Wolves": {
            "mobs": [
                { "name": "Greater Murk Wolf", "hp": 1300, "ad": 42, "armor": 10, "mr": 0, "range": 2.0, "cooldown": 1.0, "big": true },
                { "name": "Murk Wolf", "hp": 480, "ad": 16, "armor": 5, "mr": 0, "range": 2.0, "cooldown": 1.2, "big": false },
                { "name": "Murk Wolf", "hp": 480, "ad": 16, "armor": 5, "mr": 0, "range": 2.0, "cooldown": 1.2, "big": false }
            ],
            "xpReward": 135,
            "goldReward": 68
        },
        "Raptors": {
            "mobs": [
                { "name": "Crimson Raptor", "hp": 800, "ad": 20, "armor": 15, "mr": 0, "range": 2.0, "cooldown": 1.0, "big": true },
                { "name": "Raptor", "hp": 350, "ad": 10, "armor": 5, "mr": 0, "range": 2.0, "cooldown": 1.2, "big": false },
                { "name": "Raptor", "hp": 350, "ad": 10, "armor": 5, "mr": 0, "range": 2.0, "cooldown": 1.2, "big": false },
                { "name": "Raptor", "hp": 350, "ad": 10, "armor": 5, "mr": 0, "range": 2.0, "cooldown": 1.2, "big": false },
                { "name": "Raptor", "hp": 350, "ad": 10, "armor": 5, "mr": 0, "range": 2.0, "cooldown": 1.2, "big": false },
                { "name": "Raptor", "hp": 350, "ad": 10, "armor": 5, "mr": 0, "range": 2.0, "cooldown": 1.2, "big": false }
            ],
            "xpReward": 95,
            "goldReward": 85
        },
        "Gromp": {
            "mobs": [
                { "name": "Gromp", "hp": 1800, "ad": 80, "armor": 0, "mr": 0, "range": 3.0, "cooldown": 1.0, "big": true }
            ],
            "xpReward": 135,
            "goldReward": 80
        },
        "Krugs": {
            "mobs": [
                { "name": "Ancient Krug", "hp": 1350, "ad": 60, "armor": 25, "mr": 0, "range": 2.0, "cooldown": 1.0, "big": true },
                { "name": "Krug", "hp": 500, "ad": 25, "armor": 10, "mr": 0, "range": 2.0, "cooldown": 1.2, "big": false }
            ],
            "xpReward": 168,
            "goldReward": 100
        },
        "Scuttler": {
            "mobs": [
                { "name": "Rift Scuttler", "hp": 1050, "ad": 0, "armor": 60, "mr": 60, "range": 0, "cooldown": 99, "big": true }
            ],
            "xpReward": 115,
            "goldReward": 70,
            "passiveBehavior": "flee"
        },
        "Dragon": {
            "mobs": [
                { "name": "Elemental Drake", "hp": 4950, "ad": 150, "armor": 30, "mr": 70, "range": 5.0, "cooldown": 0.5, "big": true }
            ],
            "buff": { "id": "dragon_soul", "duration": 0 },
            "xpReward": 300,
            "goldReward": 300,
            "isEpic": true
        },
        "Baron": {
            "mobs": [
                { "name": "Baron Nashor", "hp": 12000, "ad": 285, "armor": 120, "mr": 70, "range": 6.0, "cooldown": 0.75, "big": true }
            ],
            "buff": { "id": "baron_buff", "duration": 180.0 },
            "xpReward": 600,
            "goldReward": 500,
            "isEpic": true
        },
        "Herald": {
            "mobs": [
                { "name": "Rift Herald", "hp": 6400, "ad": 105, "armor": 60, "mr": 50, "range": 4.0, "cooldown": 0.8, "big": true }
            ],
            "xpReward": 200,
            "goldReward": 100,
            "isEpic": true,
            "maxKills": 2
        }
    },
    "scaling": {
        "hpPerMinute": 0.03,
        "adPerMinute": 0.02
    }
}
```

**File**: `src/jungle/JungleConfig.h` / `src/jungle/JungleConfig.cpp`

```cpp
struct MonsterMobDef {
    std::string name;
    float hp, ad, armor, mr, range, cooldown;
    bool isBig;
};

struct MonsterBuffDef {
    std::string id;
    float duration; // 0 = permanent
};

struct CampDef {
    CampType type;
    std::vector<MonsterMobDef> mobs;
    MonsterBuffDef buff;
    float xpReward, goldReward;
    bool isEpic;
    std::string passiveBehavior; // "flee" for scuttler
    int maxKills; // 0 = unlimited
};

struct JungleConfig {
    std::unordered_map<CampType, CampDef> camps;
    float hpScalePerMinute;
    float adScalePerMinute;
};

namespace JungleConfigLoader {
    JungleConfig Load(const std::string &configDir);
}
```

### 3c. Jungle System

**File**: `src/jungle/JungleSystem.h`

```cpp
#pragma once
#include "jungle/JungleConfig.h"
#include "map/MapTypes.h"
#include <entt.hpp>
#include <vector>
#include <functional>

namespace glory {

using HeightQueryFn = std::function<float(float, float)>;

struct MonsterDeathEvent {
    entt::entity entity;
    CampType campType;
    uint32_t campIndex;
    glm::vec3 position;
    entt::entity killer;
    float goldReward;
    float xpReward;
    std::string buffId;
    float buffDuration;
};

class JungleSystem {
public:
    void init(const JungleConfig &config, const MapData &mapData,
              entt::registry &registry, HeightQueryFn heightFn = nullptr);

    void update(entt::registry &registry, float dt, float gameTime,
                HeightQueryFn heightFn = nullptr);

    std::vector<MonsterDeathEvent> consumeDeathEvents();

private:
    JungleConfig m_config;
    std::vector<entt::entity> m_campControllers; // one per NeutralCamp

    std::vector<MonsterDeathEvent> m_deathEvents;

    // ── Private helpers ──
    void spawnCamp(entt::registry &registry, uint32_t campIndex,
                   const NeutralCamp &camp, HeightQueryFn heightFn);
    void updateSpawnTimers(entt::registry &registry, float dt, float gameTime,
                           HeightQueryFn heightFn);
    void updateAggro(entt::registry &registry, float dt);
    void updateStateTransitions(entt::registry &registry, float dt);
    void updateMovement(entt::registry &registry, float dt,
                        HeightQueryFn heightFn);
    void updateCombat(entt::registry &registry, float dt);
    void processDeaths(entt::registry &registry, float gameTime);
    void updateReset(entt::registry &registry, float dt);
};

} // namespace glory
```

### 3d. Monster AI State Machine

```
State Machine (simpler than minions — no lane pathing):

    ┌──────┐
    │ Idle │ ← monster standing at camp
    └──┬───┘
       │ player enters aggroRange OR takes damage
       ▼
    ┌──────────┐
    │ Attacking │ ← in melee range, dealing damage
    └──┬───────┘
       │ target moves out of melee but within leash
       ▼
    ┌─────────┐
    │ Chasing │ ← moving toward target
    └──┬──────┘
       │ target exceeds leashRadius OR patience expires
       ▼
    ┌───────────┐
    │ Resetting │ ← walking home, healing to full, invulnerable
    └──┬────────┘
       │ arrived at home position
       ▼
    ┌──────┐
    │ Idle │
    └──────┘

Special: Scuttler → MonsterState::Idle + "flee" behavior (runs away from attacker)
```

### 3e. Aggro Logic (Performance)

Monsters only check aggro against entities that are **damaging them** or are within their small aggro range (~5 units). Unlike minions (700-unit aggro range), monsters don't need spatial hash — they react to damage events.

```
updateAggro():
    For each monster in Idle state:
        if monster.currentHP < monster.maxHP (took damage):
            target = monster.lastAttacker
            transition → Attacking
        elif any champion within aggroRange (5 units):
            target = closest champion
            transition → Attacking

    // ~30 monsters × 1 champion = 30 distance checks = negligible
```

---

## 4. Phase 3: Expanded Ability Types

### 4a. TARGETED Ability Support (Point-and-Click)

The `AbilityDefinition` already supports `TargetingType::TARGETED` but `ProjectileSystem` only handles `SKILLSHOT` and `POINT`. Add a new method:

**File modified**: `src/ability/ProjectileSystem.h` — add:

```cpp
entt::entity spawnTargeted(Scene &scene, entt::entity caster,
                           entt::entity target,
                           const std::string &abilityId, int level,
                           float speed, const glm::vec4 &colour);
```

**File modified**: `src/ability/ProjectileSystem.cpp` — add implementation:

```cpp
entt::entity ProjectileSystem::spawnTargeted(
    Scene &scene, entt::entity caster, entt::entity target,
    const std::string &abilityId, int level,
    float speed, const glm::vec4 &colour)
{
    auto &reg = scene.getRegistry();
    auto &casterT = reg.get<TransformComponent>(caster);

    auto e = scene.createEntity("TargetedProjectile");
    auto &t = reg.get<TransformComponent>(e);
    t.position = casterT.position + glm::vec3(0, 1.0f, 0);
    t.scale = glm::vec3(0.3f);

    // Direction will be updated each frame to track the target
    reg.emplace<MeshComponent>(e, MeshComponent{m_sphereMeshIndex});
    reg.emplace<MaterialComponent>(e, MaterialComponent{m_defaultTexIndex, m_flatNormIndex, 0, 0, 0.3f, 3.0f});
    reg.emplace<ColorComponent>(e, ColorComponent{colour});

    ProjectileComponent pc{};
    pc.velocity = glm::vec3(0); // updated per frame
    pc.speed = speed;
    pc.maxRange = 999.0f; // targeted projectiles always hit
    pc.caster = caster;
    pc.hitOnArrival = false;
    pc.abilityId = abilityId;
    pc.abilityLevel = level;
    reg.emplace<ProjectileComponent>(e, pc);

    // New: track target entity
    // Store target in a new TargetedProjectileTag
    reg.emplace<TargetedProjectileTag>(e, TargetedProjectileTag{target});
    reg.emplace<RotateComponent>(e, RotateComponent{{0, 1, 0}, 8.0f});

    return e;
}
```

**New component** (add to `src/scene/Components.h`):

```cpp
struct TargetedProjectileTag {
    entt::entity target = entt::null;
};
```

**File modified**: `src/ability/ProjectileSystem.cpp` `update()` — add targeted projectile homing:

```cpp
// Before existing collision checks, handle targeted projectile homing:
auto targetedView = reg.view<ProjectileComponent, TransformComponent,
                              TargetedProjectileTag>();
for (auto e : targetedView) {
    auto &proj = targetedView.get<ProjectileComponent>(e);
    auto &projT = targetedView.get<TransformComponent>(e);
    auto &tag = targetedView.get<TargetedProjectileTag>(e);

    if (tag.target == entt::null || !reg.valid(tag.target) ||
        !reg.all_of<TransformComponent>(tag.target)) {
        reg.destroy(e);
        continue;
    }
    auto &targetT = reg.get<TransformComponent>(tag.target);
    glm::vec3 dir = targetT.position - projT.position;
    float dist = glm::length(dir);
    if (dist < 0.5f) {
        // Hit! Queue effects and destroy
        // (same onHit logic as skillshot collision)
        // ...
        reg.destroy(e);
    } else {
        proj.velocity = glm::normalize(dir) * proj.speed;
    }
}
```

### 4b. DASH/BLINK Implementation

Currently stubs in `EffectSystem::apply()`. Implement:

**DASH**: Move caster along a direction over a duration. Add a `DashComponent`:

```cpp
struct DashComponent {
    glm::vec3 startPos{0.0f};
    glm::vec3 endPos{0.0f};
    float duration     = 0.3f;
    float elapsed      = 0.0f;
    bool  isUntargetable = false; // some dashes grant untargetability
};
```

**File modified**: `src/ability/EffectSystem.cpp` — in the DASH case:

```cpp
case EffectType::DASH: {
    auto &targetT = reg.get<TransformComponent>(pending.target);
    glm::vec3 dir = glm::normalize(targetInfo.direction);
    float dashDist = def->value; // distance in units
    reg.emplace_or_replace<DashComponent>(pending.target, DashComponent{
        targetT.position,
        targetT.position + dir * dashDist,
        def->duration,
        0.0f,
        false
    });
    break;
}
```

**New system**: Process `DashComponent` entities each frame — lerp position from start to end over duration, then remove component.

**BLINK**: Instant teleport. No component needed:

```cpp
case EffectType::BLINK: {
    auto &targetT = reg.get<TransformComponent>(pending.target);
    glm::vec3 dir = glm::normalize(targetInfo.direction);
    targetT.position += dir * def->value;
    break;
}
```

### 4c. New Champion Ability Set — Frost Archer

**File**: `assets/abilities/frost_archer_volley.json` (Q — SKILLSHOT, multi-arrow)

```json
{
    "id": "frost_archer_volley",
    "displayName": "Volley",
    "slot": "Q",
    "targeting": "SKILLSHOT",
    "resourceType": "MANA",
    "costPerLevel": [50, 55, 60, 65, 70],
    "cooldownPerLevel": [12.0, 10.0, 8.0, 6.0, 4.0],
    "castTime": 0.0,
    "castRange": 1200.0,
    "projectile": {
        "speed": 2000.0,
        "width": 20.0,
        "maxRange": 1200.0,
        "piercing": false,
        "maxTargets": 1,
        "destroyOnWall": false
    },
    "onHitEffects": [
        {
            "type": "DAMAGE",
            "damageType": "PHYSICAL",
            "scaling": { "basePerLevel": [20, 35, 50, 65, 80], "adRatio": 1.0 }
        },
        {
            "type": "SLOW",
            "duration": 2.0,
            "value": 0.20
        }
    ],
    "tags": ["damage", "skillshot", "physical", "slow"]
}
```

**File**: `assets/abilities/frost_archer_hawk.json` (W — POINT, global reveal)

```json
{
    "id": "frost_archer_hawk",
    "displayName": "Hawkshot",
    "slot": "W",
    "targeting": "POINT",
    "resourceType": "NONE",
    "costPerLevel": [0, 0, 0, 0, 0],
    "cooldownPerLevel": [60.0, 55.0, 50.0, 45.0, 40.0],
    "castTime": 0.0,
    "castRange": 2500.0,
    "projectile": {
        "speed": 1600.0,
        "width": 10.0,
        "maxRange": 2500.0,
        "piercing": true,
        "maxTargets": -1
    },
    "onHitEffects": [],
    "tags": ["utility", "skillshot", "vision"]
}
```

**File**: `assets/abilities/frost_archer_enchant.json` (E — SELF buff, attack speed)

```json
{
    "id": "frost_archer_enchant",
    "displayName": "Ranger's Focus",
    "slot": "E",
    "targeting": "SELF",
    "resourceType": "MANA",
    "costPerLevel": [40, 40, 40, 40, 40],
    "cooldownPerLevel": [16.0, 14.0, 12.0, 10.0, 8.0],
    "castTime": 0.0,
    "canMoveWhileCasting": true,
    "canBeInterrupted": false,
    "onSelfEffects": [
        {
            "type": "BUFF_STAT",
            "statMod": { "stat": "ATTACK_SPEED", "amount": 0.40, "percent": true },
            "duration": 6.0
        },
        {
            "type": "BUFF_STAT",
            "statMod": { "stat": "AD", "amount": 25, "percent": false },
            "duration": 6.0
        }
    ],
    "tags": ["buff", "self", "attack_speed"]
}
```

**File**: `assets/abilities/frost_archer_arrow.json` (R — SKILLSHOT, global stun)

```json
{
    "id": "frost_archer_arrow",
    "displayName": "Enchanted Crystal Arrow",
    "slot": "R",
    "targeting": "SKILLSHOT",
    "resourceType": "MANA",
    "costPerLevel": [100, 100, 100],
    "cooldownPerLevel": [100.0, 80.0, 60.0],
    "castTime": 0.25,
    "castRange": 9999.0,
    "projectile": {
        "speed": 1600.0,
        "width": 80.0,
        "maxRange": 9999.0,
        "piercing": false,
        "maxTargets": 1,
        "destroyOnWall": false
    },
    "onHitEffects": [
        {
            "type": "DAMAGE",
            "damageType": "MAGICAL",
            "scaling": { "basePerLevel": [200, 350, 500], "apRatio": 1.0 }
        },
        {
            "type": "STUN",
            "duration": 1.5
        }
    ],
    "tags": ["damage", "skillshot", "magical", "stun", "global"]
}
```

### 4d. Additional Ability — Tank Support Kit

**File**: `assets/abilities/iron_guardian_charge.json` (Q — DASH + stun)

```json
{
    "id": "iron_guardian_charge",
    "displayName": "Unstoppable Charge",
    "slot": "Q",
    "targeting": "SKILLSHOT",
    "resourceType": "MANA",
    "costPerLevel": [60, 65, 70, 75, 80],
    "cooldownPerLevel": [12.0, 11.0, 10.0, 9.0, 8.0],
    "castTime": 0.0,
    "castRange": 600.0,
    "onHitEffects": [
        {
            "type": "DAMAGE",
            "damageType": "PHYSICAL",
            "scaling": { "basePerLevel": [60, 100, 140, 180, 220], "adRatio": 0.5, "hpRatio": 0.04, "hpBasis": "MAX" }
        },
        { "type": "STUN", "duration": 1.0 }
    ],
    "onSelfEffects": [
        { "type": "DASH", "value": 600.0, "duration": 0.4 }
    ],
    "tags": ["damage", "engage", "physical", "stun", "dash"]
}
```

**File**: `assets/abilities/iron_guardian_shield_wall.json` (W — SELF, shield + taunt)

```json
{
    "id": "iron_guardian_shield_wall",
    "displayName": "Shield Wall",
    "slot": "W",
    "targeting": "SELF",
    "resourceType": "MANA",
    "costPerLevel": [55, 60, 65, 70, 75],
    "cooldownPerLevel": [18.0, 16.0, 14.0, 12.0, 10.0],
    "castTime": 0.0,
    "canMoveWhileCasting": true,
    "canBeInterrupted": false,
    "areaShape": "CIRCLE",
    "areaRadius": 4.0,
    "onSelfEffects": [
        {
            "type": "SHIELD",
            "scaling": { "basePerLevel": [80, 120, 160, 200, 240], "hpRatio": 0.08, "hpBasis": "MAX" },
            "duration": 4.0
        }
    ],
    "onHitEffects": [
        { "type": "TAUNT", "duration": 1.25 }
    ],
    "tags": ["defense", "taunt", "shield", "aoe"]
}
```

**File**: `assets/abilities/iron_guardian_ground_slam.json` (E — POINT AoE, slow)

```json
{
    "id": "iron_guardian_ground_slam",
    "displayName": "Ground Slam",
    "slot": "E",
    "targeting": "POINT",
    "resourceType": "MANA",
    "costPerLevel": [50, 55, 60, 65, 70],
    "cooldownPerLevel": [10.0, 9.5, 9.0, 8.5, 8.0],
    "castTime": 0.4,
    "castRange": 700.0,
    "areaShape": "CIRCLE",
    "areaRadius": 3.5,
    "onHitEffects": [
        {
            "type": "DAMAGE",
            "damageType": "MAGICAL",
            "scaling": { "basePerLevel": [70, 120, 170, 220, 270], "apRatio": 0.6, "armorRatio": 0.3 }
        },
        { "type": "SLOW", "duration": 2.5, "value": 0.40 }
    ],
    "tags": ["damage", "magical", "slow", "aoe"]
}
```

**File**: `assets/abilities/iron_guardian_fortress.json` (R — SELF, massive shield + knockup)

```json
{
    "id": "iron_guardian_fortress",
    "displayName": "Unbreakable Fortress",
    "slot": "R",
    "targeting": "SELF",
    "resourceType": "MANA",
    "costPerLevel": [100, 100, 100],
    "cooldownPerLevel": [120.0, 100.0, 80.0],
    "castTime": 0.5,
    "channelDuration": 0.0,
    "areaShape": "CIRCLE",
    "areaRadius": 5.0,
    "onSelfEffects": [
        {
            "type": "SHIELD",
            "scaling": { "basePerLevel": [300, 500, 700], "hpRatio": 0.15, "hpBasis": "MAX" },
            "duration": 8.0
        },
        {
            "type": "BUFF_STAT",
            "statMod": { "stat": "ARMOR", "amount": 60, "percent": false },
            "duration": 8.0
        },
        {
            "type": "BUFF_STAT",
            "statMod": { "stat": "MAGIC_RESIST", "amount": 60, "percent": false },
            "duration": 8.0
        }
    ],
    "onHitEffects": [
        {
            "type": "DAMAGE",
            "damageType": "MAGICAL",
            "scaling": { "basePerLevel": [150, 275, 400], "apRatio": 0.5 }
        },
        { "type": "KNOCKUP", "duration": 1.0 }
    ],
    "tags": ["defense", "shield", "knockup", "aoe", "ultimate"]
}
```

---

## 5. Phase 4: Renderer Integration

### 5a. Structure Meshes

In `buildScene()`, create procedural meshes for structures:

```cpp
// Tower mesh: tall cylinder with a sphere on top
m_towerMeshIndex = m_scene.addMesh(Model::createCylinder(*m_device, m_device->getAllocator(), 1.0f, 4.0f));
m_towerTopMeshIndex = m_scene.addMesh(Model::createSphere(*m_device, m_device->getAllocator(), 16, 32));

// Inhibitor mesh: low torus (crystal-like)
m_inhibitorMeshIndex = m_scene.addMesh(Model::createTorus(*m_device, m_device->getAllocator(), 1.5f, 0.4f));

// Nexus mesh: large icosphere (imposing crystal)
m_nexusMeshIndex = m_scene.addMesh(Model::createIcosphere(*m_device, m_device->getAllocator(), 3, 2.0f));

// Monster meshes: reuse sphere for small mobs, icosphere for big monsters, gear for epic
m_monsterSmallMeshIndex = m_scene.addMesh(Model::createSphere(*m_device, m_device->getAllocator()));
m_monsterBigMeshIndex = m_scene.addMesh(Model::createIcosphere(*m_device, m_device->getAllocator(), 2, 0.8f));
m_monsterEpicMeshIndex = m_scene.addMesh(Model::createGear(*m_device, m_device->getAllocator(), 12, 1.2f, 0.3f, 0.5f));
```

New members in `Renderer.h`:

```cpp
// Structure system
StructureSystem m_structureSystem;
uint32_t m_towerMeshIndex     = 0;
uint32_t m_inhibitorMeshIndex = 0;
uint32_t m_nexusMeshIndex     = 0;

// Jungle system
JungleSystem m_jungleSystem;
uint32_t m_monsterSmallMeshIndex = 0;
uint32_t m_monsterBigMeshIndex   = 0;
uint32_t m_monsterEpicMeshIndex  = 0;
```

### 5b. Structure Entity Spawning

In `StructureSystem::init()`, for each tower from MapData:

```cpp
auto e = registry.create();
registry.emplace<TransformComponent>(e, TransformComponent{tower.position, {0,0,0}, {1.5f, 4.0f, 1.5f}});
registry.emplace<TowerTag>(e);
registry.emplace<StructureIdentityComponent>(e, StructureIdentityComponent{team, lane, tier});
registry.emplace<StructureHealthComponent>(e, StructureHealthComponent{cfg.maxHP, cfg.maxHP, ...});
registry.emplace<TowerAttackComponent>(e, TowerAttackComponent{cfg.attackDamage, cfg.attackRange, ...});
registry.emplace<BackdoorProtectionComponent>(e);
if (tier == TowerTier::Outer)
    registry.emplace<TowerPlateComponent>(e, ...);
```

Rendering components are assigned in `Renderer.cpp` (same pattern as minions):

```cpp
// In simulation loop, after structureSystem.init():
auto towerView = registry.view<TowerTag, TransformComponent>(entt::exclude<MeshComponent>);
for (auto e : towerView) {
    auto &id = registry.get<StructureIdentityComponent>(e);
    MeshComponent mc{m_towerMeshIndex};
    registry.emplace<MeshComponent>(e, mc);
    registry.emplace<MaterialComponent>(e, MaterialComponent{m_minionDefaultTex, m_minionFlatNorm, 0, 0, 0.4f, 0.0f});
    glm::vec4 tint = (id.team == TeamID::Blue)
        ? glm::vec4(0.2f, 0.4f, 0.9f, 1.0f)
        : glm::vec4(0.9f, 0.2f, 0.2f, 1.0f);
    registry.emplace<ColorComponent>(e, ColorComponent{tint});
    registry.emplace<TargetableComponent>(e, TargetableComponent{2.0f}); // towers are big targets
}
```

### 5c. Rendering Views (drawFrame MOBA section)

Add to the existing rendering loop (alongside character, projectile, map, minion views):

```cpp
// Structure entities (towers, inhibitors, nexus)
auto structView = registry.view<TransformComponent, MeshComponent, StructureIdentityComponent>();
for (auto entity : structView) {
    auto *hp = registry.try_get<StructureHealthComponent>(entity);
    if (hp && hp->isDead) continue;
    addEntityToGroups(entity);
}

// Jungle monster entities
auto monsterView = registry.view<TransformComponent, MeshComponent, JungleMonsterTag>();
for (auto entity : monsterView) {
    auto *hp = registry.try_get<MonsterHealthComponent>(entity);
    if (hp && hp->isDead) continue;
    addEntityToGroups(entity);
}

// Tower projectiles
auto towerProjView = registry.view<TransformComponent, MeshComponent, TowerProjectileComponent>();
for (auto entity : towerProjView) {
    addEntityToGroups(entity);
}
```

### 5d. Health Bar Extension

Modify `MinionHealthBars` → rename to `WorldHealthBars` or extend to support structures/monsters:

```cpp
void WorldHealthBars::draw(entt::registry &registry, const glm::mat4 &viewProj,
                           float screenW, float screenH, const glm::vec3 &cameraPos,
                           float maxRenderDist) {
    auto *drawList = ImGui::GetBackgroundDrawList();

    // Minion health bars (existing)
    drawMinionBars(registry, drawList, viewProj, screenW, screenH, cameraPos, maxRenderDist);

    // Structure health bars — always visible, larger bars
    drawStructureBars(registry, drawList, viewProj, screenW, screenH, cameraPos, 120.0f);

    // Monster health bars — similar to minion bars
    drawMonsterBars(registry, drawList, viewProj, screenW, screenH, cameraPos, maxRenderDist);
}
```

Structure health bars: 60x8 pixels (larger than minion bars), positioned 5 units above structure. Show plate markers as vertical ticks on the bar for outer towers.

### 5e. Simulation Integration

In `Renderer.cpp` fixed-timestep block:

```cpp
if (m_mobaMode) {
    m_gameTime += FIXED_DT;
    HeightQueryFn heightFn = ...;

    m_minionSystem.update(registry, FIXED_DT, m_gameTime, heightFn);
    m_structureSystem.update(registry, FIXED_DT, m_gameTime);
    m_jungleSystem.update(registry, FIXED_DT, m_gameTime, heightFn);
    m_autoAttackSystem.update(registry, m_minionSystem, FIXED_DT);

    // Process structure deaths (for inhibitor → super minion logic)
    auto structDeaths = m_structureSystem.consumeDeathEvents();
    for (auto &ev : structDeaths) {
        if (ev.type == EntityType::Inhibitor) {
            m_minionSystem.notifyInhibitorDestroyed(ev.team, ev.lane);
        }
        if (ev.type == EntityType::Nexus) {
            spdlog::info("GAME OVER! {} wins!", ev.team == TeamID::Blue ? "Red" : "Blue");
        }
    }

    // Assign render components to newly spawned structures/monsters...
}
```

### 5f. Selection Circle Extension

Extend the TargetingSystem to pick structures and monsters too:

```cpp
entt::entity TargetingSystem::pickTarget(entt::registry &registry,
                                         const glm::vec3 &rayOrigin,
                                         const glm::vec3 &rayDir,
                                         float maxDist) {
    // Existing: minions
    // Add: structures with TargetableComponent
    // Add: monsters with TargetableComponent
    // All use same ray-sphere intersection test
    // Return closest hit across all categories
}
```

---

## 6. Phase 5: Build System & Tests

### 6a. CMakeLists.txt Additions

```cmake
src/structure/StructureConfig.cpp
src/structure/StructureSystem.cpp
src/jungle/JungleConfig.cpp
src/jungle/JungleSystem.cpp
```

### 6b. Test Plan

**File**: `tests/test_structure.cpp`

```
Test 1: Tower takes damage and dies
Test 2: Protection dependency — inner tower immune while outer alive
Test 3: Protection dependency — inhibitor immune while inhib tower alive
Test 4: Nexus immune while nexus towers alive
Test 5: Tower targeting priority — minion before champion
Test 6: Tower targeting priority — champion attacking ally gets priority
Test 7: Damage ramp — consecutive hits increase damage by 40%
Test 8: Damage ramp resets on target switch
Test 9: Tower plates — each plate gives gold on break
Test 10: Plates removed after 14 min game time
Test 11: Backdoor protection — 66% reduction without minions nearby
Test 12: Backdoor disabled when minions in range
Test 13: Inhibitor respawns after 300s
Test 14: Nexus regen when out of combat
Test 15: Game over on nexus destruction
```

**File**: `tests/test_jungle.cpp`

```
Test 1: Camp spawns at configured time
Test 2: Monster takes damage and dies
Test 3: Death grants gold/XP to killer
Test 4: Buff granted on buff camp kill (red/blue)
Test 5: Monster leashes back when pulled too far
Test 6: Monster heals to full when resetting
Test 7: Camp respawns after respawn timer
Test 8: Scuttler flees when attacked
Test 9: Epic monster has correct scaled stats
Test 10: Monster aggro clears on reset
```

---

## 7. Performance Budget

### CPU Cost Per Frame (30 Hz, worst case)

| System | Entities | Operations | Est. Cost |
|---|---|---|---|
| StructureSystem::updateTargeting | 22 towers | 22 × ~40 range checks | ~2 us |
| StructureSystem::updateCombat | ~5 attacking towers | 5 × damage calc | <1 us |
| StructureSystem::updateProjectiles | ~5 projectiles | 5 × move + collision | <1 us |
| StructureSystem::updateBackdoor | 22 towers | 22 × minion proximity check | ~3 us |
| StructureSystem::updateInhibRespawn | 6 inhibitors | 6 × timer decrement | <1 us |
| JungleSystem::updateAggro | ~30 monsters | 30 × distance to player | <1 us |
| JungleSystem::updateCombat | ~5 attacking | 5 × damage calc | <1 us |
| JungleSystem::updateMovement | ~30 monsters | 30 × position update | <1 us |
| Health bar projection | ~60 entities | 60 × matrix multiply | ~5 us |
| **Total new CPU** | — | — | **~15 us** |

Current frame budget at 30 Hz = 33.3 ms. Adding 15 us = **0.045% of frame budget**. Negligible.

### GPU Cost

| Metric | Added | Impact |
|---|---|---|
| New instanced indirect draws | ~6 (tower/inhib/nexus/monsters × mesh groups) | +6 draw calls |
| New vertex data | ~200 KB (7 procedural meshes) | Negligible |
| New instance data | ~60 entities × 176 B = ~10.5 KB | Within existing buffer |
| New pipelines/shaders | 0 | Zero overhead |
| Health bar ImGui rects | +60 entities × 3 rects = 180 rects | Batched in single ImGui draw |

### Memory Cost

| Resource | Size |
|---|---|
| StructureComponents (30 entities) | ~4 KB |
| JungleComponents (30 entities) | ~3 KB |
| Config data (JSON parsed) | ~2 KB |
| Procedural meshes (7 new) | ~200 KB GPU |
| **Total** | **~209 KB** |

---

## 8. Implementation Order

Dependencies flow top-to-bottom. Items at the same level can be parallelized.

```
Step 1: Components (no dependencies)
    ├── src/structure/StructureComponents.h
    ├── src/jungle/JungleComponents.h
    └── src/scene/Components.h (add TargetedProjectileTag, DashComponent)

Step 2: Config loaders (depend on Components)
    ├── assets/config/structure_config.json
    ├── src/structure/StructureConfig.h + .cpp
    ├── assets/config/monster_config.json
    └── src/jungle/JungleConfig.h + .cpp

Step 3: Core systems (depend on Config + Components)
    ├── src/structure/StructureSystem.h + .cpp
    └── src/jungle/JungleSystem.h + .cpp

Step 4: Ability expansion (independent of Step 3)
    ├── ProjectileSystem: add spawnTargeted()
    ├── EffectSystem: implement DASH/BLINK
    ├── assets/abilities/frost_archer_*.json (4 files)
    └── assets/abilities/iron_guardian_*.json (4 files)

Step 5: Renderer integration (depends on Steps 1-3)
    ├── Renderer.h: add system members + mesh indices
    ├── Renderer.cpp buildScene(): create meshes, init systems
    ├── Renderer.cpp drawFrame() simulation: call system updates
    ├── Renderer.cpp drawFrame() rendering: add structure/monster views
    ├── MinionHealthBars → extend for structures + monsters
    └── TargetingSystem: extend for structures + monsters

Step 6: Build & Wire
    ├── CMakeLists.txt: add new source files
    └── Build + fix compilation errors

Step 7: Tests
    ├── tests/test_structure.cpp (15 tests)
    └── tests/test_jungle.cpp (10 tests)

Step 8: Polish & Verify
    ├── Run all tests (minion + structure + jungle)
    ├── Visual verification in-game
    └── Performance profiling
```

**Estimated implementation effort**: ~2500 lines of new code across 14 new files + ~200 lines of modifications to 5 existing files.

---

## 9. File Manifest

### New Files (14)

| Path | Purpose | ~Lines |
|---|---|---|
| `src/structure/StructureComponents.h` | ECS components for towers/inhibitors/nexus | 120 |
| `src/structure/StructureConfig.h` | Config structs | 60 |
| `src/structure/StructureConfig.cpp` | JSON loading | 120 |
| `src/structure/StructureSystem.h` | System class declaration | 70 |
| `src/structure/StructureSystem.cpp` | Core logic: targeting, combat, protection, death | 500 |
| `src/jungle/JungleComponents.h` | ECS components for monsters/camps | 100 |
| `src/jungle/JungleConfig.h` | Config structs | 60 |
| `src/jungle/JungleConfig.cpp` | JSON loading | 130 |
| `src/jungle/JungleSystem.h` | System class declaration | 50 |
| `src/jungle/JungleSystem.cpp` | Core logic: spawn, aggro, AI, combat, death | 450 |
| `assets/config/structure_config.json` | Tower/inhibitor/nexus balance data | 80 |
| `assets/config/monster_config.json` | Monster camp stats & rewards | 120 |
| `tests/test_structure.cpp` | 15 structure system unit tests | 350 |
| `tests/test_jungle.cpp` | 10 jungle system unit tests | 250 |

### New Ability JSON Files (8)

| Path | Champion | Slot | Type |
|---|---|---|---|
| `assets/abilities/frost_archer_volley.json` | Frost Archer | Q | Skillshot + Slow |
| `assets/abilities/frost_archer_hawk.json` | Frost Archer | W | Global reveal (piercing) |
| `assets/abilities/frost_archer_enchant.json` | Frost Archer | E | Self AS + AD buff |
| `assets/abilities/frost_archer_arrow.json` | Frost Archer | R | Global skillshot + Stun |
| `assets/abilities/iron_guardian_charge.json` | Iron Guardian | Q | Dash + Stun |
| `assets/abilities/iron_guardian_shield_wall.json` | Iron Guardian | W | Shield + AoE Taunt |
| `assets/abilities/iron_guardian_ground_slam.json` | Iron Guardian | E | AoE Damage + Slow |
| `assets/abilities/iron_guardian_fortress.json` | Iron Guardian | R | Massive Shield + Knockup |

### Modified Files (7)

| Path | Changes |
|---|---|
| `src/scene/Components.h` | Add `TargetedProjectileTag`, `DashComponent` |
| `src/ability/ProjectileSystem.h` | Add `spawnTargeted()` method |
| `src/ability/ProjectileSystem.cpp` | Implement targeted projectile homing + spawn |
| `src/ability/EffectSystem.cpp` | Implement DASH and BLINK effect types |
| `src/renderer/Renderer.h` | Add system members + mesh indices |
| `src/renderer/Renderer.cpp` | Init systems, create meshes, render views, health bars |
| `CMakeLists.txt` | Add 4 new `.cpp` source files |

### Optional Modifications

| Path | Changes |
|---|---|
| `src/hud/MinionHealthBars.h/.cpp` | Extend to render structure + monster health bars |
| `src/input/TargetingSystem.cpp` | Extend ray-sphere to include structures + monsters |
| `src/minion/MinionSystem.h` | Add `notifyInhibitorDestroyed(TeamID, LaneType)` |

---

## Appendix: Key Design Decisions

### Why a single StructureSystem instead of TowerSystem + InhibitorSystem + NexusSystem?

Towers, inhibitors, and nexus share 80% of their logic: health management, team identity, protection dependencies, death processing, visual feedback. Splitting them would duplicate code. The tag components (`TowerTag`, `InhibitorTag`, `NexusTag`) plus specialized components (`TowerAttackComponent`, `InhibitorComponent`, `NexusComponent`) provide type-specific behavior without separate systems.

### Why not reuse MinionCombatComponent for towers?

Towers have fundamentally different combat: static position, projectile-based attacks with damage ramp, protection dependencies, plates. Sharing `MinionCombatComponent` would require adding 6+ fields that are meaningless for minions. Separate components are cleaner and allow independent iteration.

### Why no spatial hash for tower targeting?

22 towers doing linear scans of ~40 enemies = 880 distance checks at 30 Hz. Each check is ~4 cycles. Total: ~3,500 cycles = 1.2 us. A spatial hash rebuild costs more than the scan itself. Spatial hashing pays off at >200 entities (like minions with 700-unit aggro range across a 200×200 map).

### Why JSON for monster stats instead of hardcoding?

Balance iteration. When tuning Baron from 12,000 HP to 13,000 HP, you change one number in a JSON file instead of recompiling. The MinionConfig pattern already proves this works well. Config loading at init is ~1 ms (one-time cost, not per-frame).

### Why extend MinionHealthBars instead of a new health bar system?

ImGui `GetBackgroundDrawList()` batches all rects into one draw call regardless of which function added them. A unified health bar renderer avoids duplicate projection math and keeps the ImGui rect count in a single batch. The only difference between structure bars and minion bars is size and Y-offset, which is trivially parameterized.

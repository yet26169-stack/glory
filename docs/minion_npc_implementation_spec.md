# MINION NPC SYSTEM

**Implementation Specification**
*For Custom Vulkan Game Engine — MOBA Genre*

Version 1.0 · March 2026
**CONFIDENTIAL — Internal Use Only**

---

## Table of Contents

1. [Overview](#1-overview)
2. [Minion Types and Stats](#2-minion-types-and-stats)
3. [Spawning System](#3-spawning-system)
4. [Pathfinding and Movement](#4-pathfinding-and-movement)
5. [Targeting and Aggro System](#5-targeting-and-aggro-system)
6. [Combat System](#6-combat-system)
7. [ECS Architecture and Components](#7-ecs-architecture-and-components)
8. [Vulkan Rendering Considerations](#8-vulkan-rendering-considerations)
9. [Data-Driven Configuration](#9-data-driven-configuration)
10. [Performance Budget and Optimization](#10-performance-budget-and-optimization)
11. [Edge Cases and Special Rules](#11-edge-cases-and-special-rules)
12. [Testing Checklist](#12-testing-checklist)
13. [Appendix A: State Machine Diagram](#appendix-a-state-machine-diagram)

---

## 1. Overview

This document specifies the design and implementation requirements for the Minion NPC system in our custom Vulkan-based MOBA game engine. The minion system follows the core gameplay patterns established by League of Legends, adapted for our engine architecture. The implementing agent should treat this as the authoritative reference for all minion-related behaviors, data structures, rendering, and AI logic.

### 1.1 Design Goals

- Faithful recreation of core MOBA minion mechanics (spawning, pathing, targeting, combat)
- High-performance implementation suitable for Vulkan's GPU-driven rendering pipeline
- Support for hundreds of concurrent minion entities with minimal CPU overhead
- Clean ECS (Entity Component System) integration with the existing engine architecture
- Data-driven configuration for easy balancing and iteration

### 1.2 Reference Behavior (League of Legends)

Minions in League of Legends are AI-controlled NPCs that spawn periodically from each team's base (Nexus) and march along predefined lanes toward the enemy base. They engage enemy minions, towers, and champions according to a strict aggro priority system. They serve as the primary source of gold and experience for players and are the backbone of lane control and map pressure.

---

## 2. Minion Types and Stats

The system must support four distinct minion archetypes. All stats below are base values at game time 0:00 and scale over time (see Section 2.2).

### 2.1 Base Stats Table

| Type        | HP   | AD  | Armor | MR | Speed | Range |
|-------------|------|-----|-------|----|-------|-------|
| Melee       | 477  | 12  | 0     | 0  | 325   | 110   |
| Caster      | 296  | 23  | 0     | 0  | 325   | 600   |
| Siege/Cannon| 900  | 40  | 65    | 0  | 325   | 300   |
| Super       | 2000 | 190 | 100   | 30 | 325   | 170   |

### 2.2 Stat Scaling

Minion stats should scale over game time to maintain relevance as the game progresses. Scaling is applied every 90 seconds (1.5 minutes) starting from the first spawn wave.

| Stat         | Melee | Caster | Siege | Super |
|--------------|-------|--------|-------|-------|
| HP / tick    | +21   | +14    | +50   | +200  |
| AD / tick    | +1.0  | +1.5   | +3.0  | +10   |
| Armor / tick | +2    | +1.25  | +3    | +5    |

**Formula:** `stat(t) = baseStat + (scalingPerTick * floor(gameTimeSeconds / 90))`

### 2.3 Wave Composition

Each wave spawns from the Nexus and splits into three lane-specific sub-waves. Standard wave composition:

- Every wave: 3 Melee minions + 1 Caster minion (per lane)
- Every 3rd wave (cannon wave): adds 1 Siege/Cannon minion
- After 20:00 game time: every 2nd wave includes a Cannon minion
- After 35:00 game time: every wave includes a Cannon minion
- Super minions replace Siege minions in a lane when the corresponding enemy inhibitor is destroyed

---

## 3. Spawning System

### 3.1 Spawn Timing

- First wave spawns at 1:05 game time
- Subsequent waves spawn every 30 seconds
- All minions in a wave spawn simultaneously at the Nexus spawn point
- Each team has its own spawn point (one per Nexus)

### 3.2 Spawn Points and Lane Assignment

Minions spawn at the Nexus and are immediately assigned to one of three lanes (Top, Mid, Bot). The lane assignment is fixed per minion instance and determines its pathfinding waypoint sequence. Spawn positions should be slightly offset per lane to avoid initial overlap.

### 3.3 Implementation Requirements

The spawning system should be implemented as a SpawnManager or equivalent system that:

1. Tracks game time and determines when to spawn the next wave
2. Reads wave composition from a data-driven config (JSON or similar)
3. Instantiates minion entities with the correct components (see Section 7)
4. Assigns lane and team affiliation
5. Handles inhibitor state to determine if Super minions should spawn

---

## 4. Pathfinding and Movement

### 4.1 Lane Waypoints

Each lane is defined by an ordered sequence of waypoints from the friendly Nexus to the enemy Nexus. Minions follow these waypoints in order. Waypoint data should be stored externally (e.g., in the map file or a `lane_paths.json` config).

**Waypoint structure (per lane, per team):**

```cpp
struct LaneWaypoint {
    vec3 position;       // World-space position
    float radius;        // Arrival threshold radius
    bool isTowerPoint;   // If true, minions may pause here for tower aggro
};
```

### 4.2 Movement Behavior

- Minions move toward the next waypoint in their assigned path at their movement speed
- When within the waypoint's arrival radius, advance to the next waypoint
- If an enemy target enters aggro range, the minion may deviate from the path to engage (see Section 5)
- After losing aggro, minions return to the nearest waypoint on their lane path and resume
- Use the engine's navmesh or pathfinding grid for obstacle avoidance when deviating from lane

### 4.3 Collision and Separation

Minions should use soft-body separation (steering behaviors) to avoid overlapping with each other. A simple separation force based on distance to nearby minions is sufficient. Do not use rigid body physics for minion-minion interactions as this is too expensive for the entity count.

**Recommended approach:**

- Use a spatial hash or grid for neighbor queries (O(1) per minion)
- Apply separation steering force if within separation radius (e.g., 50 units)
- Blend separation force with path-following force using weighted sum

---

## 5. Targeting and Aggro System

This is the most critical behavioral system for minions. The aggro priority list must be followed precisely to replicate authentic MOBA gameplay.

### 5.1 Aggro Range

- Detection/aggro range: 700 units from minion center
- Leash range: 900 units (minion drops target and resets if target moves beyond this)
- Aggro check frequency: every 250ms (do not check every frame)

### 5.2 Target Priority List (Descending Priority)

When a minion has no current target or is re-evaluating targets, it must select from valid enemies in aggro range using this strict priority order:

1. **Enemy champion attacking a nearby allied champion** — This is the "champion aggro draw" mechanic. If an enemy champion auto-attacks or uses a targeted ability on a friendly champion within the minion's aggro range, the minion immediately switches to target that enemy champion.
2. **Enemy minion attacking a nearby allied champion**
3. **Enemy minion attacking a nearby allied minion**
4. **Enemy turret attacking a nearby allied minion**
5. **Enemy champion attacking a nearby allied minion**
6. **Closest enemy minion**
7. **Closest enemy champion (only if no minion targets)**
8. **Closest enemy turret**

### 5.3 Champion Aggro Draw Rules

- When an enemy champion attacks a friendly champion within minion aggro range, all nearby minions of the attacked champion's team immediately target the aggressor
- This aggro lasts for a fixed duration (2–3 seconds) or until the champion leaves aggro range
- After the aggro timer expires, minions re-evaluate targets using the priority list
- Champion aggro draw has a cooldown of ~2 seconds per minion to prevent constant switching

### 5.4 Target Switching

Minions do not constantly re-evaluate targets. They stick to their current target unless:

- Current target dies
- Current target leaves leash range (900 units)
- A higher-priority target event occurs (e.g., champion aggro draw)
- Periodic re-evaluation timer fires (every 3–4 seconds)

---

## 6. Combat System

### 6.1 Auto-Attack Behavior

Once a minion has a valid target and is within attack range, it performs auto-attacks on a fixed timer.

| Type   | Attack Speed   | Attack Style       | Projectile Speed |
|--------|----------------|--------------------|------------------|
| Melee  | 1.25s cooldown | Melee (instant)    | N/A              |
| Caster | 1.6s cooldown  | Ranged (projectile)| 650 units/s      |
| Siege  | 2.0s cooldown  | Ranged (projectile)| 1200 units/s     |
| Super  | 0.85s cooldown | Melee (instant)    | N/A              |

### 6.2 Damage Calculation

**Physical damage formula:**

```cpp
float effectiveArmor = target.armor; // apply armor penetration if relevant
float damageMultiplier = 100.0f / (100.0f + effectiveArmor);
float finalDamage = attacker.attackDamage * damageMultiplier;
```

Magic resist follows the same formula but uses the target's MR stat. Minion attacks deal physical damage only.

### 6.3 Projectile System

For ranged minions (Caster and Siege), attacks spawn a projectile entity that travels toward the target. The projectile must:

- Track the target's position (homing projectile)
- Apply damage on arrival/collision
- Be destroyed after impact or if the target dies mid-flight (projectile disappears)
- Have a visual representation (particle effect or small mesh) rendered via the Vulkan pipeline

### 6.4 Death and Rewards

When a minion's HP reaches zero:

- Play death animation (if available) or remove entity after a brief delay (0.5s)
- Award gold to the killing player if the last hit was from a champion
- Award experience to all nearby enemy champions within XP range (1600 units)
- Remove all associated components and free resources

**Gold values:**

| Type   | Last-Hit Gold       | Proximity XP |
|--------|---------------------|--------------|
| Melee  | 21g (+0.125g/min)   | 60 XP        |
| Caster | 14g (+0.125g/min)   | 30 XP        |
| Siege  | 60g (+0.35g/min)    | 93 XP        |
| Super  | 90g                 | 97 XP        |

---

## 7. ECS Architecture and Components

The minion system should integrate with the engine's existing ECS. Below are the required components. The implementing agent should adapt naming conventions and data types to match the project's existing patterns.

### 7.1 Core Components

**TransformComponent**

```cpp
struct TransformComponent {
    vec3 position;
    quat rotation;
    vec3 scale;
};
```

**MinionIdentityComponent**

```cpp
struct MinionIdentityComponent {
    MinionType type;     // MELEE, CASTER, SIEGE, SUPER
    TeamID team;         // BLUE, RED
    LaneID lane;         // TOP, MID, BOT
    uint32_t waveIndex;  // Which spawn wave this minion belongs to
};
```

**HealthComponent**

```cpp
struct HealthComponent {
    float currentHP;
    float maxHP;
    bool isDead;
};
```

**CombatStatsComponent**

```cpp
struct CombatStatsComponent {
    float attackDamage;
    float armor;
    float magicResist;
    float attackRange;
    float attackCooldown;   // seconds between attacks
    float timeSinceLastAttack;
};
```

**MovementComponent**

```cpp
struct MovementComponent {
    float moveSpeed;
    vec3 velocity;
    uint32_t currentWaypointIndex;
    bool isReturningToLane;
};
```

**AggroComponent**

```cpp
struct AggroComponent {
    EntityID currentTarget;         // NULL_ENTITY if no target
    float aggroRange;               // 700 units default
    float leashRange;               // 900 units default
    float timeSinceLastTargetEval;  // re-eval timer
    float championAggroCooldown;    // cooldown after champion draw
    float championAggroTimer;       // remaining forced champion target time
};
```

**RenderComponent**

```cpp
struct RenderComponent {
    MeshHandle mesh;
    MaterialHandle material;
    AnimationState animState;
    uint32_t instanceIndex;  // index into GPU instance buffer
};
```

### 7.2 Required Systems (Update Order)

1. **SpawnSystem** — Creates minion entities based on game timer and wave config
2. **AggroSystem** — Evaluates and assigns targets based on priority list
3. **MovementSystem** — Moves minions along lane paths or toward targets, applies separation
4. **CombatSystem** — Processes attacks, spawns projectiles, applies damage
5. **ProjectileSystem** — Updates projectile positions, checks for arrival/collision
6. **DeathSystem** — Handles death events, distributes gold/XP, queues cleanup
7. **StatScalingSystem** — Applies per-tick stat increases to all living minions
8. **RenderSystem** — Updates GPU instance buffers, submits draw commands via Vulkan

---

## 8. Vulkan Rendering Considerations

### 8.1 Instanced Rendering

Minions of the same type and team should be rendered using instanced draw calls to minimize Vulkan command buffer overhead. Each minion type/team combination constitutes one draw call, with per-instance data (transform, animation frame, health bar data) stored in a GPU-side storage buffer (SSBO).

**Per-instance GPU data:**

```cpp
struct MinionInstanceData {
    mat4 modelMatrix;
    vec4 colorTint;        // team color tint
    float healthPercent;   // for health bar rendering
    uint32_t animFrame;    // current animation frame index
    uint32_t flags;        // bit flags (visible, selected, etc.)
    float padding;
};
```

### 8.2 GPU Buffer Management

- Pre-allocate instance buffers for the maximum expected minion count per type (e.g., 128 per type per team)
- Use a ring buffer or double-buffering strategy to update instance data without stalling the GPU
- Upload instance data once per frame after all ECS systems have run
- Use `VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT` for staging, then GPU copy if performance requires

### 8.3 Health Bars

Minion health bars should be rendered as screen-space quads above each minion. Two approaches are acceptable:

- Geometry shader or vertex shader billboarding in a dedicated health bar render pass
- UI overlay pass that reads health data from the instance SSBO and renders 2D quads

Health bars should use team colors (blue for friendly, red for enemy) and fade/disappear when health is full.

### 8.4 LOD and Culling

- Implement frustum culling per-minion before adding to instance buffer
- Optional: 2-level LOD (full mesh within 2000 units, simplified mesh beyond)
- Minions beyond a configurable max render distance should not be submitted for drawing

---

## 9. Data-Driven Configuration

All minion parameters should be externalized to JSON (or equivalent) configuration files to allow designers to iterate without code changes.

### 9.1 Required Config Files

- **`minion_stats.json`** — Contains base stats, scaling values, and attack parameters for each minion type.
- **`wave_config.json`** — Defines wave composition rules, spawn timings, and cannon wave thresholds.
- **`lane_paths.json`** — Waypoint sequences for each lane on each map.
- **`aggro_config.json`** — Aggro ranges, leash distances, evaluation intervals, priority weights.
- **`rewards_config.json`** — Gold values, XP values, scaling per game minute.

### 9.2 Hot-Reload Support

The config system should support runtime hot-reloading during development. Watch the config files for changes and re-apply values to the relevant ECS components without requiring a full restart. This is critical for rapid balancing iteration.

---

## 10. Performance Budget and Optimization

### 10.1 Performance Targets

| Metric                      | Budget                                      |
|-----------------------------|---------------------------------------------|
| Max concurrent minions      | ~200 (typical: ~80-120)                     |
| AI update (all minions)     | < 0.5ms per frame                           |
| Movement + collision        | < 0.3ms per frame                           |
| Instance buffer upload      | < 0.1ms per frame                           |
| Draw calls for all minions  | < 16 (4 types x 2 teams x 2 LODs max)      |

### 10.2 Optimization Strategies

- **Spatial hashing:** Use a uniform grid (cell size = aggro range) for O(1) neighbor queries in the aggro system
- **Staggered AI updates:** Not all minions need aggro re-evaluation on the same frame. Distribute across frames
- **SIMD:** Vectorize damage calculations and distance checks where possible
- **Object pooling:** Pre-allocate minion entities and components. Reuse dead minion slots for new spawns
- **Avoid per-minion Vulkan resources:** All minions of the same type share the same pipeline, descriptor sets, and mesh buffers

---

## 11. Edge Cases and Special Rules

### 11.1 Tower Interaction

- Minions that reach an enemy tower without enemy minions present will aggro the tower
- Towers prioritize targeting minions over champions (unless champion aggro draw occurs)
- Minions under tower should continue attacking the tower until they die or are redirected by aggro rules

### 11.2 Inhibitor Destruction

- When a team's inhibitor is destroyed, the opposing team spawns Super minions in that lane instead of Siege minions
- Inhibitors respawn after 5 minutes; Super minion spawning in that lane stops when the inhibitor respawns
- If all 3 inhibitors are down, all lanes spawn 2 Super minions per wave

### 11.3 Game Pause and Reconnect

The minion system must properly handle game pauses (if supported) by freezing all timers including spawn timers, attack cooldowns, and aggro evaluation timers. On disconnect/reconnect, the minion state should be fully deterministic and reconstructable from the authoritative game state.

### 11.4 Fountain / Base Gate

If the map design includes base gates or fountain areas, minions should path through them freely (gates open for minions) but should never target or attack allied structures.

---

## 12. Testing Checklist

The implementing agent should verify all of the following before considering the minion system complete:

1. Minions spawn at correct timing (first wave at 1:05, then every 30s)
2. Correct wave composition (3 melee + 1 caster, cannon every 3rd wave)
3. Minions follow lane waypoints to the enemy base
4. Minions engage enemies in correct priority order
5. Champion aggro draw works (attack enemy champ near minions → minions switch to you)
6. Minions return to lane path after losing target
7. Ranged minion projectiles track and hit targets correctly
8. Gold and XP awarded correctly on last-hit and proximity
9. Stats scale correctly over game time
10. Super minions spawn when inhibitor is destroyed
11. No minion-minion overlap/stacking (separation steering works)
12. Performance within budget (≤ 200 minions at < 1ms total CPU)
13. Instanced rendering works correctly (all minions visible, correct team colors)
14. Health bars display correctly and update in real-time
15. Config hot-reload works during development

---

## Appendix A: State Machine Diagram

The minion AI can be modeled as a finite state machine with the following states:

```
SPAWNING  →  PATHING  →  ENGAGING  →  ATTACKING
                ↑           |            |
                |           v            |
             RETURNING  ←  CHASING      |
                ↑                        |
                └────────────────────────┘
                                         |
                                     DYING → DEAD (entity recycled)
```

**State Descriptions:**

- **SPAWNING:** Entity just created, playing spawn animation, not yet active
- **PATHING:** Following lane waypoints toward enemy base, no target acquired
- **ENGAGING:** Target acquired, moving toward target if out of attack range
- **ATTACKING:** Within attack range of target, performing auto-attacks
- **CHASING:** Target moved out of attack range but still in leash range, pursuing
- **RETURNING:** Target lost (died or left leash range), navigating back to nearest lane waypoint
- **DYING:** HP reached 0, playing death animation, distributing rewards
- **DEAD:** Ready for entity pool recycling

---

*End of Document*

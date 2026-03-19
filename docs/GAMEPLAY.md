# Glory Engine — Gameplay Systems

> **Sources:** ABILITIES, TOWERS_MONSTERS_ABILITIES_PLAN, UNIT_SYSTEM_PLAN, MAP_PLAN, minion_npc_implementation_spec, HUD_AND_CLICK_INDICATOR_PLAN, CHARACTER_TEST, PLAN_GLB_MAP

---

## 1. Ability System

### 1.1 Architecture Overview

The ability system sits at the intersection of multiple engine subsystems. Systems are listed in dependency order for initialization and per-frame updates.

| Layer | System | Responsibility |
|-------|--------|---------------|
| 0 | InputSystem | Capture key presses (Q/W/E/R), mouse position, target selection |
| 1 | AbilitySystem | Process requests, validate conditions, trigger execution phases |
| 2 | TargetingSystem | Resolve targets (skillshot ray, AoE circle, point-and-click) |
| 3 | EffectSystem | Apply damage, healing, CC, buffs/debuffs to resolved targets |
| 4 | StatusEffectSystem | Manage active buffs/debuffs, tick DoTs/HoTs, handle expiration |
| 5 | ProjectileSystem | Move projectile entities, check collision, trigger on-hit effects |
| 6 | CooldownSystem | Decrement cooldown timers, integrate CDR stat |
| 7 | VFX Bridge | Queue particle spawn/destroy commands for Vulkan renderer |
| 8 | AnimationSystem | Trigger cast animations, channel syncs |
| 9 | AudioSystem | Play ability sound effects based on VFX events |

**Per-frame update order:**
1. `InputSystem::update()` — capture and enqueue ability requests
2. `CooldownSystem::update(dt)` — tick all cooldown timers
3. `StatusEffectSystem::update(dt)` — tick buffs/debuffs, remove expired
4. `AbilitySystem::update()` — process queued requests through state machine
5. `TargetingSystem::resolve()` — resolve targets for active abilities
6. `ProjectileSystem::update(dt)` — move projectiles, check collisions
7. `EffectSystem::apply()` — apply all pending effects (damage, heal, CC)
8. `VFXEventQueue::flush()` — dispatch VFX commands to Vulkan renderer
9. `AnimationSystem::update(dt)` — advance ability animations
10. `AudioSystem::update()` — play triggered sounds

### 1.2 Ability State Machine

```
READY ──(key + validation)──► CASTING
CASTING ──(timer + no channel)──► EXECUTING
CASTING ──(timer + channel)──► CHANNELING
CHANNELING ──(elapsed)──► EXECUTING
CHANNELING ──(hard CC / cancel)──► INTERRUPTED
EXECUTING ──(effects dispatched)──► ON_COOLDOWN
INTERRUPTED ──(75% cooldown)──► ON_COOLDOWN
ON_COOLDOWN ──(timer ≤ 0)──► READY
```

**State transition details:**

| From | To | Trigger | Action |
|------|----|---------|----|
| READY | CASTING | Player presses key, all checks pass | Deduct resource, begin cast timer, play cast anim + VFX |
| CASTING | CHANNELING | castTime elapsed AND channelDuration > 0 | Begin channel, lock movement if needed |
| CASTING | EXECUTING | castTime elapsed AND channelDuration == 0 | Resolve targets, spawn projectiles, apply instant effects |
| CHANNELING | EXECUTING | channelDuration elapsed | Fire final effect payload |
| CHANNELING | INTERRUPTED | Hard CC applied OR player cancels | Cancel channel, enter partial cooldown (75%) |
| EXECUTING | ON_COOLDOWN | All effects dispatched | `cooldown = baseCooldown * (1 - CDR)` |
| INTERRUPTED | ON_COOLDOWN | Immediate | Start reduced cooldown |
| ON_COOLDOWN | READY | `cooldownRemaining <= 0` | Ability available again |

### 1.3 Pre-Cast Validation

Before a cast begins, `AbilitySystem` validates:
1. Ability is learned (`level > 0`)
2. Ability is `READY` phase and `cooldownRemaining == 0`
3. `StatusEffectsComponent::canCast()` — not stunned / silenced / suppressed
4. `ResourceComponent::current >= costPerLevel[level-1]`
5. Target is valid, in range, and not obstructed by terrain

### 1.4 Core Data Structures

#### AbilityDefinition (data-driven, from JSON)
```cpp
struct AbilityDefinition {
    std::string         id;               // "fire_mage_fireball"
    std::string         displayName;      // "Fireball"
    AbilitySlot         slot;             // Q, W, E, R, PASSIVE, SUMMONER
    TargetingType       targeting;        // SKILLSHOT, POINT, SELF, TARGETED, VECTOR, NONE

    ResourceType        resourceType;     // MANA, ENERGY, HEALTH, RAGE, NONE
    std::array<float,5> costPerLevel;
    std::array<float,5> cooldownPerLevel;

    float               castTime;         // 0 = instant cast
    float               channelDuration;  // 0 = not a channel
    bool                canMoveWhileCasting;
    bool                canBeInterrupted;

    float               castRange;
    AreaShape           areaShape;        // NONE, CIRCLE, CONE, RECTANGLE, LINE, RING
    float               areaRadius;
    float               areaConeAngle;
    float               areaWidth;
    float               areaLength;

    ProjectileDef       projectile;
    std::vector<EffectDef> onHitEffects;
    std::vector<EffectDef> onSelfEffects;

    std::string         castAnimation;
    std::string         castVFX;
    std::string         projectileVFX;
    std::string         impactVFX;
    std::string         castSFX;
    std::string         impactSFX;

    std::unordered_set<std::string> tags; // "damage", "cc", "mobility", "ultimate"
};
```

#### EffectDef
```cpp
struct EffectDef {
    EffectType  type;       // DAMAGE, HEAL, SHIELD, STUN, SLOW, ROOT, KNOCKBACK,
                            // SILENCE, SUPPRESS, BLIND, BUFF_STAT, DEBUFF_STAT, DOT, HOT, DASH, BLINK
    DamageType  damageType; // PHYSICAL, MAGICAL, TRUE
    ScalingFormula scaling;
    float       duration;   // for CC / buffs / debuffs / dots
    float       tickRate;   // for DoTs/HoTs
    StatModifier statMod;
    float       value;      // slow %, knockback distance, etc.
    bool        appliesGrievousWounds;
    std::string applyVFX;
};
```

#### ScalingFormula
```cpp
struct ScalingFormula {
    std::array<float,5> basePerLevel;  // [60, 100, 140, 180, 220]
    float adRatio;       // 0.0–2.0
    float apRatio;       // 0.0–2.0
    float hpRatio;       // % of max/bonus/current HP
    HPScalingBasis hpBasis; // MAX, BONUS, CURRENT, MISSING
    float armorRatio;
    float mrRatio;
    // totalDamage = basePerLevel[level] + (AD * adRatio) + (AP * apRatio) + ...
};
```

#### ProjectileDef
```cpp
struct ProjectileDef {
    float speed;           // units per second
    float width;           // collision hitbox width
    float maxRange;        // despawn distance
    bool  piercing;        // passes through targets
    int   maxTargets;      // -1 = unlimited if piercing
    bool  returnsToSource; // boomerang-style
    float curveAngle;      // 0 = straight, >0 = arced
    bool  destroyOnWall;
};
```

#### Runtime ECS Components
```cpp
// Attached to every character entity
struct AbilityBookComponent {
    std::array<AbilityInstance, 6> abilities; // Q, W, E, R, PASSIVE, SUMMONER
};

struct AbilityInstance {
    const AbilityDefinition* def;
    int                      level;
    float                    cooldownRemaining;
    AbilityPhase             currentPhase;
    float                    phaseTimer;
    TargetInfo               currentTarget;
};

struct TargetInfo {
    TargetingType type;
    EntityID      targetEntity;
    glm::vec3     targetPosition;
    glm::vec3     direction;
};

struct StatusEffectsComponent {
    std::vector<ActiveStatusEffect> activeEffects;
};

struct ActiveStatusEffect {
    const EffectDef* def;
    EntityID         sourceEntity;
    float            remainingDuration;
    float            tickAccumulator;
    float            totalValue;  // pre-computed at apply time
};
```

### 1.5 Damage Formula

```cpp
// Standard MOBA armor/MR formula
float CalculateDamage(float rawDamage, DamageType type, const Stats& target) {
    if (type == DamageType::TRUE) return rawDamage;

    float resistance = (type == DamageType::PHYSICAL)
        ? target.armor : target.magicResist;

    float effectiveResist = resistance * (1.0f - percentPen) - flatPen;
    effectiveResist = std::max(0.0f, effectiveResist);

    float multiplier = 100.0f / (100.0f + effectiveResist);
    return rawDamage * multiplier;
}
```

### 1.6 Crowd Control Hierarchy

| Priority | CC Type | Prevents | Notes |
|----------|---------|----------|-------|
| 1 (highest) | Suppress | Everything | Cannot be cleansed; caster also locked |
| 2 | Stun | Move, Cast, Attack | Can be cleansed; reduced by tenacity |
| 3 | Knockup | Move, Cast, Attack | Not reduced by tenacity; physics displacement |
| 4 | Root | Movement only | Can still cast and attack |
| 5 | Silence | Ability casts | — |
| 6 | Slow | Reduces speed | Multiplicative stacking |

### 1.7 StatusEffect Lifecycle

```cpp
void StatusEffectSystem::update(float dt) {
    for (auto& [entity, statusComp] : view<StatusEffectsComponent>()) {
        auto it = statusComp.activeEffects.begin();
        while (it != statusComp.activeEffects.end()) {
            it->remainingDuration -= dt;
            if (it->def->tickRate > 0) {
                it->tickAccumulator += dt;
                while (it->tickAccumulator >= it->def->tickRate) {
                    it->tickAccumulator -= it->def->tickRate;
                    applyTickEffect(entity, *it);
                }
            }
            if (it->remainingDuration <= 0.0f) {
                removeEffectVisuals(entity, *it);
                it = statusComp.activeEffects.erase(it);
            } else { ++it; }
        }
    }
}
```

### 1.8 Targeting Types

| Type | Input Required | Resolution Logic |
|------|---------------|-----------------|
| SKILLSHOT | Direction from caster | Spawn projectile along direction; detect hits via collision |
| POINT | Ground position | Gather all entities within AoE shape at target position |
| TARGETED | Enemy entity under cursor | Validate entity: enemy, alive, in range, visible; auto-face |
| SELF | None | Apply effects to caster; optionally resolve AoE around caster |
| VECTOR | Start + end point | Define a line/cone from start to end; resolve entities |
| NONE | Automatic (passive) | Triggered by game events (on-hit, on-kill, etc.) |

### 1.9 JSON Ability Example

```json
{
  "id": "fire_mage_fireball",
  "displayName": "Fireball",
  "slot": "Q",
  "targeting": "SKILLSHOT",
  "resourceType": "MANA",
  "costPerLevel": [60, 65, 70, 75, 80],
  "cooldownPerLevel": [8.0, 7.5, 7.0, 6.5, 6.0],
  "castTime": 0.25,
  "projectile": {
    "speed": 1200.0,
    "width": 60.0,
    "maxRange": 1100.0,
    "piercing": false
  },
  "onHitEffects": [
    {
      "type": "DAMAGE",
      "damageType": "MAGICAL",
      "scaling": {
        "basePerLevel": [80, 130, 180, 230, 280],
        "apRatio": 0.75
      }
    }
  ],
  "castVFX": "vfx_fireball_cast",
  "projectileVFX": "vfx_fireball_projectile",
  "impactVFX": "vfx_fireball_explosion"
}
```

### 1.10 Networking Considerations

```cpp
// Client → Server
struct AbilityRequestMsg {
    EntityID    casterID;
    AbilitySlot slot;
    TargetInfo  target;
    uint32_t    sequenceNumber;
    float       clientTimestamp;
};

// Server → All Clients
struct ProjectileSpawnMsg {
    uint32_t    projectileID;
    std::string abilityID;
    glm::vec3   position;
    glm::vec3   direction;
    float       speed;
};
```

Thread safety:
- Keep game logic on a single thread
- Use `VFXEventQueue` (SPSC ring buffer) as the only bridge to render thread
- Server is the final arbiter for all ability execution and projectile spawning
- Lua (via sol2) available for complex ability overrides that can't be expressed in JSON

### 1.11 Implementation Roadmap

| Weeks | Phase | Deliverables |
|-------|-------|-------------|
| 1–2 | Foundation | JSON parser, state machine, basic pre-cast validation |
| 3–4 | Targeting & Effects | Mouse-to-world raycasting, targeting indicators, damage/CC math |
| 5–6 | Projectiles & Advanced | Projectile lifecycle, status effect stacking |
| 7–8 | VFX & Polish | GPU particle integration, animation blending |
| 9–10 | Passives & Networking | Event bus for passives, server validation |
| 11–12 | Full Champions | Complete kits for 3+ champions, HUD/UI finalization |

---

## 2. Unit System

### 2.1 Hero Units

**Components:**

```cpp
// Core unit identity
struct CharacterComponent {
    glm::vec3 targetPosition{0.0f};      // Destination from right-click
    float     moveSpeed = 6.0f;          // Units per second
    bool      hasTarget = false;
    glm::quat currentFacing{1.0f,0,0,0};
    float     currentSpeed = 0.0f;
};

// Selection
struct SelectableComponent {
    bool  isSelected = false;
    float selectionRadius = 1.0f;
};

// Combat stats (also drives HUD bars)
struct CombatStatsComponent {
    float currentHP;
    float maxHP;
    float shield;
    float currentMana;   // or energy/rage
    float maxMana;
    float attackDamage;
    float abilityPower;
    float armor;
    float magicResist;
    float attackSpeed;
    float moveSpeed;
    int   level;
    ResourceType resourceType; // MANA, ENERGY, HEALTH, RAGE, NONE
};
```

**Skeletal Animation Components:**

```cpp
struct SkeletonComponent {
    Skeleton skeleton;
    std::vector<std::vector<SkinVertex>> skinVertices;
    std::vector<std::vector<Vertex>> bindPoseVertices;
};

struct AnimationComponent {
    AnimationPlayer player;
    std::vector<AnimationClip> clips;   // [Idle=0, Walk=1, Attack=2, ...]
    int activeClipIndex = -1;
    std::vector<Vertex> skinnedVertices;
};

struct GPUSkinnedMeshComponent {
    uint32_t staticSkinnedMeshIndex = 0;
    uint32_t boneSlot = 0;             // 0..MAX_SKINNED_CHARS-1
};

struct SkinnedLODComponent {
    // LOD0: 10K tris  (≤20m)
    // LOD1:  3K tris  (≤60m)
    // LOD2:  700 tris (>60m)
};
```

### 2.2 Unit Selection System

**Marquee Selection (X=spawn, Left-Drag=select, Left-Click=command):**

Components:
```cpp
struct SelectionState {
    bool isDragging = false;
    glm::vec2 dragStart{0.0f};
    glm::vec2 dragEnd{0.0f};
    std::vector<entt::entity> selectedEntities;
};
```

Drawing the marquee (ImGui):
```cpp
if (m_selection.isDragging) {
    ImGui::GetForegroundDrawList()->AddRect(
        ImVec2(m_selection.dragStart.x, m_selection.dragStart.y),
        ImVec2(m_selection.dragEnd.x, m_selection.dragEnd.y),
        IM_COL32(0, 255, 0, 255), 0.0f, 0, 2.0f);
}
```

Screen-space selection check:
```cpp
void performSelection(const glm::vec2& start, const glm::vec2& end) {
    auto view = registry.view<TransformComponent, SelectableComponent>();
    glm::vec2 min = glm::min(start, end);
    glm::vec2 max = glm::max(start, end);
    for (auto entity : view) {
        auto& t = view.get<TransformComponent>(entity);
        auto& s = view.get<SelectableComponent>(entity);
        glm::vec2 screenPos = worldToScreen(t.position);
        s.isSelected = (screenPos.x >= min.x && screenPos.x <= max.x &&
                        screenPos.y >= min.y && screenPos.y <= max.y);
    }
}
```

### 2.3 Test Character Setup

The initial test character is a capsule entity at map center (100, 0, 100):

1. **Entity creation (`buildScene()`):** Create entity with `TransformComponent`, `CharacterComponent`, `GPUSkinnedMeshComponent`
2. **Right-click handling (`drawFrame()`):** `wasRightClicked()` → `screenToWorldRay()` → ray-plane intersection at Y=0 → set `character.targetPosition`
3. **Skinned model:** `better_scientist.glb` (decimated to ~10K tris via `meshoptimizer`)

Click-to-move verification steps:
1. Run app (starts in MOBA mode)
2. Character visible at map center on terrain
3. Right-click on terrain → character moves toward click position
4. Character smoothly rotates to face movement direction
5. Character height follows terrain (`TerrainSystem::GetHeightAt()`)
6. Character stops at target position (within 0.1 units)

### 2.4 Animation State Machine

```
Idle ──(hasTarget && dist > 0.1)──► Walk
Walk ──(!hasTarget || dist ≤ 0.1)──► Idle
Walk/Idle ──(ability cast)──► Cast
Cast ──(castTime elapsed)──► Walk or Idle
Any ──(death)──► Die
```

Crossfade blending — `AnimationPlayer`:
- Linear blend between two clips over configurable fade duration
- `player.crossfadeTo(clipIndex, fadeDuration)`
- Bone matrices interpolated at frame boundary

### 2.5 Hero Stats Reference

Base stats follow standard MOBA conventions (example: "Fire Mage"):

```cpp
CombatStatsComponent stats{
    .currentHP    = 580.0f,
    .maxHP        = 580.0f,
    .currentMana  = 340.0f,
    .maxMana      = 340.0f,
    .attackDamage = 52.0f,
    .abilityPower = 0.0f,
    .armor        = 21.0f,
    .magicResist  = 30.0f,
    .attackSpeed  = 0.625f,
    .moveSpeed    = 325.0f,
    .level        = 1,
    .resourceType = ResourceType::MANA,
};
```

---

## 3. NPC & Minion System

### 3.1 Minion Types and Stats

| Type | HP | AD | Armor | MR | Speed | Range |
|------|----|----|----|----|----|-----|
| Melee | 477 | 12 | 0 | 0 | 325 | 110 |
| Caster | 296 | 23 | 0 | 0 | 325 | 600 |
| Siege/Cannon | 900 | 40 | 65 | 0 | 325 | 300 |
| Super | 2000 | 190 | 100 | 30 | 325 | 170 |

**Stat Scaling** (every 90 seconds):

| Stat | Melee | Caster | Siege | Super |
|------|-------|--------|-------|-------|
| HP/tick | +21 | +14 | +50 | +200 |
| AD/tick | +1.0 | +1.5 | +3.0 | +10 |
| Armor/tick | +2 | +1.25 | +3 | +5 |

Formula: `stat(t) = baseStat + (scalingPerTick × floor(gameTimeSeconds / 90))`

**Wave Composition (per lane):**
- Every wave: 3 Melee + 1 Caster
- Every 3rd wave (cannon wave): + 1 Siege
- After 20:00: every 2nd wave has Cannon
- After 35:00: every wave has Cannon
- Super minions replace Siege after enemy inhibitor destroyed

### 3.2 Spawning System

```
First spawn:    1:05 game time
Wave interval:  30 seconds
Spawn point:    Nexus (per team)
Lane split:     Top / Mid / Bot simultaneously
```

`SpawnSystem` responsibilities:
1. Track game time, determine when to spawn next wave
2. Read wave composition from JSON config
3. Instantiate minion entities with correct components
4. Assign lane and team
5. Check inhibitor state → Super minion upgrade

### 3.3 Minion ECS Components

```cpp
struct MinionIdentityComponent {
    MinionType type;     // MELEE, CASTER, SIEGE, SUPER
    TeamID     team;     // BLUE, RED
    LaneID     lane;     // TOP, MID, BOT
    uint32_t   waveIndex;
};

struct HealthComponent {
    float currentHP;
    float maxHP;
    bool  isDead;
};

struct CombatStatsComponent {
    float attackDamage;
    float armor;
    float magicResist;
    float attackRange;
    float attackCooldown;
    float timeSinceLastAttack;
};

struct MovementComponent {
    float    moveSpeed;
    glm::vec3 velocity;
    uint32_t currentWaypointIndex;
    bool     isReturningToLane;
};

struct AggroComponent {
    EntityID currentTarget;
    float    aggroRange   = 700.0f;
    float    leashRange   = 900.0f;
    float    timeSinceLastTargetEval;
    float    championAggroCooldown;
    float    championAggroTimer;
};

struct RenderComponent {
    MeshHandle     mesh;
    MaterialHandle material;
    AnimationState animState;
    uint32_t       instanceIndex;
};
```

### 3.4 Aggro Priority List

When evaluating targets (checked every 250ms):

1. Enemy champion attacking a nearby allied champion within aggro range
2. Enemy minion attacking a nearby allied champion
3. Enemy minion attacking a nearby allied minion
4. Enemy turret attacking a nearby allied minion
5. Enemy champion attacking a nearby allied minion
6. Closest enemy minion
7. Closest enemy champion (only if no minion targets)
8. Closest enemy turret

**Champion aggro draw rules:**
- Enemy champion attacks friendly champion → all nearby minions target aggressor
- Aggro lasts 2–3 seconds or until out of range
- Per-minion cooldown of ~2 seconds after champion draw to prevent constant switching

### 3.5 Minion Combat

| Type | Attack Cooldown | Style | Projectile Speed |
|------|----------------|-------|-----------------|
| Melee | 1.25s | Melee (instant) | N/A |
| Caster | 1.6s | Ranged (projectile) | 650 units/s |
| Siege | 2.0s | Ranged (projectile) | 1200 units/s |
| Super | 0.85s | Melee (instant) | N/A |

Damage formula:
```cpp
float effectiveArmor = target.armor;
float damageMultiplier = 100.0f / (100.0f + effectiveArmor);
float finalDamage = attacker.attackDamage * damageMultiplier;
```

Death rewards:

| Type | Last-Hit Gold | Proximity XP |
|------|-------------|-------------|
| Melee | 21g (+0.125g/min) | 60 XP |
| Caster | 14g (+0.125g/min) | 30 XP |
| Siege | 60g (+0.35g/min) | 93 XP |
| Super | 90g | 97 XP |

XP range: 1600 units from minion; all nearby enemy champions within range receive XP.

### 3.6 Minion System Update Order

1. **SpawnSystem** — create entities from wave timer + config
2. **AggroSystem** — evaluate and assign targets
3. **MovementSystem** — move along lane / toward targets, apply separation
4. **CombatSystem** — process attacks, spawn projectiles, apply damage
5. **ProjectileSystem** — move projectiles, check arrival/collision
6. **DeathSystem** — death events, distribute gold/XP, queue cleanup
7. **StatScalingSystem** — per-tick stat increases
8. **RenderSystem** — update GPU instance buffers, submit draw commands

### 3.7 Rendering (Instanced)

```cpp
struct MinionInstanceData {
    glm::mat4 modelMatrix;
    glm::vec4 colorTint;        // team color tint
    float     healthPercent;    // for health bar rendering
    uint32_t  animationFrame;   // current skeleton pose index
    uint32_t  textureIndex;     // bindless texture array index
};
```

GPU skinning for minions:
- Switch from `DynamicMeshComponent` (CPU) to `GPUSkinnedMeshComponent` (GPU compute)
- `ComputeSkinner` dispatches `skinning.comp` per entity (local_size=64)
- Mesh decimation: `targetReduction=0.6f` in `GLBLoader` (27K → ~10K verts per minion)
- `MAX_SKINNED_CHARS = 128` (bumped from 32)

Performance target: 24 minions × ~0.05ms GPU compute = ~1.2ms total. CPU skinning: 0ms.

### 3.8 Structure System (Towers, Inhibitors, Nexus)

#### ECS Components

```cpp
// Identity tags
struct TowerTag {};
struct InhibitorTag {};
struct NexusTag {};

struct StructureIdentityComponent {
    TeamID    team;
    LaneType  lane;
    TowerTier tier;  // Outer, Inner, Inhibitor, Nexus
};

struct StructureHealthComponent {
    float currentHP       = 3500.0f;
    float maxHP           = 3500.0f;
    float hpRegen         = 0.0f;
    float outOfCombatTime = 0.0f;
    float outOfCombatThreshold = 8.0f;
    bool  isDead          = false;
    bool  isInvulnerable  = false;
};

struct TowerAttackComponent {
    float attackDamage       = 150.0f;
    float attackRange        = 15.0f;
    float attackCooldown     = 0.833f;   // ~1.2 attacks/sec
    float timeSinceLastAttack = 0.0f;
    float damageRampPercent  = 0.0f;    // +40% per consecutive hit
    float damageRampRate     = 0.40f;
    float maxDamageRamp      = 1.20f;   // cap at +120% (4 consecutive hits)
    entt::entity currentTarget = entt::null;
    float projectileSpeed    = 30.0f;
};

struct TowerPlateComponent {
    uint8_t platesRemaining  = 5;
    float   plateHP          = 0.0f;
    float   goldPerPlate     = 160.0f;
    float   armorPerPlate    = 0.40f;   // +40% damage reduction per plate
    float   plateFalloffTime = 840.0f;  // plates fall at 14:00
};

struct BackdoorProtectionComponent {
    float damageReduction      = 0.66f;
    float minionProximityRange = 20.0f;
    bool  isProtected          = true;
};

struct InhibitorComponent {
    float respawnTimer = 0.0f;
    float respawnTime  = 300.0f; // 5 minutes
    bool  isRespawning = false;
};

struct NexusComponent {
    float hpRegen = 5.0f;  // HP per second when out of combat
};

struct ProtectionDependencyComponent {
    entt::entity prerequisite = entt::null; // must be dead before this takes damage
};
```

#### Structure Stats (from JSON config)

| Tier | Max HP | AD | Armor | MR | Regen |
|------|--------|----|-------|----|-------|
| Outer | 5000 | 152 | 40 | 40 | — |
| Inner | 3500 | 170 | 55 | 55 | — |
| Inhibitor Tower | 3500 | 170 | 55 | 55 | — |
| Nexus Tower | 2700 | 180 | 65 | 65 | — |
| Inhibitor | 4000 | — | 20 | 20 | — |
| Nexus | 5500 | — | 0 | 0 | 5 HP/s |

#### Tower Targeting Priority (LoL-accurate)

1. Enemy champion attacking an allied champion within tower range
2. Enemy champion attacking an allied minion within tower range
3. Closest enemy minion within tower range
4. Closest enemy champion within tower range

Performance note: 22 towers × ~40 enemies = ~880 checks/frame at 30Hz = ~26,400 checks/sec. No spatial hash needed (static positions, small range). Linear scan: ~1.2µs total.

#### Protection Dependencies

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
    nexus_tower_0.prerequisite   = any inhibitor (all 3 inhib towers must be dead)
    nexus_tower_1.prerequisite   = any inhibitor
    nexus.prerequisite           = both nexus towers
```

O(1) structure lookup: `m_towers[2][3][4]` — indexed by `TeamID × LaneType × TowerTier`.

#### StructureSystem Interface

```cpp
class StructureSystem {
public:
    void init(const StructureConfig &config, const MapData &mapData,
              entt::registry &registry, HeightQueryFn heightFn = nullptr);
    void update(entt::registry &registry, float dt, float gameTime);

    std::vector<StructureDeathEvent> consumeDeathEvents();
    bool isInhibitorDead(TeamID team, LaneType lane) const;
    bool isGameOver() const { return m_gameOver; }
    TeamID getWinningTeam() const { return m_winningTeam; }
    entt::entity getTower(TeamID team, LaneType lane, TowerTier tier) const;
};
```

### 3.9 Jungle Monster System

#### Monster Types (from `monster_config.json`)

| Camp | Big Monster HP | AD | XP | Gold |
|------|---------------|----|----|------|
| Red Buff (Brambleback) | 1800 | 75 | 200 | 100 |
| Blue Buff (Sentinel) | 1800 | 75 | 200 | 100 |
| Wolves | 1300 | 42 | 135 | 68 |
| Raptors (6 mobs) | 800 (big) | 20 | 95 | 85 |
| Gromp | 1800 | 80 | 135 | 80 |
| Krugs (2 mobs) | 1350 | 60 | 168 | 100 |
| Scuttler | 1050 | 0 | 115 | 70 |

#### Jungle Monster Components

```cpp
struct JungleMonsterTag {};

struct MonsterIdentityComponent {
    CampType campType;
    uint32_t campIndex;
    uint32_t mobIndex;
    bool     isBigMonster;
};

struct MonsterAggroComponent {
    entt::entity currentTarget = entt::null;
    float aggroRange   = 5.0f;
    float leashRadius  = 0.0f;
    glm::vec3 homePosition{0.0f};
    float patience     = 0.0f;
    float patienceMax  = 8.0f;
    bool  isResetting  = false;
};

enum class MonsterState : uint8_t {
    Idle, Attacking, Chasing, Resetting, Dying, Dead
};

struct CampControllerComponent {
    uint32_t campIndex;
    CampType campType;
    float respawnTimer;
    float respawnTime;
    float initialSpawnTime;
    bool  isAlive;
    uint8_t aliveMobs;
    glm::vec3 position;
};
```

---

## 4. Map & Assets

### 4.1 Coordinate System

- **Right-handed, Y-up**
- X = Width (East/West), Z = Depth (North/South), Y = Height (Vertical)
- Map bounds: `(0, 0, 0)` to `(200, maxHeight, 200)`
- Map center: `(100, 0, 100)`
- Team 1 (Blue) base corner: near `(0, 0, 0)`
- Team 2 (Red) base corner: near `(200, 0, 200)`

### 4.2 Map Data Structures

```cpp
enum class TeamID : uint8_t { Blue = 0, Red = 1, Neutral = 2 };
enum class LaneType : uint8_t { Top = 0, Mid = 1, Bot = 2, Count = 3 };
enum class TowerTier : uint8_t { Outer = 0, Inner = 1, Inhibitor = 2, Nexus = 3 };
enum class CampType : uint8_t {
    RedBuff, BlueBuff, Wolves, Raptors, Gromp, Krugs,
    Scuttler, Dragon, Baron, Herald
};

struct Base {
    glm::vec3 nexusPosition;
    glm::vec3 spawnPlatformCenter;
    float     spawnPlatformRadius = 8.0f;
    glm::vec3 shopPosition;
};

struct Tower {
    glm::vec3                  position;
    std::optional<glm::vec3>   team2Override;
    LaneType                   lane;
    TowerTier                  tier;
    float                      attackRange  = 15.0f;
    float                      maxHealth    = 3500.0f;
    float                      attackDamage = 150.0f;
};

struct Inhibitor {
    glm::vec3                  position;
    std::optional<glm::vec3>   team2Override;
    LaneType                   lane;
    float                      maxHealth   = 4000.0f;
    float                      respawnTime = 300.0f;
};

struct Lane {
    LaneType               type;
    std::vector<glm::vec3> waypoints;  // Team 1 direction: base → enemy base
    float                  width = 12.0f;
};

struct NeutralCamp {
    glm::vec3              position;
    CampType               campType;
    float                  spawnTime   = 90.0f;
    float                  respawnTime = 300.0f;
    float                  leashRadius = 8.0f;
    std::vector<glm::vec3> mobPositions;
};

struct BrushZone {
    glm::vec3              center;
    glm::vec3              halfExtents;
    std::optional<glm::vec3> team2Override;
};
```

### 4.3 Map Symmetry

Data is defined once for Team 1 + neutrals. Engine mirrors Team 1 data to produce Team 2 with optional manual overrides via `team2Override`. Mirror utility: `MapSymmetry.h`.

### 4.4 GLB Map Loading

**Goal:** Replace procedural terrain with a pre-made 3D map GLB model (`cyberpunk+moba+map+model.glb` or `fantasy+arena+3d+model.glb`).

**Approach:** Load GLB as a regular scene entity rendered through the existing scene pipeline.

Scale/position calculation:
```
AABB from GLB: min(x,y,z) → max(x,y,z)
width  = max.x - min.x
depth  = max.z - min.z

scale      = 200.0 / max(width, depth)
position.x = 100.0 - ((min.x + width/2) * scale)
position.y = -(min.y * scale)
position.z = 100.0 - ((min.z + depth/2) * scale)
```

Axis-correction heuristic (Z-up detection):
```cpp
float height = bounds.max.y - bounds.min.y;
float zExtent = bounds.max.z - bounds.min.z;
bool isZUp = (height > zExtent * 2.0f);
if (isZUp) {
    mapT.rotation.x = glm::radians(-90.0f);  // -90° X rotation
    // Recompute width/depth using corrected axes
}
```

Entity creation in `buildScene()`:
```cpp
auto mapEntity = m_scene.createEntity("GLBMap");
m_scene.getRegistry().emplace<MeshComponent>(mapEntity, MeshComponent{mapMeshIdx});
m_scene.getRegistry().emplace<MaterialComponent>(
    mapEntity, MaterialComponent{mapTexIdx, flatNorm, 0.0f, 0.0f, 1.0f});
m_scene.getRegistry().emplace<ColorComponent>(
    mapEntity, ColorComponent{glm::vec4(1.0f)});  // white tint = show texture as-is
m_scene.getRegistry().emplace<MapComponent>(mapEntity);
```

### 4.5 Map Asset Files

**Directory:** `map models/` (122 MB total, 14 GLB files)

| File | Size | Purpose |
|------|------|---------|
| `arcane+tile+3d+model.glb` | 4.2 MB | Lane tile |
| `blue_team_tower_1.glb` | 4.6 MB | T1 tower (blue team) |
| `blue_team_tower_2.glb` | 4.5 MB | T2 tower (blue team) |
| `blue_team_tower_3.glb` | 5.2 MB | T3 tower (blue team) |
| `blue_team_inhib.glb` | 3.7 MB | Inhibitor (blue team) |
| `blue_team_nexus.glb` | 4.4 MB | Nexus (blue team) |
| `jungle_tile.glb` | 3.2 MB | Jungle terrain tile |
| `red_team_tower_1.glb` | 4.2 MB | T1 tower (red team) |
| `read_team_tower_2.glb` | 4.2 MB | T2 tower (red team) — ⚠️ TYPO: "read" |
| `red_team_tower3.glb` | 5.2 MB | T3 tower (red team) — ⚠️ no underscore |
| `red_team_inhib.glb` | 3.6 MB | Inhibitor (red team) |
| `red_team_nexus.glb` | 4.5 MB | Nexus (red team) |
| `river_tile.glb` | 4.2 MB | River terrain tile |
| `lower_quality_meshes.glb` | 4.0 MB | (Unused in buildScene) |

### 4.6 GLB Texture Pipeline

The texture pipeline extracts embedded GLB textures into the engine's bindless descriptor system.

**Full pipeline call chain:**
```
Renderer::buildScene()
├─ Model::loadFromGLB()              → returns Model{m_meshes[], m_meshMaterialIndices[]}
├─ Scene::addMesh(model)             → returns uint32_t meshIdx
├─ Model::loadGLBTextures()          → returns std::vector<GLBTexture>{materialIndex, Texture}
├─ Scene::addTexture(texture)        → returns uint32_t texIdx
├─ BindlessDescriptors::registerTexture()  → adds to descriptor array
├─ Scene::createEntity()
│   ├─ MeshComponent{meshIdx}
│   └─ MaterialComponent{texIdx, normalIdx, shininess, metallic, roughness, emissive}
└─ [Render Pass]
   ├─ GpuObjectData.texIndices = {texIdx, normalIdx, 0, 0}
   └─ Fragment shader: texture(textures[nonuniformEXT(fragDiffuseIdx)], uv)
```

**GLBTexture struct:**
```cpp
struct GLBTexture {
    int     materialIndex;  // glTF material index
    Texture texture;        // GPU-allocated Texture
};
```

**Current shader binding layout:**

| Binding | Set | Purpose | Format |
|---------|-----|---------|--------|
| 0 | 0 | Camera/transform UBO | Matrices |
| 2 | 0 | Light UBO | 4 lights, params |
| 3 | 0 | Shadow map | Depth texture |
| 5 | 0 | Toon ramp | 256×1 R8G8B8A8 |
| 6 | 0 | Fog of War | 512×512 R8 |
| 7 | 0 | Scene SSBO | GpuObjectData[] |
| 0 | 1 | Bindless textures | sampler2D[4096] |

### 4.7 Map Phases (from MAP_PLAN)

| Phase | Goal |
|-------|------|
| Phase 0 | Vulkan bootstrap: instance, device, swapchain, command buffers |
| Phase 1 | Data-driven map system: MapTypes.h, JSON loading, MapLoader |
| Phase 2 | Terrain + IsometricCamera |
| Phase 3 | Shading + Water |
| Phase 4 | Navigation + Spawning (Recast/Detour or waypoints) |
| Phase 5 | Fog of War |

---

## 5. HUD & UI

### 5.1 HUD Architecture

Dear ImGui renders on top of the Vulkan post-process pass (after tone-mapping). ImGui is already integrated via `DebugOverlay.h/cpp`.

**HUD rendering position in pipeline:**
1. Shadow pass
2. HDR scene pass (terrain, meshes, particles, debug lines)
3. Post-process pass (tone-map)
4. ImGui overlay ← HUD renders here

```cpp
// src/hud/HUD.h
class HUD {
public:
    void init(float screenWidth, float screenHeight);
    void resize(float screenWidth, float screenHeight);
    // Call once per frame between ImGui::NewFrame() and ImGui::Render()
    void draw(const Scene& scene, entt::entity player);

private:
    void drawHealthBar(const CombatStatsComponent& stats);
    void drawResourceBar(const CombatStatsComponent& stats);
    void drawAbilityBar(const AbilityBookComponent& book);
    void drawStatPanel(const CombatStatsComponent& stats);
    void drawXPBar(const CombatStatsComponent& stats);
    void drawMinimap();
    void drawKillFeed();
    void drawTooltip(const struct AbilityInstance& ability);

    float m_screenW = 1280.0f;
    float m_screenH = 720.0f;
};
```

### 5.2 HUD Layout

```
┌──────────────────────────────────────────────────────────────────┐
│  [Minimap placeholder]                              [KDA / Timer]│
│  (bottom-left, 180×180)                          (top-right)     │
│                                                                  │
│                        GAME VIEW                                 │
│                                                                  │
│  ┌──────────────────────────────────────────────────────────────┐│
│  │ [Portrait] [HP bar]  [═══ XP bar ═══]      [Stats]          ││
│  │            [MP bar]  [ Q ][ W ][ E ][ R ]  [AD/AP/ARM/MR]  ││
│  └──────────────────────────────────────────────────────────────┘│
└──────────────────────────────────────────────────────────────────┘
```

### 5.3 Health Bar

```cpp
void HUD::drawHealthBar(const CombatStatsComponent& stats) {
    float barW = 300.0f, barH = 22.0f;
    float x = (m_screenW - barW) * 0.5f - 80.0f;
    float y = m_screenH - 65.0f;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    // Background
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + barW, y + barH),
                      IM_COL32(20, 20, 20, 200), 4.0f);

    // Health fill (green > 50%, orange > 25%, red ≤ 25%)
    float frac = std::clamp(stats.currentHP / stats.maxHP, 0.0f, 1.0f);
    ImU32 hpColor = frac > 0.5f ? IM_COL32(50, 205, 50, 255)
                 : frac > 0.25f ? IM_COL32(255, 165, 0, 255)
                 : IM_COL32(220, 20, 20, 255);
    dl->AddRectFilled(ImVec2(x + 1, y + 1),
                      ImVec2(x + 1 + (barW - 2) * frac, y + barH - 1), hpColor, 3.0f);

    // Shield overlay
    if (stats.shield > 0.0f) {
        float shieldFrac = std::clamp(stats.shield / stats.maxHP, 0.0f, 1.0f - frac);
        float shieldStart = x + 1 + (barW - 2) * frac;
        dl->AddRectFilled(ImVec2(shieldStart, y + 1),
                          ImVec2(shieldStart + (barW - 2) * shieldFrac, y + barH - 1),
                          IM_COL32(200, 200, 200, 180), 3.0f);
    }

    // Text: "577 / 580"
    char buf[32];
    snprintf(buf, sizeof(buf), "%.0f / %.0f", stats.currentHP, stats.maxHP);
    ImVec2 textSize = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(x + (barW - textSize.x) * 0.5f, y + 2), IM_COL32_WHITE, buf);
}
```

### 5.4 Ability Bar

```cpp
void HUD::drawAbilityBar(const AbilityBookComponent& book) {
    const float iconSize = 48.0f;
    const float spacing  = 8.0f;
    const float totalW   = 4 * iconSize + 3 * spacing;
    float startX = (m_screenW - totalW) * 0.5f;
    float y      = m_screenH - 120.0f;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const char* labels[] = {"Q", "W", "E", "R"};
    AbilitySlot slots[]  = {AbilitySlot::Q, AbilitySlot::W, AbilitySlot::E, AbilitySlot::R};

    for (int i = 0; i < 4; ++i) {
        float x = startX + i * (iconSize + spacing);
        const auto& ab = book.get(slots[i]);

        ImU32 bgColor = (ab.level > 0) ? IM_COL32(40, 40, 60, 220) : IM_COL32(30, 30, 30, 180);
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + iconSize, y + iconSize), bgColor, 6.0f);

        // Cooldown sweep overlay
        if (ab.cooldownRemaining > 0.0f && ab.def) {
            float cdFrac = ab.cooldownRemaining / ab.def->cooldown;
            float overlayH = iconSize * cdFrac;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + iconSize, y + overlayH),
                              IM_COL32(0, 0, 0, 160), 6.0f);
            char cd[8];
            snprintf(cd, sizeof(cd), "%.1f", ab.cooldownRemaining);
            ImVec2 cdSize = ImGui::CalcTextSize(cd);
            dl->AddText(ImVec2(x + (iconSize - cdSize.x) * 0.5f,
                               y + (iconSize - cdSize.y) * 0.5f),
                        IM_COL32(255, 255, 255, 200), cd);
        }

        // Key label (gold if learned, gray if not)
        dl->AddText(ImVec2(x + 4, y + 2),
                    ab.level > 0 ? IM_COL32(255, 220, 80, 255) : IM_COL32(120, 120, 120, 200),
                    labels[i]);

        // Casting state border (gold while casting, gray idle)
        ImU32 borderColor = (ab.currentPhase != AbilityPhase::READY && ab.level > 0)
                          ? IM_COL32(255, 200, 50, 255) : IM_COL32(80, 80, 100, 255);
        dl->AddRect(ImVec2(x, y), ImVec2(x + iconSize, y + iconSize), borderColor, 6.0f, 0, 2.0f);

        // Hover tooltip
        ImVec2 mouse = ImGui::GetMousePos();
        if (mouse.x >= x && mouse.x <= x + iconSize &&
            mouse.y >= y && mouse.y <= y + iconSize && ab.def) {
            drawTooltip(ab);
        }
    }
}
```

### 5.5 Stats Panel

```cpp
void HUD::drawStatPanel(const CombatStatsComponent& stats) {
    float x = m_screenW * 0.5f + 180.0f;
    float y = m_screenH - 120.0f;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 160, y + 96), IM_COL32(20, 20, 30, 200), 6.0f);
    // AD (orange), AP (blue), ARM (gold), MR (purple), AS (yellow), MS (green)
}
```

### 5.6 XP Bar

```cpp
void HUD::drawXPBar(const CombatStatsComponent& stats) {
    float barW = 340.0f, barH = 6.0f;
    float x = (m_screenW - barW) * 0.5f;
    float y = m_screenH - 128.0f;
    // Thin purple bar above ability bar
    // Placeholder: level / 18 as progress
}
```

### 5.7 Minimap Placeholder

```cpp
void HUD::drawMinimap() {
    float size = 180.0f;
    float x = 10.0f;
    float y = m_screenH - size - 10.0f;
    // Dark green bordered rectangle, bottom-left corner
    // "MINIMAP" placeholder text in center
}
```

### 5.8 Click Indicator (World-Space)

A green shrinking ring on the terrain where the player right-clicks. Fades and shrinks over ~0.5s.

```cpp
struct ClickIndicator {
    glm::vec3 position{0.0f};   // world-space (snapped to terrain Y)
    float     lifetime = 0.0f;  // remaining seconds (starts at 0.5)
    float     maxLife  = 0.5f;
};
```

Render via `DebugRenderer`:
```cpp
if (m_clickIndicator) {
    float t = m_clickIndicator->lifetime / m_clickIndicator->maxLife; // 1→0
    float radius = 0.3f + 0.7f * t;        // shrinks from 1.0 to 0.3
    float alpha  = t;                       // fades out
    glm::vec4 color(0.2f, 1.0f, 0.4f, alpha); // green
    glm::vec3 pos = m_clickIndicator->position;
    pos.y += 0.05f;     // slight offset above terrain (avoid z-fight)
    m_debugRenderer.drawCircle(pos, radius, color, 48);
}
```

Optional enhancement: `ClickIndicatorRenderer.h/cpp` — dedicated textured-quad pipeline with a 64×64 procedural ring texture, UV-animated to shrink and fade.

### 5.9 Health Bars (World-Space, above units)

```cpp
void drawHealthBar(glm::vec3 worldPos, float health, float maxHealth) {
    float barWidth = 1.0f, barHeight = 0.1f;
    glm::vec3 barMin = worldPos + glm::vec3(-barWidth/2, 2.0f, 0);
    glm::vec3 barMax = barMin + glm::vec3(barWidth, barHeight, 0);
    m_debugRenderer.drawAABB(barMin, barMax, glm::vec4(0, 0, 0, 1)); // background

    float healthFrac = glm::clamp(health / maxHealth, 0.0f, 1.0f);
    glm::vec3 healthMax = barMin + glm::vec3(barWidth * healthFrac, barHeight, 0);
    m_debugRenderer.drawAABB(barMin, healthMax, glm::vec4(0, 1, 0, 1)); // green fill
}
```

### 5.10 Integration into Renderer

```cpp
// Renderer.h — members to add
#include "hud/HUD.h"
std::unique_ptr<HUD> m_hud;
std::optional<ClickIndicator> m_clickIndicator;

// Renderer.cpp — initialization
m_hud = std::make_unique<HUD>();
m_hud->init(static_cast<float>(m_swapchain->getExtent().width),
            static_cast<float>(m_swapchain->getExtent().height));

// Renderer.cpp — per-frame (inside ImGui NewFrame / Render block)
if (m_playerEntity != entt::null) {
    m_hud->draw(m_scene, m_playerEntity);
}
```

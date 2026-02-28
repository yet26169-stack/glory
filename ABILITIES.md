1. Executive Overview
The proposed architecture provides a complete framework for managing MOBA-style abilities.

Slot Support: Handles four standard slots (Q, W, E, R), passive abilities, and summoner spells.

Feature Set: Includes full support for cooldowns, resource costs, targeting (AoE, skillshots), crowd control (CC), projectiles, and Vulkan-integrated VFX.

Design Pattern: Utilizes an Entity-Component-System (ECS) pattern with data-driven definitions (JSON/YAML) and a clear separation between logic and rendering via a VFX event queue.

2. Core Architecture
The system relies on a strict dependency and update order to ensure stability.
+1

2.1 System Responsibilities

Layer	System	Responsibility
0	Input	
Captures keys (Q/W/E/R), mouse position, and target selection.

1	Ability	
Processes requests, validates conditions, and triggers execution.

2	Targeting	
Resolves skillshots, AoE circles, and point-and-click targets.

3	Effect	
Applies damage, healing, CC, and buffs/debuffs.

4	Status	
Manages active buffs/debuffs and ticks DoTs/HoTs.

5	Projectile	
Handles movement, collision, and on-hit effects.

6	Cooldown	
Decrements timers and integrates Cooldown Reduction (CDR).

7	VFX Bridge	
Queues particle commands for the Vulkan renderer.

8	Animation	
Triggers cast animations and syncs with ability phases.

9	Audio	
Plays sound effects based on VFX events.

3. Ability State Machine
Every ability instance cycles through a specific set of states to ensure precise gameplay logic.

3.1 State Transitions

READY to CASTING: Triggered by player input if all validation checks pass; deducts resources and begins the cast timer.

CASTING to CHANNELING/EXECUTING: Transitions once the castTime elapses; starts a channel or resolves targets/projectiles.

CHANNELING to EXECUTING: Fires the final effect payload after the duration ends.

INTERRUPTED: Occurs if hard CC is applied or the player cancels; results in a reduced cooldown (75%).

ON_COOLDOWN to READY: Becomes available again once the cooldownRemaining reaches zero.

3.2 Pre-Cast Validation

Before a cast begins, the AbilitySystem validates:

The ability is learned (level > 0).

The ability is not on cooldown or currently being cast.

Sufficient resources (mana, energy, health) are available.

The caster is not disabled by CC (stun, silence, etc.).

Targets are valid, in range, and not obstructed by terrain.

4. Effect and Damage System
The system uses a command pattern where abilities produce effects that are processed by the EffectSystem.

4.1 Damage Calculation

Standard MOBA resistance formulas are used to determine final damage.

Effective Resistance:

effectiveResist=max(0.0f,(resistance×(1.0f−percentPen))−flatPen)


Damage Multiplier:

multiplier= 
100.0f+effectiveResist
100.0f
​	
 


Final Damage:

rawDamage×multiplier


4.2 Crowd Control (CC) Hierarchy

CC types have implicit priorities:

Suppress: Highest priority; prevents all actions and cannot be cleansed.

Stun: Prevents movement, casting, and attacking; reduced by tenacity.

Charm/Fear/Taunt: Forces specific movement patterns.

Knockup/Knockback: Physics displacement; not reduced by tenacity.

Root: Prevents movement only.

Silence: Prevents ability casts.

5. Vulkan VFX Integration
The game logic communicates with the renderer through a lock-free, single-producer single-consumer (SPSC) queue.
+2

GPU Particles: Uses compute shaders for simulation (emission, physics, lifetime).

Data Storage: Particle data is stored in GPU-side SSBOs to avoid CPU readback overhead.

Rendering: Supports instanced drawing, billboard shaders, and depth-tested soft particles.
+1

6. Implementation Roadmap
The plan is divided into six phases spanning 12 weeks.

Weeks 1-2 (Foundation): Implement JSON parser, state machine, and basic pre-cast validation .

Weeks 3-4 (Targeting & Effects): Implement mouse-to-world raycasting, targeting indicators, and damage/CC math .

Weeks 5-6 (Projectiles & Advanced Effects): Build the projectile lifecycle and status effect stacking rules .

Weeks 7-8 (VFX & Polish): Integrate the GPU particle system and animation blending .

Weeks 9-10 (Passives & Networking): Implement the event bus for passives and authoritative server validation .

Weeks 11-12 (Full Champion Implementation): Create complete kits for 3+ champions and finalize the HUD/UI .

7. Key Design Decisions
Libraries: Use nlohmann/json for data, GLM for math, and entt for the ECS framework.
+1

Scripting: Use Lua (via sol2) for complex ability overrides that cannot be expressed purely in JSON.
+1

Thread Safety: Keep game logic on a single thread, using the VFXEventQueue as the only bridge to the render thread.

Authority: The server is the final arbiter for all ability execution and projectile spawning.
+1


MOBA ABILITY SYSTEM: Implementation Plan
Vulkan C++ Custom Game Engine | Technical Architecture & Agent Implementation Guide

February 2026 

1. Executive Overview
This document provides a complete implementation plan for a data-driven ability system in a Vulkan-based C++ MOBA game engine. The system supports four ability slots per character (Q, W, E, R) plus passive abilities and summoner-style spells, with full support for cooldowns, resource costs, targeting, area-of-effect, crowd control, buffs/debuffs, projectiles, and visual effects integration with the Vulkan rendering pipeline.
+1

The architecture follows an Entity-Component-System (ECS) pattern, uses a data-driven approach via JSON/YAML ability definitions, and cleanly separates game logic from rendering through a VFX event queue.

2. Core Architecture
2.1 System Dependency Graph

The ability system sits at the intersection of multiple engine subsystems. Below is the dependency ordering that must be respected during initialization and per-frame updates.

Layer	System	Responsibility
0 - Input	InputSystem	
Captures key presses (Q/W/E/R), mouse position, target selection 

1 - Ability	AbilitySystem	
Processes ability requests, validates conditions, triggers execution phases 

2 - Targeting	TargetingSystem	
Resolves targets from input (skillshot ray, AoE circle, point-and-click) 

3 - Effect	EffectSystem	
Applies damage, healing, CC, buffs/debuffs to resolved targets 

4 - Buff/Debuff	StatusEffectSystem	
Manages active buffs/debuffs, ticks DoTs/HoTs, expiration 

5 - Projectile	ProjectileSystem	
Moves projectile entities, checks collision, triggers on-hit effects 

6 - Cooldown	CooldownSystem	
Decrements cooldown timers, integrates CDR stat 

7 - VFX Bridge	VFXEventQueue	
Queues particle spawn/destroy commands for Vulkan renderer 

8 - Animation	AnimationSystem	
Triggers cast animations, channels, syncs with ability phases 

9 - Audio	AudioSystem	
Plays ability sound effects based on VFX events 

2.2 Per-Frame Update Order

Strict ordering is critical. Each frame processes systems in this sequence:

InputSystem::update() — Capture and enqueue ability requests 

CooldownSystem::update(dt) — Tick all cooldown timers 

StatusEffectSystem::update(dt) — Tick buffs/debuffs, remove expired 

AbilitySystem::update() — Process queued requests through the ability state machine 

TargetingSystem::resolve() — Resolve targets for active abilities 

ProjectileSystem::update(dt) — Move projectiles, check collisions 

EffectSystem::apply() — Apply all pending effects (damage, heal, CC) 

VFXEventQueue::flush() — Dispatch VFX commands to Vulkan renderer 

AnimationSystem::update(dt) — Advance ability animations 

AudioSystem::update() — Play triggered sounds 

3. Core Data Structures
3.1 AbilityDefinition (Data-Driven, loaded from JSON)

Every ability in the game is defined as a data file. The engine loads these at startup and hot-reloads them during development.

C++
struct AbilityDefinition {
    std::string         id;               // "ahri_charm"
    std::string         displayName;      // "Charm"
    AbilitySlot         slot;             // Q, W, E, R, PASSIVE, SUMMONER
    TargetingType       targeting;        // SKILLSHOT, POINT, SELF, TARGETED, VECTOR, NONE

    // Resource & Cooldown [cite: 31, 32, 33, 34, 35]
    ResourceType        resourceType;     // MANA, ENERGY, HEALTH, RAGE, NONE
    std::array<float,5> costPerLevel;     // cost at each ability level (1-5)
    std::array<float,5> cooldownPerLevel; // cooldown in seconds per level [cite: 37, 38, 39]

    // Casting [cite: 40]
    float               castTime;         // 0 = instant cast
    float               channelDuration;  // 0 = not a channel
    bool                canMoveWhileCasting;
    bool                canBeInterrupted; [cite: 41, 43, 45, 46]

    // Range & Area [cite: 47]
    float               castRange;        // max cast distance
    AreaShape           areaShape;        // NONE, CIRCLE, CONE, RECTANGLE, LINE, RING
    float               areaRadius;
    float               areaConeAngle;    // for cone shapes, degrees
    float               areaWidth;        // for rectangle/line shapes
    float               areaLength; [cite: 48, 49, 50, 51, 53, 54]

    // Projectile (if SKILLSHOT) [cite: 55]
    ProjectileDef       projectile;       // speed, width, maxRange, piercing, etc. [cite: 56]

    // Effects on hit [cite: 57]
    std::vector<EffectDef> onHitEffects;  // damage, CC, buff, debuff, etc.
    std::vector<EffectDef> onSelfEffects; // self-buffs on cast [cite: 58, 59]

    // Visual & Audio [cite: 60]
    std::string         castAnimation;
    std::string         castVFX;          // Vulkan particle system ID
    std::string         projectileVFX;
    std::string         impactVFX;
    std::string         castSFX;
    std::string         impactSFX; [cite: 61, 62, 63, 64, 65, 66]

    // Tags for interaction queries [cite: 67]
    std::unordered_set<std::string> tags; // "damage", "cc", "mobility", "ultimate" [cite: 68]
};
3.2 EffectDef (Sub-structure)

C++
struct EffectDef {
    EffectType          type;      // DAMAGE, HEAL, SHIELD, STUN, SLOW, ROOT,
                                   // KNOCKBACK, SILENCE, SUPPRESS, BLIND,
                                   // BUFF_STAT, DEBUFF_STAT, DOT, HOT, DASH, BLINK [cite: 71, 72, 73, 74]
    DamageType          damageType; // PHYSICAL, MAGICAL, TRUE
    ScalingFormula      scaling;    // base + (adRatio * AD) + (apRatio * AP) + ...
    float               duration;   // for CC / buffs / debuffs / dots
    float               tickRate;   // for DoTs/HoTs
    StatModifier        statMod;    // which stat to modify, by how much, flat or %
    float               value;      // slow %, knockback distance, dash distance, etc.
    bool                appliesGrievousWounds;
    std::string         applyVFX;   // VFX on the target when effect is active [cite: 75, 76, 77, 79, 80, 81, 83, 84]
};
3.3 ScalingFormula

C++
struct ScalingFormula {
    std::array<float,5> basePerLevel;  // [60, 100, 140, 180, 220]
    float               adRatio;       // 0.0 - 2.0 typically
    float               apRatio;       // 0.0 - 2.0 typically
    float               hpRatio;       // % of max/bonus/current HP
    HPScalingBasis      hpBasis;       // MAX, BONUS, CURRENT, MISSING
    float               armorRatio;
    float               mrRatio;

    // Evaluation:
    // totalDamage = basePerLevel[level] + (AD * adRatio)
    //             + (AP * apRatio) + (HP_value * hpRatio) + ... [cite: 88, 89, 90, 92, 94, 96, 97, 98, 100, 101]
};
3.4 ProjectileDef

C++
struct ProjectileDef {
    float    speed;           // units per second
    float    width;           // collision hitbox width
    float    maxRange;        // despawn distance
    bool     piercing;        // passes through targets (or stops on first hit)
    int      maxTargets;      // max entities hit if piercing (-1 = unlimited)
    bool     returnsToSource; // boomerang-style (e.g., Ahri Q)
    float    curveAngle;      // 0 = straight, > 0 = arced
    bool     destroyOnWall;   // collides with terrain? [cite: 104, 105, 106, 107, 108, 109, 110, 111, 112]
};
3.5 Runtime Components (ECS)

These hold the mutable state changed every frame.

C++
// Attached to every character entity
struct AbilityBookComponent {
    std::array<AbilityInstance, 6> abilities; // Q, W, E, R, PASSIVE, SUMMONER [cite: 116, 117, 118]
};

struct AbilityInstance {
    const AbilityDefinition* def;
    int                      level;             // 0 = not learned
    float                    cooldownRemaining;
    AbilityPhase             currentPhase;      // READY, CASTING, CHANNELING, ON_COOLDOWN
    float                    phaseTimer;
    TargetInfo               currentTarget; [cite: 120, 121, 122, 124, 125, 126, 127]
};

struct TargetInfo {
    TargetingType type;
    EntityID      targetEntity;   // for point-and-click
    glm::vec3     targetPosition; // for ground-targeted / skillshots
    glm::vec3     direction;      // for skillshots / vector targeting [cite: 129, 130, 131, 132, 133]
};

struct StatusEffectsComponent {
    std::vector<ActiveStatusEffect> activeEffects; [cite: 135, 136]
};

struct ActiveStatusEffect {
    const EffectDef* def;
    EntityID         sourceEntity;
    float            remainingDuration;
    float            tickAccumulator;
    float            totalValue;  // pre-computed at apply time [cite: 138, 139, 140, 141, 142, 143]
};
4. Ability State Machine
4.1 State Transitions

From	To	Trigger	Action
READY	CASTING	Player presses ability key, all conditions pass	
Deduct resource, begin cast timer, play cast anim + VFX 

CASTING	CHANNELING	castTime elapsed AND channelDuration > 0	
Begin channel, lock movement if needed 

CASTING	EXECUTING	castTime elapsed AND channelDuration == 0	
Resolve targets, spawn projectiles or apply instant effects 

CHANNELING	EXECUTING	channelDuration elapsed	
Fire final effect payload 

CHANNELING	INTERRUPTED	Hard CC applied OR player cancels	
Cancel channel, enter partial cooldown (75%) 

EXECUTING	ON_COOLDOWN	All effects dispatched	
Start cooldown timer = baseCooldown * (1 - CDR) 

INTERRUPTED	ON_COOLDOWN	Immediate	
Start reduced cooldown 

ON_COOLDOWN	READY	cooldownRemaining <= 0	
Ability available again 

5. Targeting System
5.1 Targeting Types

Type	Input Required	Resolution Logic
SKILLSHOT	Direction from caster	
Spawn projectile along direction vector; detect hits via collision 

POINT	Ground position	
Gather all entities within AoE shape at target position 

TARGETED	Enemy entity under cursor	
Validate entity is enemy, alive, in range, visible; auto-face target 

SELF	None	
Apply effects to caster; optionally resolve AoE around caster 

VECTOR	Start point + end point	
Define a line/cone from start to end; resolve entities in the shape 

NONE	Automatic (passive)	
Triggered by game events (on-hit, on-kill, etc.) 

6. Effect System
6.2 Damage Formula

C++
// Standard MOBA armor/MR formula
float CalculateDamage(float rawDamage, DamageType type, const Stats& target) {
    if (type == DamageType::TRUE) return rawDamage;

    float resistance = (type == DamageType::PHYSICAL)
        ? target.armor : target.magicResist;

    // Apply penetration (flat after %)
    float effectiveResist = resistance * (1.0f - percentPen) - flatPen;
    effectiveResist = std::max(0.0f, effectiveResist);

    float multiplier = 100.0f / (100.0f + effectiveResist);
    return rawDamage * multiplier; [cite: 191, 192, 193, 194, 196, 197, 198, 199]
}
6.3 Crowd Control Hierarchy

Priority	CC Type	Prevents	Notes
1 (highest)	Suppress	Everything	
Cannot be cleansed. Caster is also locked in place. 

2	Stun	Move, Cast, Attack	
Can be cleansed. Reduced by tenacity. 

4	Knockup	Move, Cast, Attack	
Not reduced by tenacity. Applies physics displacement. 

5	Root / Snare	Movement only	
Can still cast and attack 

7	Slow	Reduces speed	
Multiplicative stacking. Soft cap at 110 base. 

8. Buff / Debuff System
8.1 Status Effect Lifecycle

C++
void StatusEffectSystem::update(float dt) {
    for (auto& [entity, statusComp] : view<StatusEffectsComponent>()) {
        auto it = statusComp.activeEffects.begin();
        while (it != statusComp.activeEffects.end()) {
            it->remainingDuration -= dt;

            // Tick DoTs/HoTs
            if (it->def->tickRate > 0) {
                it->tickAccumulator += dt;
                while (it->tickAccumulator >= it->def->tickRate) {
                    it->tickAccumulator -= it->def->tickRate;
                    applyTickEffect(entity, *it);
                }
            }

            // Remove expired
            if (it->remainingDuration <= 0.0f) {
                removeEffectVisuals(entity, *it);
                it = statusComp.activeEffects.erase(it);
            } else { ++it; }
        }
    }
} [cite: 222, 223, 224, 225, 226, 228, 229, 230, 231, 232, 236, 237, 238, 239]
10. Data-Driven Ability Pipeline
10.1 JSON Ability Definition Example

JSON
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
} [cite: 283, 284, 285, 286, 287, 288, 289, 290, 291, 296, 297, 298, 299, 300, 303, 305, 306, 307, 308, 309, 320, 321, 322]
11. Networking Considerations
11.2 Network Messages

C++
// Client -> Server
struct AbilityRequestMsg {
    EntityID    casterID;
    AbilitySlot slot;
    TargetInfo  target;
    uint32_t    sequenceNumber; // for prediction reconciliation
    float       clientTimestamp; [cite: 342, 343, 344, 345, 346, 347]
};

// Server -> All Clients
struct ProjectileSpawnMsg {
    uint32_t    projectileID;
    std::string abilityID;
    glm::vec3   position;
    glm::vec3   direction;
    float       speed; [cite: 366, 367, 368, 369, 370, 371]
};
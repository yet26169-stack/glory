#pragma once

// ── Ability System: Data Types ─────────────────────────────────────────────
// Data-driven MOBA ability definitions (loaded from JSON) and supporting enums.
// No Vulkan dependency — purely game-logic structs.

#include <glm/glm.hpp>
#include <array>
#include <string>
#include <vector>
#include <unordered_set>
#include <cstdint>

namespace glory {

// ── Entity ID ─────────────────────────────────────────────────────────────
using EntityID = uint32_t;
static constexpr EntityID NULL_ENTITY = UINT32_MAX;

// ── Enumerations ──────────────────────────────────────────────────────────

enum class AbilitySlot : uint8_t { Q, W, E, R, PASSIVE, SUMMONER, COUNT };

enum class TargetingType : uint8_t {
    SKILLSHOT,   // fires a projectile along a direction
    POINT,       // ground-targeted AoE at cursor position
    TARGETED,    // single-target (requires enemy entity under cursor)
    SELF,        // always applied to the caster
    VECTOR,      // start + end point (e.g. blink or displacement)
    NONE         // passive / auto-triggered
};

enum class AreaShape : uint8_t {
    NONE, CIRCLE, CONE, RECTANGLE, LINE, RING
};

enum class ResourceType : uint8_t {
    MANA, ENERGY, HEALTH, RAGE, FURY, NONE
};

enum class DamageType : uint8_t { PHYSICAL, MAGICAL, TRUE_DMG };

enum class EffectType : uint8_t {
    DAMAGE,
    HEAL,
    SHIELD,
    STUN,
    SLOW,
    ROOT,
    SILENCE,
    KNOCKBACK,
    KNOCKUP,
    CHARM,
    FEAR,
    TAUNT,
    SUPPRESS,
    BLIND,
    BUFF_STAT,
    DEBUFF_STAT,
    DOT,           // damage over time
    HOT,           // heal over time
    DASH,
    BLINK
};

enum class HPScalingBasis : uint8_t { MAX, BONUS, CURRENT, MISSING };

enum class AbilityPhase : uint8_t {
    READY,
    CASTING,      // cast time countdown
    CHANNELING,   // channel duration countdown
    EXECUTING,    // effects are being applied (one-shot; transition to ON_COOLDOWN)
    ON_COOLDOWN,
    INTERRUPTED
};

// ── Scaling formula ────────────────────────────────────────────────────────
struct ScalingFormula {
    std::array<float, 5> basePerLevel{};  // [0] = level 1, [4] = level 5
    float adRatio    = 0.0f;
    float apRatio    = 0.0f;
    float hpRatio    = 0.0f;
    HPScalingBasis hpBasis = HPScalingBasis::MAX;
    float armorRatio = 0.0f;
    float mrRatio    = 0.0f;

    // Evaluate at the given level (0-based index: level 1 → index 0)
    float evaluate(int abilityLevel, float ad, float ap,
                   float hp, float armor, float mr) const {
        const int idx   = std::max(0, std::min(abilityLevel - 1, 4));
        float     total = basePerLevel[static_cast<size_t>(idx)];
        total += ad    * adRatio;
        total += ap    * apRatio;
        total += hp    * hpRatio;
        total += armor * armorRatio;
        total += mr    * mrRatio;
        return total;
    }
};

// ── Stat modifier ──────────────────────────────────────────────────────────
struct StatModifier {
    std::string stat;       // "movementSpeed", "attackDamage", etc.
    float       flatValue  = 0.0f;
    float       percentValue = 0.0f; // additive percent
};

// ── Projectile definition ──────────────────────────────────────────────────
struct ProjectileDef {
    float  speed          = 1200.0f; // units/s initial speed
    float  acceleration   = 0.0f;    // units/s² — speed increase per second
    float  maxSpeed       = 9999.0f; // cap for accelerating projectiles
    float  width          = 60.0f;   // collision hitbox width
    float  maxRange       = 1100.0f; // despawn distance
    bool   piercing       = false;   // passes through targets
    int    maxTargets     = 1;       // -1 = unlimited
    bool   returnsToSource = false;  // boomerang style
    float  curveAngle     = 0.0f;   // 0 = straight, >0 = arc
    bool   destroyOnWall  = true;

    // Lob (arc) projectile — used for thrown grenades / bombs
    bool   isLob         = false;
    float  lobFlightTime = 1.0f;   // total arc duration in seconds
    float  lobApexHeight = 8.0f;   // extra Y added at arc midpoint (Bezier P1)
};

// ── Effect definition ──────────────────────────────────────────────────────
struct EffectDef {
    EffectType      type         = EffectType::DAMAGE;
    DamageType      damageType   = DamageType::MAGICAL;
    ScalingFormula  scaling;
    float           duration     = 0.0f;    // for CC / buffs / DoTs
    float           tickRate     = 0.5f;    // for DoTs/HoTs
    StatModifier    statMod;
    float           value        = 0.0f;    // slow %, knockback distance, etc.
    bool            appliesGrievousWounds = false;
    std::string     applyVFX;               // visual on the target (e.g. "vfx_stun")
};

// ── Ability definition ─────────────────────────────────────────────────────
struct AbilityDefinition {
    std::string   id;            // "fire_mage_fireball"
    std::string   displayName;   // "Fireball"
    AbilitySlot   slot          = AbilitySlot::Q;
    TargetingType targeting     = TargetingType::SKILLSHOT;

    ResourceType  resourceType  = ResourceType::MANA;
    std::array<float, 5> costPerLevel{};
    std::array<float, 5> cooldownPerLevel{};

    float  castTime            = 0.0f;
    float  channelDuration     = 0.0f;
    bool   canMoveWhileCasting = false;
    bool   canBeInterrupted    = true;

    float      castRange   = 1100.0f;
    AreaShape  areaShape   = AreaShape::NONE;
    float      areaRadius  = 0.0f;
    float      areaConeAngle = 0.0f;
    float      areaWidth   = 0.0f;
    float      areaLength  = 0.0f;

    ProjectileDef projectile;

    std::vector<EffectDef> onHitEffects;
    std::vector<EffectDef> onSelfEffects;

    std::string castAnimation;
    std::vector<std::string> castVFX;        // e.g. ["vfx_fireball_cast", "vfx_glow"]
    std::vector<std::string> projectileVFX;
    std::vector<std::string> impactVFX;
    std::string castSFX;
    std::string impactSFX;

    std::unordered_set<std::string> tags;  // "damage", "cc", "mobility", "ultimate"
};

// ── Damage calculation ─────────────────────────────────────────────────────
struct Stats {
    float attackDamage  = 60.0f;
    float abilityPower  = 0.0f;
    float armor         = 40.0f;
    float magicResist   = 32.0f;
    float maxHP         = 600.0f;
    float currentHP     = 600.0f;
    float flatArmorPen  = 0.0f;
    float percentArmorPen = 0.0f;
    float flatMagicPen  = 0.0f;
    float percentMagicPen = 0.0f;
    float tenacity      = 0.0f;   // 0..1
    float cooldownReduction = 0.0f; // 0..0.45
};

inline float calculateDamage(float rawDamage, DamageType type, const Stats& target,
                              float flatPen = 0.0f, float percentPen = 0.0f) {
    if (type == DamageType::TRUE_DMG) return rawDamage;

    const float resistance = (type == DamageType::PHYSICAL)
                             ? target.armor : target.magicResist;

    float effectiveResist = resistance * (1.0f - percentPen) - flatPen;
    effectiveResist = std::max(0.0f, effectiveResist);

    const float multiplier = 100.0f / (100.0f + effectiveResist);
    return rawDamage * multiplier;
}

} // namespace glory

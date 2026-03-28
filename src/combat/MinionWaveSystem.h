#pragma once

// ── Minion Wave Spawning & AI ───────────────────────────────────────────────
// Spawns waves of melee/ranged/cannon minions per lane per team on a timer.
// Minions follow flow fields toward enemy nexus.  Aggro: closest enemy
// minion/structure in range, with hero-aggro override (leash + timeout).

#include "combat/CombatComponents.h"
#include "combat/EconomySystem.h"
#include "map/MapTypes.h"
#include "scene/Components.h"

#include <entt.hpp>
#include <glm/glm.hpp>
#include <functional>
#include <vector>
#include <cstdint>

namespace glory {

class PathfindingSystem;
class Scene;
class CombatSystem;

// ── Per-entity tint (used by renderer for team colouring) ───────────────────
struct TintComponent {
    glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
};

// ── Minion AI state machine ─────────────────────────────────────────────────
enum class MinionAIState : uint8_t {
    LANE_MARCH,      // Default: follow flow field down the lane
    CHASE_TARGET,    // Moving toward an aggro target
    ATTACKING,       // In attack range, letting CombatSystem drive the attack cycle
    RETURN_TO_LANE,  // Lost target, walking back to lane flow field
};

// ── Per-minion wave AI component ────────────────────────────────────────────
struct WaveMinionComponent {
    uint8_t   teamIndex   = 0;      // 0=blue, 1=red
    uint8_t   laneIndex   = 0;      // 0=top, 1=mid, 2=bot
    MinionType type       = MinionType::MELEE;

    // FSM state
    MinionAIState aiState = MinionAIState::LANE_MARCH;

    // Aggro state
    entt::entity aggroTarget = entt::null;
    float aggroRange         = 6.0f;   // leash radius (drop target beyond this × 1.5)
    float acquisitionRange   = 8.0f;   // scanning radius for new targets
    float callForHelpRadius  = 10.0f;  // radius for hero-attacked-hero aggro

    // Retarget cooldown — avoids O(N×M) registry scan every frame
    float retargetCooldown = 0.0f;
    static constexpr float RETARGET_INTERVAL = 0.25f; // seconds between target searches

    // Hero-aggro override (when hero attacks nearby allied hero)
    entt::entity heroAggroTarget = entt::null;
    float heroAggroTimer  = 0.0f;   // seconds remaining on hero aggro
    float heroAggroLeash  = 10.0f;  // max distance before dropping hero aggro
    static constexpr float HERO_AGGRO_DURATION = 6.0f;

    // Lane return: remember where we left the lane to limit chase distance
    glm::vec3 laneLeavePos{0.0f};
    static constexpr float MAX_CHASE_DIST = 12.0f; // max distance from lane before forced return

    // NPC ability decision
    float   decisionCooldown = 0.0f; // seconds until next ability decision
    uint8_t abilitySetId     = 0;    // 0 = melee, 1 = caster (reserved)
};

// ── Spawn configuration (mesh, textures, animation) ─────────────────────────
// Mirrors MinionSpawnConfig from GameplaySystem.h but stored here for clarity.
struct WaveSpawnConfig {
    uint32_t meshIndex     = 0;
    uint32_t texIndex      = 0;
    uint32_t flatNormIndex = 0;
    Skeleton skeleton;
    // skinVertices / bindPoseVertices removed — not needed for GPU skinning.
    std::vector<AnimationClip> clips;
    bool ready = false;
};

// ── MinionWaveSystem ────────────────────────────────────────────────────────
class MinionWaveSystem {
public:
    void init(const MapData& mapData);

    void setSpawnConfig(WaveSpawnConfig config) { m_spawnCfg = std::move(config); }
    const WaveSpawnConfig& getSpawnConfig() const { return m_spawnCfg; }

    void setCasterSpawnConfig(WaveSpawnConfig config) { m_casterSpawnCfg = std::move(config); }
    const WaveSpawnConfig& getCasterSpawnConfig() const { return m_casterSpawnCfg; }

    void setPathfinding(PathfindingSystem* ps) { m_pathfinding = ps; }
    void setScene(Scene* s) { m_scene = s; }
    void setCombatSystem(CombatSystem* cs) { m_combat = cs; }

    // Called each sim tick.  gameTime is seconds since match start.
    void update(entt::registry& reg, float dt, float gameTime);

    // Notify hero-aggro: when heroAttacker attacks heroVictim, nearby enemy
    // minions within leash range switch target to heroAttacker for 6s.
    void notifyHeroAttackedHero(entt::registry& reg,
                                entt::entity heroAttacker,
                                entt::entity heroVictim);

    uint32_t waveNumber() const { return m_waveNumber; }

private:
    // ── Wave timing ─────────────────────────────────────────────────────────
    static constexpr float FIRST_WAVE_TIME  = 65.0f;  // 1:05
    static constexpr float WAVE_INTERVAL    = 30.0f;
    float    m_nextWaveTime = FIRST_WAVE_TIME;
    uint32_t m_waveNumber   = 0;

    // ── Per-minion scaling ──────────────────────────────────────────────────
    struct MinionStats {
        float hp;
        float ad;
        float moveSpeed;
        float attackRange;
        bool  isRanged;
        MinionType type;
        int   goldReward;
        int   xpReward;
    };
    MinionStats scaledStats(MinionType type, uint32_t wave) const;

    // ── Spawn helpers ───────────────────────────────────────────────────────
    void spawnWave(entt::registry& reg);
    entt::entity spawnMinion(entt::registry& reg,
                             uint8_t team, uint8_t lane,
                             glm::vec3 pos, const MinionStats& stats,
                             const glm::vec4& tint);

    // ── Movement & aggro ────────────────────────────────────────────────────
    void updateMovementAndAggro(entt::registry& reg, float dt);
    entt::entity findTarget(entt::registry& reg,
                            entt::entity self,
                            const WaveMinionComponent& wmc,
                            glm::vec3 selfPos) const;

    // ── Data ────────────────────────────────────────────────────────────────
    MapData            m_mapData;
    WaveSpawnConfig    m_spawnCfg;        // melee minions
    WaveSpawnConfig    m_casterSpawnCfg;  // ranged / cannon minions
    PathfindingSystem* m_pathfinding = nullptr;
    Scene*             m_scene       = nullptr;
    CombatSystem*      m_combat      = nullptr;

    // Team tint colours
    static constexpr glm::vec4 BLUE_TINT{0.6f, 0.7f, 1.0f, 1.0f};
    static constexpr glm::vec4 RED_TINT {1.0f, 0.55f, 0.5f, 1.0f};
};

} // namespace glory

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

// ── Per-entity tint (used by renderer for team colouring) ───────────────────
struct TintComponent {
    glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
};

// ── Per-minion wave AI component ────────────────────────────────────────────
struct WaveMinionComponent {
    uint8_t   teamIndex   = 0;      // 0=blue, 1=red
    uint8_t   laneIndex   = 0;      // 0=top, 1=mid, 2=bot
    MinionType type       = MinionType::MELEE;

    // Aggro state
    entt::entity aggroTarget = entt::null;
    float aggroRange      = 6.0f;   // acquire-target radius

    // Hero-aggro override (when hero attacks nearby allied hero)
    entt::entity heroAggroTarget = entt::null;
    float heroAggroTimer  = 0.0f;   // seconds remaining on hero aggro
    float heroAggroLeash  = 10.0f;  // max distance before dropping hero aggro
    static constexpr float HERO_AGGRO_DURATION = 6.0f;
};

// ── Spawn configuration (mesh, textures, animation) ─────────────────────────
// Mirrors MinionSpawnConfig from GameplaySystem.h but stored here for clarity.
struct WaveSpawnConfig {
    uint32_t meshIndex     = 0;
    uint32_t texIndex      = 0;
    uint32_t flatNormIndex = 0;
    Skeleton skeleton;
    std::vector<std::vector<SkinVertex>> skinVertices;
    std::vector<std::vector<Vertex>>     bindPoseVertices;
    std::vector<AnimationClip>           clips;
    bool ready = false;
};

// ── MinionWaveSystem ────────────────────────────────────────────────────────
class MinionWaveSystem {
public:
    void init(const MapData& mapData);

    void setSpawnConfig(WaveSpawnConfig config) { m_spawnCfg = std::move(config); }
    const WaveSpawnConfig& getSpawnConfig() const { return m_spawnCfg; }

    void setPathfinding(PathfindingSystem* ps) { m_pathfinding = ps; }
    void setScene(Scene* s) { m_scene = s; }

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
    WaveSpawnConfig    m_spawnCfg;
    PathfindingSystem* m_pathfinding = nullptr;
    Scene*             m_scene       = nullptr;

    // Team tint colours
    static constexpr glm::vec4 BLUE_TINT{0.6f, 0.7f, 1.0f, 1.0f};
    static constexpr glm::vec4 RED_TINT {1.0f, 0.55f, 0.5f, 1.0f};
};

} // namespace glory

#pragma once
#include "structure/StructureConfig.h"
#include "map/MapTypes.h"
#include <entt/entt.hpp>
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

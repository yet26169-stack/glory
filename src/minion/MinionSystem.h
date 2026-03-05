#pragma once

#include "minion/MinionComponents.h"
#include "minion/MinionConfig.h"
#include "minion/SpatialHash.h"
#include "map/MapTypes.h"

#include <entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <vector>

namespace glory {

/// Callback for height queries (matches TerrainSystem::GetHeightAt signature).
using HeightQueryFn = std::function<float(float x, float z)>;

/// Event emitted when a minion dies — renderer / reward system can listen.
struct MinionDeathEvent {
  entt::entity minion = entt::null;
  MinionType type = MinionType::Melee;
  TeamID team = TeamID::Blue;
  LaneType lane = LaneType::Mid;
  glm::vec3 position{0.0f};
  entt::entity killer = entt::null;
  float goldValue = 0.0f;
  float xpValue = 0.0f;
};

/// Manages spawn, AI, combat, projectiles, death, and stat scaling for minions.
/// Call `update()` once per frame from Scene or Renderer.
class MinionSystem {
public:
  MinionSystem() = default;

  /// Initialise with config and map data.  Must be called before update().
  void init(const MinionConfig &config, const MapData &mapData);

  /// Main per-frame tick.  `gameTime` is total elapsed game time in seconds.
  void update(entt::registry &reg, float dt, float gameTime,
              const HeightQueryFn &heightQuery = nullptr);

  /// Notify that a champion attacked another champion (for champion aggro draw).
  void notifyChampionAttack(entt::entity attacker, entt::entity victim,
                            const glm::vec3 &victimPos);

  /// Notify that an inhibitor was destroyed/respawned.
  void setInhibitorDown(TeamID team, LaneType lane, bool down);
  void notifyInhibitorDestroyed(TeamID team, LaneType lane) {
      setInhibitorDown(team, lane, true);
  }

  /// Consume accumulated death events (cleared after each call).
  std::vector<MinionDeathEvent> consumeDeathEvents();

  /// Total living minion count.
  uint32_t getLivingCount() const { return m_livingCount; }

  /// Access config (for tests / hot-reload).
  const MinionConfig &getConfig() const { return m_config; }
  void reloadConfig(const MinionConfig &config) { m_config = config; }

private:
  MinionConfig m_config;
  MapData m_mapData;

  // Spawn state
  uint32_t m_waveCounter = 0;
  float m_nextWaveTime = 0.0f;
  uint32_t m_livingCount = 0;

  // Inhibitor state: [teamIdx][laneIdx] = true if enemy inhib destroyed
  bool m_inhibitorDown[2][3] = {};

  // Spatial hash rebuilt every frame
  SpatialHash m_spatialBlue{14.0f};
  SpatialHash m_spatialRed{14.0f};

  // Champion aggro events (consumed each aggro tick)
  struct ChampionAggroEvent {
    entt::entity attacker = entt::null;
    entt::entity victim = entt::null;
    glm::vec3 victimPos{0.0f};
    float timestamp = 0.0f;
  };
  std::vector<ChampionAggroEvent> m_champAggroEvents;
  float m_currentGameTime = 0.0f;

  // Death events
  std::vector<MinionDeathEvent> m_deathEvents;

  // Scratch buffers (avoid per-frame allocations)
  std::vector<entt::entity> m_queryResults;

  // ── Sub-systems ───────────────────────────────────────────────────────────

  void spawnWave(entt::registry &reg, float gameTime,
                const HeightQueryFn &heightQuery);
  entt::entity spawnMinion(entt::registry &reg, MinionType type, TeamID team,
                           LaneType lane, const glm::vec3 &pos,
                           uint32_t waveIdx, float gameTime);

  void buildSpatialHash(entt::registry &reg);

  void updateStatScaling(entt::registry &reg, float gameTime);
  void updateAggro(entt::registry &reg, float dt);
  void updateMovement(entt::registry &reg, float dt,
                      const HeightQueryFn &heightQuery);
  void updateCombat(entt::registry &reg, float dt);
  void updateProjectiles(entt::registry &reg, float dt);
  void updateDeath(entt::registry &reg, float dt, float gameTime);
  void updateStates(entt::registry &reg, float dt);

  // Helpers
  float computeScaledStat(float base, float perTick, float gameTime) const;
  float computeDamage(float ad, float targetArmor) const;
  float computeGold(MinionType type, float gameTime) const;
  bool isCannonWave(uint32_t waveIdx, float gameTime) const;

  const std::vector<glm::vec3> &getLaneWaypoints(TeamID team,
                                                  LaneType lane) const;
};

} // namespace glory

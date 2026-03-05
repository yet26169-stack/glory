#pragma once
#include "jungle/JungleConfig.h"
#include "map/MapTypes.h"
#include <entt/entt.hpp>
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

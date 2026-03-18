#include "nav/DynamicObstacle.h"

#include <spdlog/spdlog.h>
#include <vector>

namespace glory {

// dtTileCache implementation requires tiled navmesh setup.
// Since NavMeshBuilder currently produces a single-tile blob,
// we manage obstacles here for FlowField baking, but dtTileCache
// hooks are stubs until Renderer/NavMeshBuilder support tiled generation.

void DynamicObstacleManager::init() {
    m_obstacles.clear();
    m_nextId = 0;
    spdlog::info("[DynamicObstacleManager] initialized");
}

void DynamicObstacleManager::shutdown() {
    m_obstacles.clear();
    m_nextId = 0;
    spdlog::info("[DynamicObstacleManager] shut down");
}

ObstacleId DynamicObstacleManager::addObstacle(const ObstacleDesc& desc) {
    ObstacleId id = m_nextId++;

    ObstacleEntry entry;
    entry.desc = desc;
    entry.remainingLife = (desc.lifetime > 0.0f) ? desc.lifetime : -1.0f;

    m_obstacles[id] = entry;
    ++m_version;

    const char* shapeName = (desc.shape == ObstacleShape::Cylinder) ? "Cylinder" : "Box";
    spdlog::debug("[DynamicObstacleManager] addObstacle id={} shape={} pos=({:.1f},{:.1f},{:.1f}) life={:.1f}",
                  id, shapeName, desc.position.x, desc.position.y, desc.position.z, desc.lifetime);

    return id;
}

void DynamicObstacleManager::removeObstacle(ObstacleId id) {
    auto it = m_obstacles.find(id);
    if (it == m_obstacles.end()) {
        spdlog::warn("[DynamicObstacleManager] removeObstacle — id {} not found", id);
        return;
    }
    m_obstacles.erase(it);
    ++m_version;
    spdlog::debug("[DynamicObstacleManager] removeObstacle id={}", id);
}

void DynamicObstacleManager::update(float dt) {
    std::vector<ObstacleId> expired;
    for (auto& [id, entry] : m_obstacles) {
        if (entry.remainingLife < 0.0f) continue;  // permanent
        entry.remainingLife -= dt;
        if (entry.remainingLife <= 0.0f) {
            expired.push_back(id);
        }
    }

    for (ObstacleId id : expired) {
        spdlog::debug("[DynamicObstacleManager] obstacle {} expired", id);
        m_obstacles.erase(id);
    }
    if (!expired.empty()) ++m_version;
}

uint32_t DynamicObstacleManager::obstacleCount() const {
    return static_cast<uint32_t>(m_obstacles.size());
}

const ObstacleDesc* DynamicObstacleManager::getObstacle(ObstacleId id) const {
    auto it = m_obstacles.find(id);
    if (it == m_obstacles.end()) return nullptr;
    return &it->second.desc;
}

} // namespace glory

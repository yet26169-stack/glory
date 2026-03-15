#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>
#include <unordered_map>

namespace glory {

using ObstacleId = uint32_t;
static constexpr ObstacleId INVALID_OBSTACLE = UINT32_MAX;

enum class ObstacleShape { Cylinder, Box };

struct ObstacleDesc {
    ObstacleShape shape = ObstacleShape::Cylinder;
    glm::vec3 position{0.0f};
    float radius = 1.0f;            // for cylinder
    float height = 2.0f;
    glm::vec3 halfExtents{1.0f};    // for box
    float lifetime = 0.0f;          // 0 = permanent
};

class DynamicObstacleManager {
public:
    void init();
    void shutdown();

    ObstacleId addObstacle(const ObstacleDesc& desc);
    void removeObstacle(ObstacleId id);
    void update(float dt);  // decrements lifetimes, removes expired

    uint32_t obstacleCount() const;
    const ObstacleDesc* getObstacle(ObstacleId id) const;

private:
    struct ObstacleEntry {
        ObstacleDesc desc;
        float remainingLife = 0.0f;
        // dtTileCacheObstacle handle will go here
    };
    std::unordered_map<ObstacleId, ObstacleEntry> m_obstacles;
    ObstacleId m_nextId = 0;
};

} // namespace glory

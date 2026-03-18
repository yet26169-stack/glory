#pragma once

#include "nav/FlowField.h"

#include <glm/glm.hpp>
#include <entt.hpp>
#include <vector>
#include <array>
#include <cstdint>
#include <unordered_map>

#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>
#include <DetourCrowd.h>

namespace glory {

struct NavMeshData;
struct MapData;
class DynamicObstacleManager;

using AgentId = uint32_t;
static constexpr AgentId INVALID_AGENT = UINT32_MAX;

struct PathResult {
    std::vector<glm::vec3> waypoints;
    bool found = false;
};

/// Flow field key: (team, lane) → index.
///   Blue Top=0, Blue Mid=1, Blue Bot=2, Red Top=3, Red Mid=4, Red Bot=5.
static constexpr uint32_t FLOW_FIELD_COUNT = 6;

/// NavMesh-based pathfinding and crowd simulation.
///
/// Hybrid approach:
///   - Minions query flow fields for O(1) steering (one field per lane endpoint).
///   - Champions use individual A* path queries.
class PathfindingSystem {
public:
    // Initialize with built navmesh data
    bool init(const NavMeshData& navData);
    void shutdown();

    /// Initialise flow fields for all 6 lane endpoints.
    /// Call once after init() when MapData is available.
    void initFlowFields(const MapData& map);

    /// Regenerate any dirty flow fields (call once per frame).
    void updateFlowFields(const DynamicObstacleManager* obstacles = nullptr);

    /// Sample the flow field for a given team+lane at a world position.
    /// Returns a normalised XZ direction or (0,0) if outside grid.
    glm::vec2 sampleFlowField(uint32_t team, uint32_t lane, glm::vec2 worldXZ) const;

    /// Direct access to a flow field by index.
    const FlowField* getFlowField(uint32_t index) const;

    // Single path query (synchronous, for champions)
    PathResult findPath(glm::vec3 start, glm::vec3 end) const;

    // Crowd agent management (for hero/monster movement)
    AgentId addAgent(entt::entity entity, glm::vec3 pos, float radius, float speed);
    void removeAgent(AgentId id);
    void setAgentTarget(AgentId id, glm::vec3 target);
    void setAgentSpeed(AgentId id, float speed);
    glm::vec3 getAgentPosition(AgentId id) const;
    glm::vec3 getAgentVelocity(AgentId id) const;

    // Tick the crowd simulation
    void update(float dt);

    // Get entity for an agent
    entt::entity getAgentEntity(AgentId id) const;

    // Query: is a point on the navmesh?
    bool isOnNavMesh(glm::vec3 point, float searchRadius = 2.0f) const;

    // Query: find nearest point on navmesh
    glm::vec3 findNearestPoint(glm::vec3 point, float searchRadius = 5.0f) const;

    uint32_t agentCount() const { return static_cast<uint32_t>(m_agents.size()); }
    bool isInitialized() const { return m_initialized; }

    bool flowFieldsReady() const { return m_flowFieldsReady; }

private:
    bool m_initialized = false;
    bool m_flowFieldsReady = false;

    struct AgentData {
        entt::entity entity;
        glm::vec3 position{0.0f};
        glm::vec3 target{0.0f};
        glm::vec3 velocity{0.0f};
        float radius = 0.5f;
        float speed  = 5.0f;
        bool hasTarget = false;
    };

    std::unordered_map<AgentId, AgentData> m_agents;
    AgentId m_nextId = 0;

    // Flow fields: 6 = 2 teams × 3 lanes
    std::array<FlowField, FLOW_FIELD_COUNT> m_flowFields;
    std::array<glm::vec2, FLOW_FIELD_COUNT> m_flowFieldGoals;

    // Recast/Detour handles
    dtNavMesh*      m_navMesh = nullptr;
    dtNavMeshQuery* m_query   = nullptr;
    dtCrowd*        m_crowd   = nullptr;
};

} // namespace glory

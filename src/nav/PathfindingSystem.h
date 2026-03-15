#pragma once

#include <glm/glm.hpp>
#include <entt.hpp>
#include <vector>
#include <cstdint>
#include <unordered_map>

namespace glory {

struct NavMeshData;

using AgentId = uint32_t;
static constexpr AgentId INVALID_AGENT = UINT32_MAX;

struct PathResult {
    std::vector<glm::vec3> waypoints;
    bool found = false;
};

/// NavMesh-based pathfinding and crowd simulation.
///
/// Hybrid approach with LaneFollower:
///   - Minions follow lanes via LaneFollower (spline-based, unchanged).
///   - When a minion must detour (e.g. chasing a hero off-lane), call
///     findPath() to get a navmesh path, then feed the waypoints to a
///     simple steering loop (or use addAgent() for crowd-managed movement).
///   - Heroes and monsters always use this system for movement.
class PathfindingSystem {
public:
    // Initialize with built navmesh data
    bool init(const NavMeshData& navData);
    void shutdown();

    // Single path query (synchronous)
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

private:
    bool m_initialized = false;

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

    // Recast/Detour handles (activate when library is linked):
    // dtNavMesh*      m_navMesh = nullptr;
    // dtNavMeshQuery* m_query   = nullptr;
    // dtCrowd*        m_crowd   = nullptr;
};

} // namespace glory

#include "nav/PathfindingSystem.h"
#include "nav/NavMeshBuilder.h"

#include <spdlog/spdlog.h>
#include <glm/geometric.hpp>

namespace glory {

bool PathfindingSystem::init(const NavMeshData& navData) {
    if (m_initialized) {
        spdlog::warn("[PathfindingSystem] already initialized — call shutdown() first");
        return false;
    }

    if (!navData.valid) {
        spdlog::warn("[PathfindingSystem] NavMeshData not valid — running in stub mode (straight-line movement)");
    } else {
        spdlog::info("[PathfindingSystem] init() — navData {} bytes",
                     navData.serializedData.size());
    }

    // TODO: dtAllocNavMesh, dtAllocNavMeshQuery, dtAllocCrowd
    //       m_navMesh->init(navData.serializedData ...)
    //       m_query->init(m_navMesh, 2048)
    //       m_crowd->init(MAX_AGENTS, agentRadius, m_navMesh)

    m_initialized = true;
    spdlog::info("[PathfindingSystem] initialized (stub — straight-line fallback)");
    return true;
}

void PathfindingSystem::shutdown() {
    if (!m_initialized) return;

    // TODO: dtFreeCrowd, dtFreeNavMeshQuery, dtFreeNavMesh
    m_agents.clear();
    m_nextId = 0;
    m_initialized = false;
    spdlog::info("[PathfindingSystem] shut down");
}

PathResult PathfindingSystem::findPath(glm::vec3 start, glm::vec3 end) const {
    PathResult result;

    if (!m_initialized) {
        spdlog::warn("[PathfindingSystem] findPath() called before init()");
        return result;
    }

    // TODO: dtNavMeshQuery::findPath + findStraightPath
    // Stub: return a direct line from start to end
    result.waypoints.push_back(start);
    result.waypoints.push_back(end);
    result.found = true;
    return result;
}

AgentId PathfindingSystem::addAgent(entt::entity entity, glm::vec3 pos,
                                    float radius, float speed) {
    if (!m_initialized) {
        spdlog::warn("[PathfindingSystem] addAgent() called before init()");
        return INVALID_AGENT;
    }

    AgentId id = m_nextId++;
    AgentData& agent = m_agents[id];
    agent.entity   = entity;
    agent.position = pos;
    agent.radius   = radius;
    agent.speed    = speed;
    agent.hasTarget = false;

    spdlog::debug("[PathfindingSystem] addAgent id={} entity={} pos=({:.1f},{:.1f},{:.1f})",
                  id, static_cast<uint32_t>(entity), pos.x, pos.y, pos.z);
    return id;
}

void PathfindingSystem::removeAgent(AgentId id) {
    if (m_agents.erase(id) == 0) {
        spdlog::warn("[PathfindingSystem] removeAgent — id {} not found", id);
    }
}

void PathfindingSystem::setAgentTarget(AgentId id, glm::vec3 target) {
    auto it = m_agents.find(id);
    if (it == m_agents.end()) {
        spdlog::warn("[PathfindingSystem] setAgentTarget — id {} not found", id);
        return;
    }
    it->second.target    = target;
    it->second.hasTarget = true;
}

void PathfindingSystem::setAgentSpeed(AgentId id, float speed) {
    auto it = m_agents.find(id);
    if (it == m_agents.end()) return;
    it->second.speed = speed;
}

glm::vec3 PathfindingSystem::getAgentPosition(AgentId id) const {
    auto it = m_agents.find(id);
    if (it == m_agents.end()) return glm::vec3(0.0f);
    return it->second.position;
}

glm::vec3 PathfindingSystem::getAgentVelocity(AgentId id) const {
    auto it = m_agents.find(id);
    if (it == m_agents.end()) return glm::vec3(0.0f);
    return it->second.velocity;
}

void PathfindingSystem::update(float dt) {
    if (!m_initialized) return;

    // TODO: m_crowd->update(dt, nullptr)
    //       then sync positions back from dtCrowdAgent

    // Stub: simple straight-line movement toward target
    for (auto& [id, agent] : m_agents) {
        if (!agent.hasTarget) {
            agent.velocity = glm::vec3(0.0f);
            continue;
        }

        glm::vec3 toTarget = agent.target - agent.position;
        float dist = glm::length(toTarget);

        if (dist < 0.1f) {
            agent.hasTarget = false;
            agent.velocity  = glm::vec3(0.0f);
            continue;
        }

        glm::vec3 dir = toTarget / dist;
        float step = std::min(agent.speed * dt, dist);
        agent.velocity  = dir * agent.speed;
        agent.position += dir * step;
    }
}

entt::entity PathfindingSystem::getAgentEntity(AgentId id) const {
    auto it = m_agents.find(id);
    if (it == m_agents.end()) return entt::null;
    return it->second.entity;
}

bool PathfindingSystem::isOnNavMesh(glm::vec3 /*point*/, float /*searchRadius*/) const {
    // TODO: dtNavMeshQuery::findNearestPoly
    // Stub: always return true
    return m_initialized;
}

glm::vec3 PathfindingSystem::findNearestPoint(glm::vec3 point, float /*searchRadius*/) const {
    // TODO: dtNavMeshQuery::findNearestPoly → closestPointOnPoly
    // Stub: return input point unchanged
    return point;
}

} // namespace glory

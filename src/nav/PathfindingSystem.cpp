#include "nav/PathfindingSystem.h"
#include "nav/NavMeshBuilder.h"
#include "nav/DynamicObstacle.h"
#include "map/MapTypes.h"

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
    m_initialized = true;
    spdlog::info("[PathfindingSystem] initialized (stub — straight-line fallback)");
    return true;
}

void PathfindingSystem::shutdown() {
    if (!m_initialized) return;

    m_agents.clear();
    m_nextId = 0;
    m_initialized = false;
    m_flowFieldsReady = false;
    spdlog::info("[PathfindingSystem] shut down");
}

// ── Flow field management ──────────────────────────────────────────────────

void PathfindingSystem::initFlowFields(const MapData& map) {
    glm::vec2 worldMin(map.mapBoundsMin.x, map.mapBoundsMin.z);
    glm::vec2 worldMax(map.mapBoundsMax.x, map.mapBoundsMax.z);

    // Each lane endpoint is the last waypoint of the lane.
    // Blue team flows toward enemy (Red) base → use Red lane endpoints.
    // Red team flows toward enemy (Blue) base → use Blue lane endpoints.
    for (uint32_t li = 0; li < 3; ++li) {
        // Blue team field (index 0-2): goal = end of lane (heading toward red base)
        const auto& blueLane = map.GetLane(TeamID::Blue, static_cast<LaneType>(li));
        uint32_t blueIdx = 0 * 3 + li;  // Blue team
        m_flowFields[blueIdx].init(worldMin, worldMax, 1.0f);
        m_flowFields[blueIdx].setLabel(std::string("Blue_") + (li == 0 ? "Top" : li == 1 ? "Mid" : "Bot"));

        if (!blueLane.waypoints.empty()) {
            auto& goal = blueLane.waypoints.back();
            m_flowFieldGoals[blueIdx] = glm::vec2(goal.x, goal.z);
        } else {
            // Fallback: aim toward opposite base
            m_flowFieldGoals[blueIdx] = glm::vec2(map.mapBoundsMax.x, map.mapBoundsMax.z);
        }

        // Red team field (index 3-5): goal = end of lane (heading toward blue base)
        const auto& redLane = map.GetLane(TeamID::Red, static_cast<LaneType>(li));
        uint32_t redIdx = 1 * 3 + li;  // Red team
        m_flowFields[redIdx].init(worldMin, worldMax, 1.0f);
        m_flowFields[redIdx].setLabel(std::string("Red_") + (li == 0 ? "Top" : li == 1 ? "Mid" : "Bot"));

        if (!redLane.waypoints.empty()) {
            auto& goal = redLane.waypoints.back();
            m_flowFieldGoals[redIdx] = glm::vec2(goal.x, goal.z);
        } else {
            // Fallback: aim toward blue base
            m_flowFieldGoals[redIdx] = glm::vec2(map.mapBoundsMin.x, map.mapBoundsMin.z);
        }
    }

    // Generate all fields
    for (uint32_t i = 0; i < FLOW_FIELD_COUNT; ++i) {
        m_flowFields[i].generate(m_flowFieldGoals[i]);
    }

    m_flowFieldsReady = true;
    spdlog::info("[PathfindingSystem] {} flow fields generated", FLOW_FIELD_COUNT);
}

void PathfindingSystem::updateFlowFields(const DynamicObstacleManager* obstacles) {
    if (!m_flowFieldsReady) return;

    bool anyDirty = false;
    for (auto& ff : m_flowFields) {
        if (obstacles) ff.bakeObstacles(*obstacles);
        if (ff.isDirty()) anyDirty = true;
    }

    if (!anyDirty) return;

    for (uint32_t i = 0; i < FLOW_FIELD_COUNT; ++i) {
        if (m_flowFields[i].isDirty()) {
            m_flowFields[i].generate(m_flowFieldGoals[i]);
        }
    }
}

glm::vec2 PathfindingSystem::sampleFlowField(uint32_t team, uint32_t lane,
                                               glm::vec2 worldXZ) const {
    uint32_t idx = team * 3 + lane;
    if (idx >= FLOW_FIELD_COUNT) return glm::vec2(0.0f);
    return m_flowFields[idx].sample(worldXZ);
}

const FlowField* PathfindingSystem::getFlowField(uint32_t index) const {
    if (index >= FLOW_FIELD_COUNT) return nullptr;
    return &m_flowFields[index];
}

// ── Path queries (for champions) ───────────────────────────────────────────

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

// ── Crowd agent management ─────────────────────────────────────────────────

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

    // TODO: m_crowd->update(dt, nullptr) for Recast/Detour agents
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
    return m_initialized;
}

glm::vec3 PathfindingSystem::findNearestPoint(glm::vec3 point, float /*searchRadius*/) const {
    return point;
}

} // namespace glory

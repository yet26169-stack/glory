#include "nav/PathfindingSystem.h"
#include "nav/NavMeshBuilder.h"
#include "nav/DynamicObstacle.h"
#include "map/MapTypes.h"

#include <spdlog/spdlog.h>
#include <glm/geometric.hpp>

#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>
#include <DetourCrowd.h>
#include <DetourCommon.h>

namespace glory {

bool PathfindingSystem::init(const NavMeshData& navData) {
    if (m_initialized) {
        spdlog::warn("[PathfindingSystem] already initialized — call shutdown() first");
        return false;
    }

    if (!navData.valid || navData.serializedData.empty()) {
        spdlog::error("[PathfindingSystem] init() — invalid navmesh data");
        return false;
    }

    // 1. Create dtNavMesh
    m_navMesh = dtAllocNavMesh();
    if (!m_navMesh) {
        spdlog::error("[PathfindingSystem] Could not allocate navmesh");
        return false;
    }

    unsigned char* data = (unsigned char*)dtAlloc(navData.serializedData.size(), DT_ALLOC_PERM);
    std::memcpy(data, navData.serializedData.data(), navData.serializedData.size());

    dtStatus status = m_navMesh->init(data, (int)navData.serializedData.size(), DT_TILE_FREE_DATA);
    if (dtStatusFailed(status)) {
        dtFree(data);
        spdlog::error("[PathfindingSystem] Could not init navmesh (status=0x{:x})", status);
        return false;
    }

    // 2. Create dtNavMeshQuery
    m_query = dtAllocNavMeshQuery();
    if (!m_query) {
        spdlog::error("[PathfindingSystem] Could not allocate navmesh query");
        return false;
    }
    status = m_query->init(m_navMesh, 2048);
    if (dtStatusFailed(status)) {
        spdlog::error("[PathfindingSystem] Could not init navmesh query (status=0x{:x})", status);
        return false;
    }

    // 3. Create dtCrowd
    m_crowd = dtAllocCrowd();
    if (!m_crowd) {
        spdlog::error("[PathfindingSystem] Could not allocate crowd");
        return false;
    }
    if (!m_crowd->init(128, 5.0f, m_navMesh)) {
        spdlog::error("[PathfindingSystem] Could not init crowd");
        return false;
    }

    m_initialized = true;
    spdlog::info("[PathfindingSystem] initialized successfully");
    return true;
}

void PathfindingSystem::shutdown() {
    if (!m_initialized) return;

    dtFreeCrowd(m_crowd);
    m_crowd = nullptr;
    dtFreeNavMeshQuery(m_query);
    m_query = nullptr;
    dtFreeNavMesh(m_navMesh);
    m_navMesh = nullptr;

    m_agents.clear();
    m_nextId = 0;
    m_initialized = false;
    m_flowFieldsReady = false;
    spdlog::info("[PathfindingSystem] shut down");
}

void PathfindingSystem::initFlowFields(const MapData& map) {
    glm::vec2 worldMin(map.mapBoundsMin.x, map.mapBoundsMin.z);
    glm::vec2 worldMax(map.mapBoundsMax.x, map.mapBoundsMax.z);

    for (uint32_t li = 0; li < 3; ++li) {
        const auto& blueLane = map.GetLane(TeamID::Blue, static_cast<LaneType>(li));
        uint32_t blueIdx = 0 * 3 + li;
        m_flowFields[blueIdx].init(worldMin, worldMax, 1.0f);
        m_flowFields[blueIdx].setLabel(std::string("Blue_") + (li == 0 ? "Top" : li == 1 ? "Mid" : "Bot"));

        if (!blueLane.waypoints.empty()) {
            auto& goal = blueLane.waypoints.back();
            m_flowFieldGoals[blueIdx] = glm::vec2(goal.x, goal.z);
        } else {
            m_flowFieldGoals[blueIdx] = glm::vec2(map.mapBoundsMax.x, map.mapBoundsMax.z);
        }

        const auto& redLane = map.GetLane(TeamID::Red, static_cast<LaneType>(li));
        uint32_t redIdx = 1 * 3 + li;
        m_flowFields[redIdx].init(worldMin, worldMax, 1.0f);
        m_flowFields[redIdx].setLabel(std::string("Red_") + (li == 0 ? "Top" : li == 1 ? "Mid" : "Bot"));

        if (!redLane.waypoints.empty()) {
            auto& goal = redLane.waypoints.back();
            m_flowFieldGoals[redIdx] = glm::vec2(goal.x, goal.z);
        } else {
            m_flowFieldGoals[redIdx] = glm::vec2(map.mapBoundsMin.x, map.mapBoundsMin.z);
        }
    }

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

PathResult PathfindingSystem::findPath(glm::vec3 start, glm::vec3 end) const {
    PathResult result;
    if (!m_initialized || !m_query) return result;

    dtQueryFilter filter;
    filter.setIncludeFlags(0xffff);
    filter.setExcludeFlags(0);

    float extents[3] = {2.0f, 4.0f, 2.0f};
    dtPolyRef startPoly = 0, endPoly = 0;
    float startNearest[3] = {};
    float endNearest[3] = {};

    m_query->findNearestPoly(&start.x, extents, &filter, &startPoly, startNearest);
    m_query->findNearestPoly(&end.x, extents, &filter, &endPoly, endNearest);

    if (!startPoly || !endPoly) return result;

    static const int MAX_POLYS = 256;
    dtPolyRef polys[MAX_POLYS];
    int npolys = 0;

    dtStatus status = m_query->findPath(startPoly, endPoly, startNearest, endNearest, &filter, polys, &npolys, MAX_POLYS);
    if (dtStatusFailed(status) || npolys == 0) return result;

    static const int MAX_STEPS = 256;
    float straightPath[MAX_STEPS * 3];
    unsigned char straightPathFlags[MAX_STEPS];
    dtPolyRef straightPathPolys[MAX_STEPS];
    int nstraight = 0;

    status = m_query->findStraightPath(startNearest, endNearest, polys, npolys, straightPath, straightPathFlags, straightPathPolys, &nstraight, MAX_STEPS);
    if (dtStatusFailed(status) || nstraight == 0) return result;

    result.waypoints.reserve(nstraight);
    for (int i = 0; i < nstraight; ++i) {
        result.waypoints.push_back({straightPath[i * 3], straightPath[i * 3 + 1], straightPath[i * 3 + 2]});
    }
    result.found = true;
    return result;
}

AgentId PathfindingSystem::addAgent(entt::entity entity, glm::vec3 pos,
                                    float radius, float speed) {
    if (!m_initialized || !m_crowd) return INVALID_AGENT;

    dtCrowdAgentParams params;
    std::memset(&params, 0, sizeof(params));
    params.radius = radius;
    params.height = 2.0f;
    params.maxAcceleration = 20.0f;
    params.maxSpeed = speed;
    params.collisionQueryRange = radius * 12.0f;
    params.pathOptimizationRange = radius * 30.0f;
    params.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO |
                         DT_CROWD_OBSTACLE_AVOIDANCE | DT_CROWD_SEPARATION;
    params.obstacleAvoidanceType = 3;
    params.separationWeight = 2.0f;

    int idx = m_crowd->addAgent(&pos.x, &params);
    if (idx == -1) return INVALID_AGENT;

    AgentId id = static_cast<AgentId>(idx);
    m_agents[id].entity = entity;
    m_agents[id].position = pos;
    m_agents[id].speed = speed;
    m_agents[id].hasTarget = false;

    return id;
}

void PathfindingSystem::removeAgent(AgentId id) {
    if (m_initialized && m_crowd) {
        m_crowd->removeAgent(static_cast<int>(id));
    }
    m_agents.erase(id);
}

void PathfindingSystem::setAgentTarget(AgentId id, glm::vec3 target) {
    if (!m_initialized || !m_crowd || !m_query) return;

    dtQueryFilter filter;
    filter.setIncludeFlags(0xffff);
    float extents[3] = {2.0f, 4.0f, 2.0f};
    dtPolyRef targetPoly;
    float targetNearest[3];
    m_query->findNearestPoly(&target.x, extents, &filter, &targetPoly, targetNearest);

    if (targetPoly) {
        m_crowd->requestMoveTarget(static_cast<int>(id), targetPoly, targetNearest);
        m_agents[id].target = target;
        m_agents[id].hasTarget = true;
    }
}

void PathfindingSystem::setAgentSpeed(AgentId id, float speed) {
    if (!m_initialized || !m_crowd) return;
    const dtCrowdAgent* ag = m_crowd->getAgent(static_cast<int>(id));
    if (ag && ag->active) {
        dtCrowdAgentParams params = ag->params;
        params.maxSpeed = speed;
        m_crowd->updateAgentParameters(static_cast<int>(id), &params);
    }
    auto it = m_agents.find(id);
    if (it != m_agents.end()) it->second.speed = speed;
}

glm::vec3 PathfindingSystem::getAgentPosition(AgentId id) const {
    if (m_initialized && m_crowd) {
        const dtCrowdAgent* ag = m_crowd->getAgent(static_cast<int>(id));
        if (ag && ag->active) {
            return {ag->npos[0], ag->npos[1], ag->npos[2]};
        }
    }
    auto it = m_agents.find(id);
    if (it != m_agents.end()) return it->second.position;
    return {0.0f, 0.0f, 0.0f};
}

glm::vec3 PathfindingSystem::getAgentVelocity(AgentId id) const {
    if (m_initialized && m_crowd) {
        const dtCrowdAgent* ag = m_crowd->getAgent(static_cast<int>(id));
        if (ag && ag->active) {
            return {ag->vel[0], ag->vel[1], ag->vel[2]};
        }
    }
    return {0,0,0};
}

void PathfindingSystem::update(float dt) {
    if (!m_initialized || !m_crowd) return;

    m_crowd->update(dt, nullptr);

    for (auto& [id, agent] : m_agents) {
        const dtCrowdAgent* ag = m_crowd->getAgent(static_cast<int>(id));
        if (ag && ag->active) {
            agent.position = {ag->npos[0], ag->npos[1], ag->npos[2]};
            agent.velocity = {ag->vel[0], ag->vel[1], ag->vel[2]};
        }
    }
}

entt::entity PathfindingSystem::getAgentEntity(AgentId id) const {
    auto it = m_agents.find(id);
    if (it == m_agents.end()) return entt::null;
    return it->second.entity;
}

bool PathfindingSystem::isOnNavMesh(glm::vec3 point, float searchRadius) const {
    if (!m_initialized || !m_query) return false;
    dtQueryFilter filter;
    filter.setIncludeFlags(0xffff);
    filter.setExcludeFlags(0);
    float extents[3] = {searchRadius, 4.0f, searchRadius};
    dtPolyRef poly = 0;
    float nearest[3] = {};
    m_query->findNearestPoly(&point.x, extents, &filter, &poly, nearest);
    return poly != 0;
}

glm::vec3 PathfindingSystem::findNearestPoint(glm::vec3 point, float searchRadius) const {
    if (!m_initialized || !m_query) return point;
    dtQueryFilter filter;
    filter.setIncludeFlags(0xffff);
    filter.setExcludeFlags(0);
    float extents[3] = {searchRadius, 4.0f, searchRadius};
    dtPolyRef poly = 0;
    float nearest[3] = {};
    m_query->findNearestPoly(&point.x, extents, &filter, &poly, nearest);
    if (poly) return {nearest[0], nearest[1], nearest[2]};
    return point;
}

} // namespace glory

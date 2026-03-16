#include "nav/FlowField.h"
#include "nav/DynamicObstacle.h"

#include <spdlog/spdlog.h>
#include <glm/geometric.hpp>
#include <queue>
#include <algorithm>
#include <cmath>
#include <limits>

namespace glory {

static constexpr uint32_t COST_UNREACHABLE = std::numeric_limits<uint32_t>::max();

// 8-connected neighbour offsets (dx, dz)
static constexpr int DX[8] = { -1,  0,  1, -1, 1, -1, 0, 1 };
static constexpr int DZ[8] = { -1, -1, -1,  0, 0,  1, 1, 1 };

void FlowField::init(glm::vec2 worldMin, glm::vec2 worldMax, float cellSize) {
    m_worldMin  = worldMin;
    m_worldMax  = worldMax;
    m_cellSize  = cellSize;
    m_gridW     = static_cast<int>(std::ceil((worldMax.x - worldMin.x) / cellSize));
    m_gridH     = static_cast<int>(std::ceil((worldMax.y - worldMin.y) / cellSize));

    size_t count = static_cast<size_t>(m_gridW) * m_gridH;
    m_obstacles.assign(count, 0);
    m_directions.assign(count, glm::vec2(0.0f));
    m_costField.assign(count, COST_UNREACHABLE);
    m_dirty = true;

    spdlog::info("[FlowField] init {}x{} grid  cell={:.1f}  world=[{:.0f},{:.0f}]->[{:.0f},{:.0f}]",
                 m_gridW, m_gridH, cellSize,
                 worldMin.x, worldMin.y, worldMax.x, worldMax.y);
}

int FlowField::worldToCell(float world, float wMin) const {
    return static_cast<int>((world - wMin) / m_cellSize);
}

// ── Obstacle helpers ───────────────────────────────────────────────────────

void FlowField::setObstacleGrid(const std::vector<uint8_t>& grid) {
    if (grid.size() == m_obstacles.size()) {
        m_obstacles = grid;
        m_dirty = true;
    }
}

void FlowField::setObstacle(int cx, int cz, bool blocked) {
    if (!inBounds(cx, cz)) return;
    uint8_t val = blocked ? 1 : 0;
    if (m_obstacles[idx(cx, cz)] != val) {
        m_obstacles[idx(cx, cz)] = val;
        m_dirty = true;
    }
}

void FlowField::bakeObstacles(const DynamicObstacleManager& obstacles) {
    uint32_t ver = obstacles.version();
    if (ver == m_lastObstacleVersion) return;  // no changes
    m_lastObstacleVersion = ver;

    // Clear dynamic obstacles (reset grid to all passable, then re-bake)
    std::fill(m_obstacles.begin(), m_obstacles.end(), uint8_t(0));

    obstacles.forEachObstacle([&](ObstacleId /*id*/, const ObstacleDesc& desc) {
        float r = desc.radius;
        if (desc.shape == ObstacleShape::Box) {
            r = std::max(desc.halfExtents.x, desc.halfExtents.z);
        }

        int minCx = worldToCell(desc.position.x - r, m_worldMin.x);
        int maxCx = worldToCell(desc.position.x + r, m_worldMin.x);
        int minCz = worldToCell(desc.position.z - r, m_worldMin.y);
        int maxCz = worldToCell(desc.position.z + r, m_worldMin.y);

        for (int cz = minCz; cz <= maxCz; ++cz) {
            for (int cx = minCx; cx <= maxCx; ++cx) {
                if (inBounds(cx, cz)) {
                    m_obstacles[idx(cx, cz)] = 1;
                }
            }
        }
    });

    m_dirty = true;
}

// ── BFS Generation ─────────────────────────────────────────────────────────

void FlowField::generate(glm::vec2 goalWorldXZ) {
    int goalCx = worldToCell(goalWorldXZ.x, m_worldMin.x);
    int goalCz = worldToCell(goalWorldXZ.y, m_worldMin.y);

    goalCx = std::clamp(goalCx, 0, m_gridW - 1);
    goalCz = std::clamp(goalCz, 0, m_gridH - 1);

    // Reset cost field
    std::fill(m_costField.begin(), m_costField.end(), COST_UNREACHABLE);
    std::fill(m_directions.begin(), m_directions.end(), glm::vec2(0.0f));

    // BFS from goal (multi-source possible — single goal here)
    std::queue<int> frontier;

    int goalIdx = idx(goalCx, goalCz);
    m_costField[goalIdx] = 0;
    frontier.push(goalIdx);

    while (!frontier.empty()) {
        int ci = frontier.front();
        frontier.pop();

        int cx = ci % m_gridW;
        int cz = ci / m_gridW;
        uint32_t cost = m_costField[ci];

        for (int n = 0; n < 8; ++n) {
            int nx = cx + DX[n];
            int nz = cz + DZ[n];
            if (!inBounds(nx, nz)) continue;

            int ni = idx(nx, nz);
            if (m_obstacles[ni]) continue;

            // Diagonal cost 14, cardinal cost 10 (x10 for integer BFS)
            uint32_t moveCost = (DX[n] != 0 && DZ[n] != 0) ? 14 : 10;
            uint32_t newCost = cost + moveCost;

            if (newCost < m_costField[ni]) {
                m_costField[ni] = newCost;
                frontier.push(ni);
            }
        }
    }

    // Build direction field: each cell points toward its lowest-cost neighbour
    for (int cz = 0; cz < m_gridH; ++cz) {
        for (int cx = 0; cx < m_gridW; ++cx) {
            int ci = idx(cx, cz);
            if (m_costField[ci] == COST_UNREACHABLE || m_costField[ci] == 0) {
                m_directions[ci] = glm::vec2(0.0f);
                continue;
            }

            uint32_t bestCost = m_costField[ci];
            int bestDx = 0, bestDz = 0;

            for (int n = 0; n < 8; ++n) {
                int nx = cx + DX[n];
                int nz = cz + DZ[n];
                if (!inBounds(nx, nz)) continue;

                uint32_t nc = m_costField[idx(nx, nz)];
                if (nc < bestCost) {
                    bestCost = nc;
                    bestDx = DX[n];
                    bestDz = DZ[n];
                }
            }

            glm::vec2 dir(static_cast<float>(bestDx), static_cast<float>(bestDz));
            float len = glm::length(dir);
            m_directions[ci] = (len > 0.0f) ? dir / len : glm::vec2(0.0f);
        }
    }

    m_dirty = false;
}

// ── Sampling ───────────────────────────────────────────────────────────────

glm::vec2 FlowField::sample(glm::vec2 worldXZ) const {
    int cx = worldToCell(worldXZ.x, m_worldMin.x);
    int cz = worldToCell(worldXZ.y, m_worldMin.y);
    if (!inBounds(cx, cz)) return glm::vec2(0.0f);
    return m_directions[idx(cx, cz)];
}

glm::vec2 FlowField::cellDirection(int cx, int cz) const {
    if (!inBounds(cx, cz)) return glm::vec2(0.0f);
    return m_directions[idx(cx, cz)];
}

bool FlowField::isPassable(int cx, int cz) const {
    if (!inBounds(cx, cz)) return false;
    return m_obstacles[idx(cx, cz)] == 0;
}

} // namespace glory

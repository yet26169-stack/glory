#include "nav/FlowField.h"
#include <queue>
#include <limits>
#include <cmath>

namespace glory {

void FlowField::build(uint32_t gridW, uint32_t gridH,
                      const std::vector<uint8_t>& costMap,
                      uint32_t goalX, uint32_t goalZ) {
    m_w = gridW;
    m_h = gridH;
    m_cells.assign(gridW * gridH, FlowCell{0, 0, 0xFFFF});
    m_built = false;

    if (goalX >= gridW || goalZ >= gridH) return;

    // Backward Dijkstra from the goal cell
    using Entry = std::pair<uint32_t, int>; // (cost, cell index)
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> pq;

    auto& goalCell = m_cells[goalZ * gridW + goalX];
    goalCell.cost = 0;
    pq.push({0, static_cast<int>(goalZ * gridW + goalX)});

    static constexpr int DX[] = {-1, 1,  0, 0, -1,  1, -1,  1};
    static constexpr int DZ[] = { 0, 0, -1, 1, -1, -1,  1,  1};
    static constexpr uint16_t COSTS[] = {10, 10, 10, 10, 14, 14, 14, 14}; // cardinal 10, diagonal 14

    while (!pq.empty()) {
        auto [curCost, idx] = pq.top(); pq.pop();
        int cx = idx % static_cast<int>(gridW);
        int cz = idx / static_cast<int>(gridW);

        if (curCost > m_cells[idx].cost) continue; // stale entry

        for (int d = 0; d < 8; ++d) {
            int nx = cx + DX[d];
            int nz = cz + DZ[d];
            if (!inBounds(nx, nz)) continue;
            int ni = cellIndex(nx, nz);
            if (costMap[ni] == 255) continue; // wall

            uint32_t newCost = curCost + COSTS[d] + costMap[ni];
            if (newCost < m_cells[ni].cost) {
                m_cells[ni].cost = static_cast<uint16_t>(std::min(newCost, (uint32_t)0xFFFEu));
                pq.push({m_cells[ni].cost, ni});
            }
        }
    }

    // Compute flow directions: each non-goal cell points toward its lowest-cost neighbour
    for (uint32_t z = 0; z < gridH; ++z) {
        for (uint32_t x = 0; x < gridW; ++x) {
            int idx = cellIndex(static_cast<int>(x), static_cast<int>(z));
            if (m_cells[idx].cost == 0xFFFF) continue; // impassable

            int bestDx = 0, bestDz = 0;
            uint16_t bestCost = m_cells[idx].cost;

            for (int d = 0; d < 8; ++d) {
                int nx = static_cast<int>(x) + DX[d];
                int nz = static_cast<int>(z) + DZ[d];
                if (!inBounds(nx, nz)) continue;
                int ni = cellIndex(nx, nz);
                if (m_cells[ni].cost < bestCost) {
                    bestCost = m_cells[ni].cost;
                    bestDx = DX[d];
                    bestDz = DZ[d];
                }
            }
            m_cells[idx].dx = static_cast<int8_t>(bestDx);
            m_cells[idx].dz = static_cast<int8_t>(bestDz);
        }
    }

    m_built = true;
}

glm::vec2 FlowField::getDirection(float worldX, float worldZ) const {
    if (!m_built) return glm::vec2(0.0f);
    int cx = static_cast<int>(worldX / CELL_SIZE);
    int cz = static_cast<int>(worldZ / CELL_SIZE);
    if (!inBounds(cx, cz)) return glm::vec2(0.0f);
    const auto& cell = m_cells[cellIndex(cx, cz)];
    if (cell.cost == 0xFFFF) return glm::vec2(0.0f);
    return glm::normalize(glm::vec2(cell.dx, cell.dz) + glm::vec2(0.0f)); // already unit-ish
}

} // namespace glory

#include "nav/HierarchicalFlowField.h"
#include <algorithm>

namespace glory {

void HierarchicalFlowField::build(uint32_t gridW, uint32_t gridH,
                                   const std::vector<uint8_t>& costMap,
                                   float goalWorldX, float goalWorldZ) {
    m_gridW = gridW;
    m_gridH = gridH;

    uint32_t cW = numClustersX();
    uint32_t cH = numClustersZ();

    // Build coarse field: one cell per cluster, averaged passability
    std::vector<uint8_t> coarseCost(cW * cH, 0);
    for (uint32_t cz = 0; cz < cH; ++cz) {
        for (uint32_t cx = 0; cx < cW; ++cx) {
            uint32_t wallCount = 0, total = 0;
            for (int dz = 0; dz < CLUSTER_SIZE; ++dz) {
                for (int dx = 0; dx < CLUSTER_SIZE; ++dx) {
                    uint32_t fx = cx * CLUSTER_SIZE + dx;
                    uint32_t fz = cz * CLUSTER_SIZE + dz;
                    if (fx < gridW && fz < gridH) {
                        if (costMap[fz * gridW + fx] == 255) ++wallCount;
                        ++total;
                    }
                }
            }
            coarseCost[cz * cW + cx] = (total > 0 && wallCount == total) ? 255 : 0;
        }
    }

    uint32_t goalCX = static_cast<uint32_t>(goalWorldX / (FlowField::CELL_SIZE * CLUSTER_SIZE));
    uint32_t goalCZ = static_cast<uint32_t>(goalWorldZ / (FlowField::CELL_SIZE * CLUSTER_SIZE));
    goalCX = std::min(goalCX, cW - 1);
    goalCZ = std::min(goalCZ, cH - 1);
    m_coarseField.build(cW, cH, coarseCost, goalCX, goalCZ);

    // Build one fine field per cluster
    uint32_t goalCellX = static_cast<uint32_t>(goalWorldX / FlowField::CELL_SIZE);
    uint32_t goalCellZ = static_cast<uint32_t>(goalWorldZ / FlowField::CELL_SIZE);
    goalCellX = std::min(goalCellX, gridW - 1);
    goalCellZ = std::min(goalCellZ, gridH - 1);

    m_fineFields.resize(cW * cH);
    for (uint32_t cz = 0; cz < cH; ++cz) {
        for (uint32_t cx = 0; cx < cW; ++cx) {
            // Extract cluster sub-map
            uint32_t fw = std::min(CLUSTER_SIZE, static_cast<int>(gridW - cx * CLUSTER_SIZE));
            uint32_t fh = std::min(CLUSTER_SIZE, static_cast<int>(gridH - cz * CLUSTER_SIZE));
            std::vector<uint8_t> sub(fw * fh);
            for (uint32_t dz = 0; dz < fh; ++dz) {
                for (uint32_t dx = 0; dx < fw; ++dx) {
                    uint32_t gx = cx * CLUSTER_SIZE + dx;
                    uint32_t gz = cz * CLUSTER_SIZE + dz;
                    sub[dz * fw + dx] = costMap[gz * gridW + gx];
                }
            }
            // Compute local goal within this cluster
            uint32_t localGoalX = goalCellX >= cx * CLUSTER_SIZE
                ? std::min(goalCellX - cx * CLUSTER_SIZE, fw - 1) : 0;
            uint32_t localGoalZ = goalCellZ >= cz * CLUSTER_SIZE
                ? std::min(goalCellZ - cz * CLUSTER_SIZE, fh - 1) : 0;

            auto ff = std::make_unique<FlowField>();
            ff->build(fw, fh, sub, localGoalX, localGoalZ);
            ff->id = cz * cW + cx;
            m_fineFields[cz * cW + cx] = std::move(ff);
        }
    }
}

glm::vec2 HierarchicalFlowField::getDirection(float worldX, float worldZ) const {
    if (m_fineFields.empty()) return glm::vec2(0.0f);
    uint32_t cW = numClustersX();

    uint32_t cx = static_cast<uint32_t>(worldX / (FlowField::CELL_SIZE * CLUSTER_SIZE));
    uint32_t cz = static_cast<uint32_t>(worldZ / (FlowField::CELL_SIZE * CLUSTER_SIZE));
    cx = std::min(cx, numClustersX() - 1);
    cz = std::min(cz, numClustersZ() - 1);

    const auto& ff = m_fineFields[cz * cW + cx];
    if (!ff || !ff->isBuilt()) return m_coarseField.getDirection(worldX, worldZ);

    // Local coordinate within cluster
    float localX = worldX - static_cast<float>(cx * CLUSTER_SIZE * FlowField::CELL_SIZE);
    float localZ = worldZ - static_cast<float>(cz * CLUSTER_SIZE * FlowField::CELL_SIZE);
    return ff->getDirection(localX, localZ);
}

} // namespace glory

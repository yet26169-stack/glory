#pragma once
#include "nav/FlowField.h"
#include <vector>
#include <memory>
#include <glm/glm.hpp>

namespace glory {

/// Two-level hierarchical flow field.
/// Reduces build time from O(N²) to O(N) for large maps (> 200×200 cells)
/// by using a coarse 8×8-cell cluster grid plus per-cluster fine fields.
class HierarchicalFlowField {
public:
    static constexpr int CLUSTER_SIZE = 8; // cells per cluster side

    void build(uint32_t gridW, uint32_t gridH,
               const std::vector<uint8_t>& costMap,
               float goalWorldX, float goalWorldZ);

    glm::vec2 getDirection(float worldX, float worldZ) const;

private:
    uint32_t  m_gridW = 0, m_gridH = 0;
    FlowField m_coarseField;
    std::vector<std::unique_ptr<FlowField>> m_fineFields;

    uint32_t numClustersX() const { return (m_gridW + CLUSTER_SIZE - 1) / CLUSTER_SIZE; }
    uint32_t numClustersZ() const { return (m_gridH + CLUSTER_SIZE - 1) / CLUSTER_SIZE; }
};

} // namespace glory

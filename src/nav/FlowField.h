#pragma once
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

namespace glory {

/// One cell of a flow field — stores the best-direction vector and Dijkstra cost.
struct FlowCell {
    int8_t   dx, dz;   // best direction to goal: each in {-1, 0, +1}
    uint16_t cost;     // Dijkstra cost from goal (65535 = impassable)
};

/// CPU flow field built via backward Dijkstra from a goal cell.
/// Used for hero group movement and jungle monster roaming.
/// Lane minions continue to use LaneFollower (src/nav/LaneFollower.h).
class FlowField {
public:
    static constexpr int CELL_SIZE = 2; // world units per cell

    /// Build the field. costMap values: 0 = passable, 255 = wall.
    void build(uint32_t gridW, uint32_t gridH,
               const std::vector<uint8_t>& costMap,
               uint32_t goalX, uint32_t goalZ);

    /// Sample the flow direction at a world-space position.
    glm::vec2 getDirection(float worldX, float worldZ) const;

    bool isBuilt() const { return m_built; }

    uint32_t id = 0; // matches FlowFieldAgent::flowFieldID

private:
    uint32_t              m_w = 0, m_h = 0;
    std::vector<FlowCell> m_cells;
    bool                  m_built = false;

    int cellIndex(int x, int z) const { return z * static_cast<int>(m_w) + x; }
    bool inBounds(int x, int z) const {
        return x >= 0 && z >= 0 &&
               static_cast<uint32_t>(x) < m_w &&
               static_cast<uint32_t>(z) < m_h;
    }
};

} // namespace glory

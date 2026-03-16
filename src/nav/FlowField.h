#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>
#include <string>

namespace glory {

class DynamicObstacleManager;

/// Flow field for efficient group pathfinding.
///
/// A 2D grid over the XZ plane.  generate() runs BFS from a goal cell,
/// producing a normalised direction vector per cell that always points
/// toward the goal.  Minions simply sample the field at their position
/// to get a steering direction — O(1) per query regardless of wave size.
///
/// Multiple FlowField instances can coexist (one per lane endpoint).
class FlowField {
public:
    FlowField() = default;

    /// Initialise the grid.  worldMin/worldMax are the XZ bounds.
    void init(glm::vec2 worldMin, glm::vec2 worldMax, float cellSize = 1.0f);

    /// Mark the field as needing regeneration.
    void markDirty() { m_dirty = true; }
    bool isDirty() const { return m_dirty; }

    /// Set obstacle grid.  true = blocked.  Must match grid dimensions.
    void setObstacleGrid(const std::vector<uint8_t>& grid);

    /// Set a single cell as blocked/free.
    void setObstacle(int cx, int cz, bool blocked);

    /// Bake obstacles from the DynamicObstacleManager into the grid.
    void bakeObstacles(const DynamicObstacleManager& obstacles);

    /// (Re-)generate the field toward a world-space goal.
    void generate(glm::vec2 goalWorldXZ);

    /// Sample the direction at a world-space XZ position.
    /// Returns a normalised 2D direction or (0,0) if outside bounds.
    glm::vec2 sample(glm::vec2 worldXZ) const;

    /// Read a direction at a specific grid cell.
    glm::vec2 cellDirection(int cx, int cz) const;

    /// True if the cell at (cx,cz) is passable.
    bool isPassable(int cx, int cz) const;

    // ── Accessors ──────────────────────────────────────────────────────
    int       gridW()     const { return m_gridW; }
    int       gridH()     const { return m_gridH; }
    float     cellSize()  const { return m_cellSize; }
    glm::vec2 worldMin()  const { return m_worldMin; }
    glm::vec2 worldMax()  const { return m_worldMax; }

    const std::vector<glm::vec2>& directions() const { return m_directions; }

    /// Optional label for debug display.
    void        setLabel(const std::string& l) { m_label = l; }
    const std::string& label() const { return m_label; }

private:
    int   m_gridW    = 0;
    int   m_gridH    = 0;
    float m_cellSize = 1.0f;
    glm::vec2 m_worldMin{0.0f};
    glm::vec2 m_worldMax{0.0f};

    std::vector<uint8_t>  m_obstacles;   // 1 = blocked
    std::vector<glm::vec2> m_directions; // normalised per cell
    std::vector<uint32_t>  m_costField;  // BFS cost (UINT32_MAX = unreachable)

    bool m_dirty = true;
    uint32_t m_lastObstacleVersion = 0;
    std::string m_label;

    int worldToCell(float world, float worldMin) const;
    int idx(int cx, int cz) const { return cz * m_gridW + cx; }
    bool inBounds(int cx, int cz) const {
        return cx >= 0 && cx < m_gridW && cz >= 0 && cz < m_gridH;
    }
};

} // namespace glory

#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <cstdint>

namespace glory {

struct VisionEntity {
    glm::vec3 position;              // World XZ used (Y ignored)
    float     sightRange = 8.0f;     // Vision radius in world units
};

// CPU-side Fog of War system.
// Maintains a 128×128 visibility grid and an exploration (shroud) grid.
// Call update() each frame with friendly unit positions; the resulting
// getVisibilityBuffer() is then uploaded to the GPU by FogOfWarRenderer.
//
// Grid layout: buffer[z * MAP_SIZE + x], matches Vulkan texture layout
// (row 0 = world Z = -100, row 127 = world Z = +100; left = X = -100).
class FogSystem {
public:
    static constexpr uint32_t MAP_SIZE   = 128;
    static constexpr float    WORLD_SIZE = 200.0f;  // total world extent
    static constexpr float    WORLD_MIN  =   0.0f;  // world-space origin of grid

    FogSystem();

    // Recompute current vision, update exploration shroud, build combined buffer.
    // Call once per game frame before FogOfWarRenderer::updateVisibility().
    void update(const std::vector<VisionEntity>& entities);

    // CPU-only variant (used by unit tests — no Vulkan).
    void updateCpuOnly(const std::vector<VisionEntity>& entities);

    // Combined buffer: 255 = currently visible, 127 = explored (shroud), 0 = unexplored.
    const std::vector<uint8_t>& getVisibilityBuffer() const { return m_visBuffer; }

    // Individual buffers (accessible by tests via non-const cast)
    const std::vector<uint8_t>& getVisionBuffer()       const { return m_visionBuffer; }
    const std::vector<uint8_t>& getExplorationBuffer()  const { return m_explorationBuffer; }

    bool isPositionVisible (float worldX, float worldZ) const;
    bool isPositionExplored(float worldX, float worldZ) const;

private:
    std::vector<uint8_t> m_visionBuffer;      // current frame: 0 or 255
    std::vector<uint8_t> m_explorationBuffer; // cumulative: 0 or 127
    std::vector<uint8_t> m_visBuffer;         // combined output uploaded to GPU

    void paintCircle(float worldX, float worldZ, float radius);
    int  worldToCell(float worldCoord) const;
    void buildCombinedBuffer();
};

} // namespace glory

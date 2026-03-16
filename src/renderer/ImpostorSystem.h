#pragma once

#include "renderer/Buffer.h"
#include "renderer/Texture.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace glory {

class Device;
struct RenderFormats;

/// Descriptor for a single impostor unit type in the atlas.
struct ImpostorEntry {
    uint32_t atlasIndex  = 0;  // which atlas page
    uint32_t startCol    = 0;  // first column in atlas grid
    uint32_t startRow    = 0;  // first row
    uint32_t angleCount  = 8;  // number of pre-rendered angles
    float    worldHeight = 2.0f; // unit height for billboard sizing
    float    worldWidth  = 1.5f; // unit width
};

/// Per-instance impostor data pushed to the GPU each frame.
struct ImpostorInstance {
    glm::vec3 worldPos;
    float     scale;
    glm::vec4 uvRect;   // x,y = min UV, z,w = max UV
    glm::vec4 tint;
};

/// Pre-renders unit types from multiple angles into a texture atlas,
/// then at runtime renders distant entities as screen-aligned billboard quads.
///
/// Integration:
///   - Renderer populates instances each frame for entities beyond LOD impostor distance.
///   - ImpostorSystem selects the closest pre-rendered angle based on camera direction.
///   - Quads are drawn in a single instanced draw call after the main geometry pass.
class ImpostorSystem {
public:
    ImpostorSystem() = default;
    ~ImpostorSystem() = default;

    /// Initialise GPU resources (quad VBO, pipeline, atlas storage).
    void init(const Device& device, const RenderFormats& formats);
    void cleanup();

    /// Register a unit type for impostor rendering.
    /// modelName is the asset key; worldWidth/Height determine billboard sizing.
    void registerUnitType(const std::string& modelName, float worldWidth,
                          float worldHeight, uint32_t angleCount = 8);

    /// Generate the impostor atlas by rendering each registered type from
    /// `angleCount` evenly spaced angles.  Call once after all types registered
    /// and scene meshes are loaded.
    ///
    /// For now this creates a placeholder solid-color atlas.  Full off-screen
    /// rendering of unit meshes will be added when the off-screen render-target
    /// infrastructure is in place.
    void generateAtlas();

    /// Look up impostor entry for a model name.
    const ImpostorEntry* getEntry(const std::string& modelName) const;

    /// Build an ImpostorInstance for a given entity, selecting the best
    /// pre-rendered angle based on the camera→entity direction.
    ImpostorInstance buildInstance(const std::string& modelName,
                                  glm::vec3 worldPos,
                                  glm::vec3 cameraPos,
                                  glm::vec4 tint = glm::vec4(1.0f)) const;

    /// Submit a batch of impostor instances for rendering this frame.
    void beginFrame();
    void addInstance(const ImpostorInstance& inst);

    /// Record draw commands into the command buffer.
    void render(VkCommandBuffer cmd, const glm::mat4& viewProj);

    uint32_t instanceCount() const { return static_cast<uint32_t>(m_frameInstances.size()); }
    bool isInitialized() const { return m_initialized; }

private:
    const Device* m_device = nullptr;
    bool m_initialized = false;

    // ── Atlas ────────────────────────────────────────────────────────────
    static constexpr uint32_t ATLAS_SIZE     = 2048; // px
    static constexpr uint32_t CELL_SIZE      = 128;  // px per angle view
    static constexpr uint32_t CELLS_PER_ROW  = ATLAS_SIZE / CELL_SIZE; // 16

    Texture m_atlas;
    std::unordered_map<std::string, ImpostorEntry> m_entries;
    uint32_t m_nextCell = 0; // allocation cursor in atlas grid

    // ── Per-frame instance data ──────────────────────────────────────────
    std::vector<ImpostorInstance> m_frameInstances;
    Buffer m_instanceBuffer;
    size_t m_instanceCapacity = 0;
    static constexpr size_t INITIAL_INSTANCE_CAP = 1024;

    // ── Pipeline ─────────────────────────────────────────────────────────
    VkPipeline       m_pipeline       = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_dsLayout  = VK_NULL_HANDLE;
    VkDescriptorPool m_descPool       = VK_NULL_HANDLE;
    VkDescriptorSet  m_descSet        = VK_NULL_HANDLE;

    void createPipeline(const RenderFormats& formats);
    void ensureInstanceCapacity(size_t needed);

    float angleFromDirection(glm::vec2 dir) const;
    uint32_t bestAngleIndex(float angleRad, uint32_t angleCount) const;
    glm::vec4 cellUVRect(uint32_t cellIndex) const;
};

} // namespace glory

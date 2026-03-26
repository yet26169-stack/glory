#pragma once

#include "renderer/MegaBuffer.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>
#include <string>

namespace glory {

/// Single LOD level — points into the mega-buffer (or individual buffers).
struct LODLevel {
    float     maxDistance = 0.0f; // switch to next LOD beyond this distance
    uint32_t  vertexOffset = 0;
    uint32_t  indexOffset  = 0;
    uint32_t  indexCount   = 0;
};

/// Per-mesh LOD chain (LOD 0 = full detail, last = coarsest).
/// The chain ends with a sentinel whose maxDistance = FLT_MAX.
struct LODChain {
    std::vector<LODLevel> levels; // sorted by ascending maxDistance
};

/// Distance thresholds per quality preset.
struct LODConfig {
    float lod1Distance = 30.0f;  // switch to 50%
    float lod2Distance = 50.0f;  // switch to 25%
    float lod3Distance = 70.0f;  // switch to 12.5%
};

/// Runtime LOD selection system.
///
/// Each loaded mesh (from the cooked asset pipeline or GLBLoader) registers
/// a LODChain.  Every frame, selectLOD() returns the appropriate MeshHandle
/// offsets for a given camera distance.
///
/// Works with the GPU-driven pipeline: the selected LOD's mesh offsets are
/// written into the per-object GpuObjectData scene buffer SSBO so that
/// occlusion_cull.comp and vkCmdDrawIndexedIndirectCount use the correct
/// geometry automatically.
class LODSystem {
public:
    LODSystem() = default;

    /// Load distance thresholds from a JSON config file.
    /// Falls back to built-in defaults if the file doesn't exist.
    void loadConfig(const std::string& configPath);

    /// Get the active config (for reading thresholds).
    const LODConfig& config() const { return m_configs[static_cast<int>(m_quality)]; }

    /// Register a LODChain for a (modelIndex, subMeshIndex) pair.
    /// The base MeshHandle (LOD 0) is required; additional LOD levels are
    /// appended as suballocated handles from the mega-buffer or individually.
    void registerChain(uint32_t modelIndex, uint32_t subMeshIndex,
                       const LODChain& chain);

    /// Select the LOD level for a given camera distance.
    /// Returns the index into the LODChain (0 = full, N = coarsest).
    int selectLOD(uint32_t modelIndex, uint32_t subMeshIndex,
                  float distance) const;

    /// Convenience: get the MeshHandle-like offsets for the selected LOD.
    /// Returns LOD 0 if no chain is registered.
    LODLevel getLODLevel(uint32_t modelIndex, uint32_t subMeshIndex,
                         float distance) const;

    /// Total registered chains.
    uint32_t chainCount() const { return m_totalChains; }

    /// Set render quality — changes distance thresholds.
    void setQuality(int quality) { m_quality = quality; }
    int  quality() const { return m_quality; }

private:
    // Per quality level: MOBA_PERFORMANCE=0, HIGH_QUALITY=1, ULTRA=2
    LODConfig m_configs[3] = {
        { 25.0f, 40.0f, 55.0f },   // performance — aggressive
        { 35.0f, 55.0f, 75.0f },   // high quality
        { 50.0f, 80.0f, 110.0f },  // ultra
    };

    int m_quality = 0; // default: MOBA_PERFORMANCE

    // Keyed by (modelIndex * MAX_SUBMESHES + subMeshIndex)
    static constexpr uint32_t MAX_SUBMESHES = 32;
    std::vector<LODChain> m_chains; // sparse, indexed by key
    uint32_t m_totalChains = 0;

    uint32_t key(uint32_t model, uint32_t sub) const {
        return model * MAX_SUBMESHES + sub;
    }
};

} // namespace glory

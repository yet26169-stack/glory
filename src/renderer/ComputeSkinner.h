#pragma once
// ── Compute Skinner ──────────────────────────────────────────────────────
// Dispatches a compute shader each frame to pre-skin character vertices.
// The result is written into a device-local output buffer that the vertex
// shader reads as a plain Vertex buffer (no bone indices / weights at draw
// time), reducing vertex shader complexity and bandwidth for large minion waves.
//
// Activation threshold: when > COMPUTE_SKIN_THRESHOLD skinned entities
// are visible, use compute pre-skinning; otherwise fall back to the existing
// vertex-shader skinning path (bone SSBO at binding 4).

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace glory {

class Device;

// Above this count the compute path is more efficient
static constexpr uint32_t COMPUTE_SKIN_THRESHOLD = 50;

struct SkinBatch {
    VkBuffer      bindPoseBuffer = VK_NULL_HANDLE; // StaticSkinnedMesh vertex buf
    VkBuffer      outputBuffer   = VK_NULL_HANDLE; // pre-skinned vertex buf (device-local)
    VmaAllocation outputAlloc    = VK_NULL_HANDLE;
    VkBuffer      boneBuffer     = VK_NULL_HANDLE; // ring-buffer SSBO binding
    uint32_t      vertexCount    = 0;
    uint32_t      boneBaseIndex  = 0; // offset into ring buffer
    uint32_t      entityIndex    = 0; // for draw-call association
};

class ComputeSkinner {
public:
    ComputeSkinner() = default;
    ~ComputeSkinner() { destroy(); }

    ComputeSkinner(const ComputeSkinner&)            = delete;
    ComputeSkinner& operator=(const ComputeSkinner&) = delete;

    void init(const Device& device, uint32_t framesInFlight);
    void destroy();

    // Per-frame: build a batch list, then dispatch all at once.
    // batches[] are populated by the caller (Renderer) before calling dispatch.
    void dispatch(VkCommandBuffer cmd,
                  const std::vector<SkinBatch>& batches,
                  uint32_t frameIdx);

    // Barrier: compute write → vertex read (insert after dispatch, before scene pass)
    void insertOutputBarrier(VkCommandBuffer cmd,
                             const std::vector<SkinBatch>& batches);

    // Allocate a persistent per-mesh output buffer (device-local, STORAGE + VERTEX usage)
    // Returns VkBuffer + VmaAllocation. Caller owns lifetime.
    static VkBuffer allocateOutputBuffer(const Device& device,
                                         uint32_t vertexCount,
                                         VmaAllocation& outAlloc);

    bool isInitialised() const { return m_pipeline != VK_NULL_HANDLE; }

private:
    void createPipeline();
    void createDescriptorPool(uint32_t maxBatchesPerFrame, uint32_t frames);

    VkDescriptorSet acquireDescSet(uint32_t frameIdx);
    void            resetFrame(uint32_t frameIdx);

    const Device*    m_device   = nullptr;
    VkDevice         m_vkDevice = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_descLayout  = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool    = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipeLayout  = VK_NULL_HANDLE;
    VkPipeline            m_pipeline    = VK_NULL_HANDLE;

    // Per-frame pool of pre-allocated descriptor sets (reset each frame)
    struct FramePool {
        std::vector<VkDescriptorSet> sets;
        uint32_t nextSet = 0;
    };
    std::vector<FramePool> m_framePools;

    uint32_t m_maxBatchesPerFrame = 256; // supports 256 skinned entities per frame
    uint32_t m_framesCount        = 0;
};

} // namespace glory

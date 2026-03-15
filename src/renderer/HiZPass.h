#pragma once

#include "renderer/Device.h"
#include "renderer/Buffer.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace glory {

// Hierarchical Z-Buffer for GPU occlusion culling.
//
// Pipeline:
//  1. Depth prepass writes full-resolution depth
//  2. HiZPass generates a mip-chain (max-depth downsampling)
//  3. GpuCuller reads the Hi-Z pyramid to cull per-object AABBs
//
// This class manages the Hi-Z image/pyramid and the mipmap generation compute pass.
// The depth prepass itself is recorded by the caller (just a depth-only render pass).
class HiZPass {
public:
    HiZPass() = default;

    // Initialize with the resolution matching the main depth buffer.
    void init(const Device& device, uint32_t width, uint32_t height);
    void destroy();

    // Rebuild if resolution changes (window resize).
    void resize(uint32_t width, uint32_t height);

    // Generate the Hi-Z mipmap chain from the source depth buffer.
    // sourceDepthView: the full-resolution depth buffer image view to read from.
    // Must be called outside of a render pass.
    void generate(VkCommandBuffer cmd, VkImageView sourceDepthView);

    VkImageView   getPyramidView() const { return m_pyramidView; }
    VkSampler     getSampler()     const { return m_sampler; }
    uint32_t      getMipLevels()   const { return m_mipLevels; }
    uint32_t      getWidth()       const { return m_width; }
    uint32_t      getHeight()      const { return m_height; }

private:
    const Device* m_device = nullptr;
    uint32_t m_width  = 0;
    uint32_t m_height = 0;
    uint32_t m_mipLevels = 0;

    // Hi-Z pyramid image (R32_SFLOAT, full mip chain)
    VkImage       m_pyramidImage = VK_NULL_HANDLE;
    VmaAllocation m_pyramidAlloc = VK_NULL_HANDLE;
    VkImageView   m_pyramidView  = VK_NULL_HANDLE; // all mips

    // Per-mip views for compute storage writes
    std::vector<VkImageView> m_mipViews;

    // Sampler for reading the pyramid
    VkSampler m_sampler = VK_NULL_HANDLE;

    // Compute pipeline for mipmap generation
    VkDescriptorSetLayout m_descLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool   = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_descSets; // one per mip transition
    VkPipelineLayout      m_pipeLayout = VK_NULL_HANDLE;
    VkPipeline            m_pipeline   = VK_NULL_HANDLE;

    void createPyramidImage();
    void createMipViews();
    void createSampler();
    void createComputePipeline();
    void createDescriptors();
    void destroyResources();
};

// GPU Occlusion Culler — reads the Hi-Z pyramid and produces an indirect draw buffer.
// Objects with AABBs that are occluded by the Hi-Z data are culled.
class GpuCuller {
public:
    GpuCuller() = default;

    void init(const Device& device, uint32_t maxObjects);
    void destroy();

    // GPU-side cull parameters (std140 layout, written per-frame)
    struct GpuCullParams {
        glm::mat4 viewProj;          // 64 bytes
        glm::vec4 frustumPlanes[6];  // 96 bytes
        glm::vec4 screenSize;        // 16 bytes  (x=w, y=h, z=1/w, w=1/h)
        uint32_t  objectCount;       // 4 bytes
        uint32_t  phase;             // 4 bytes
        uint32_t  _pad[2];           // 8 bytes  (round to 16-byte std140 alignment)
    }; // total = 192 bytes

    struct CullParams {
        glm::mat4 viewProj;
        glm::vec4 frustumPlanes[6];
        uint32_t  screenWidth;
        uint32_t  screenHeight;
        uint32_t  objectCount;
        uint32_t  phase; // 0 = prev HiZ, 1 = new HiZ (disocclusion)
    };

    // Dispatch the cull compute shader.
    // sceneBuffer: the per-object SSBO (same buffer as Descriptors binding 7).
    void dispatch(VkCommandBuffer cmd, uint32_t frameIndex,
                  VkBuffer sceneBuffer, VkDeviceSize sceneBufferSize,
                  VkImageView hizView, VkSampler hizSampler,
                  const CullParams& params);

    // The output indirect draw buffer for vkCmdDrawIndexedIndirectCount.
    // Commands start at offset sizeof(uint32_t) (byte 4); drawCount at offset 0.
    VkBuffer     getIndirectBuffer(uint32_t frameIndex) const;
    VkDeviceSize getIndirectOffset() const { return sizeof(uint32_t); }

    VkBuffer     getCountBuffer(uint32_t frameIndex) const;
    VkDeviceSize getCountOffset()  const { return 0; }

    uint32_t getMaxObjects() const { return m_maxObjects; }

private:
    const Device* m_device = nullptr;
    uint32_t m_maxObjects = 0;

    // Per-frame resources (double-buffered)
    struct FrameResources {
        Buffer drawBuffer;       // [uint32_t drawCount, DrawCommand[maxObjects]]
        Buffer visibilityFlags;  // uint32_t[maxObjects] (phase 0 writes, phase 1 reads)
        Buffer cullParamsBuffer; // GpuCullParams UBO, CPU→GPU
    };
    std::vector<FrameResources> m_frames;

    VkDescriptorSetLayout m_descLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool   = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_descSets;
    VkPipelineLayout      m_pipeLayout = VK_NULL_HANDLE;
    VkPipeline            m_pipeline   = VK_NULL_HANDLE;

    void createComputePipeline();
    void createDescriptors();
};

} // namespace glory

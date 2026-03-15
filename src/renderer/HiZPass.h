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

    // Upload per-object AABB data (call once per frame after scene update).
    struct ObjectAABB {
        glm::vec4 aabbMin; // xyz + pad
        glm::vec4 aabbMax; // xyz + pad
    };
    void uploadBounds(uint32_t frameIndex, const std::vector<ObjectAABB>& bounds);

    // Run the cull compute shader. Produces an indirect draw buffer.
    // hizView/hizSampler: the Hi-Z pyramid to read.
    // viewProj: current camera's view-projection matrix.
    // screenWidth/Height: for NDC→pixel calculation.
    void dispatch(VkCommandBuffer cmd, uint32_t frameIndex,
                  VkImageView hizView, VkSampler hizSampler,
                  const glm::mat4& viewProj,
                  uint32_t screenWidth, uint32_t screenHeight);

    // The output indirect draw buffer for vkCmdDrawIndexedIndirectCount.
    VkBuffer getIndirectBuffer(uint32_t frameIndex) const;

    // The draw count buffer (first uint32_t is the count).
    VkBuffer getCountBuffer(uint32_t frameIndex) const;

    uint32_t getMaxObjects() const { return m_maxObjects; }

private:
    const Device* m_device = nullptr;
    uint32_t m_maxObjects = 0;

    // Per-frame resources (double-buffered)
    struct FrameResources {
        Buffer boundsBuffer;     // ObjectAABB[], CPU→GPU
        Buffer srcDrawBuffer;    // source DrawIndexedIndirectCommand[]
        Buffer dstDrawBuffer;    // output compacted DrawIndexedIndirectCommand[]
        Buffer countBuffer;      // uint32_t drawCount
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

#pragma once

#include "renderer/Device.h"
#include "renderer/RenderFormats.h"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>
#include <string>
#include <vector>

namespace glory {

class ColorGradePass {
public:
    void init(const Device& device, const RenderFormats& formats,
              VkImageView sceneView, VkSampler sampler,
              VkImageView lutView, uint32_t lutSize = 32);

    void render(VkCommandBuffer cmd, float lutIntensity);

    void updateDescriptorSets(VkImageView sceneView, VkImageView lutView);
    void destroy();

    // ── 3D LUT helpers ──────────────────────────────────────────────────────
    struct LUTImage {
        VkImage       image      = VK_NULL_HANDLE;
        VkImageView   imageView  = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        uint32_t      size       = 0;           // e.g. 32 for a 32³ LUT
    };

    /// Load a 3D LUT from a horizontal-strip PNG (width = size², height = size).
    static LUTImage loadLUT(const Device& device, const std::string& path);

    /// Create a neutral (identity) 3D LUT procedurally.
    static LUTImage createNeutralLUT(const Device& device, uint32_t size = 32);

    /// Destroy a LUTImage's Vulkan resources.
    static void destroyLUT(const Device& device, LUTImage& lut);

private:
    const Device* m_device  = nullptr;
    RenderFormats m_formats;
    VkImageView   m_sceneView = VK_NULL_HANDLE;
    VkImageView   m_lutView   = VK_NULL_HANDLE;
    VkSampler     m_sampler   = VK_NULL_HANDLE;
    uint32_t      m_lutSize   = 32;

    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_descriptorPool      = VK_NULL_HANDLE;
    VkDescriptorSet       m_descriptorSet       = VK_NULL_HANDLE;

    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_pipeline       = VK_NULL_HANDLE;

    // Dedicated sampler for the 3D LUT (linear, clamp-to-edge)
    VkSampler m_lutSampler = VK_NULL_HANDLE;

    struct PushConstants {
        float lutIntensity; // 0 = bypass, 1 = full grade
        float lutSize;      // e.g. 32 or 64
    };

    void createDescriptorSetLayout();
    void createDescriptorPool();
    void createDescriptorSet();
    void createPipeline();
    void createLutSampler();

    VkShaderModule createShaderModule(const std::vector<char>& code);
};

} // namespace glory

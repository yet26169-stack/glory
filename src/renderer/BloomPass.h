#pragma once

#include "renderer/Device.h"
#include "renderer/Image.h"

#include <vulkan/vulkan.h>
#include <vector>

namespace glory {

class BloomPass {
public:
    void init(const Device& device, VkImageView hdrColorView, VkSampler sampler,
              uint32_t width, uint32_t height);

    void dispatch(VkCommandBuffer cmd);

    VkImageView bloomResultView() const { return m_blurImages[0].getImageView(); }

    void recreate(VkImageView hdrColorView, uint32_t width, uint32_t height);
    void destroy();

private:
    const Device* m_device = nullptr;
    VkImageView m_hdrColorView = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    uint32_t m_width = 0;
    uint32_t m_height = 0;

    // Ping-pong images for blur (half resolution)
    std::vector<Image> m_blurImages;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_framebuffers;

    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    
    // Pipelines
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_extractPipeline = VK_NULL_HANDLE;
    VkPipeline m_blurPipeline = VK_NULL_HANDLE;

    // Descriptor sets
    // 0: Extract (HDR -> Blur0)
    // 1: Blur H (Blur0 -> Blur1)
    // 2: Blur V (Blur1 -> Blur0)
    std::vector<VkDescriptorSet> m_descriptorSets;

    struct BloomPushConstants {
        uint32_t horizontal;
        float threshold;
    };

    void createImages();
    void createRenderPass();
    void createDescriptorSetLayout();
    void createPipelines();
    void createDescriptorPool();
    void createDescriptorSets();
    void createFramebuffers();

    VkShaderModule createShaderModule(const std::vector<char>& code);
};

} // namespace glory

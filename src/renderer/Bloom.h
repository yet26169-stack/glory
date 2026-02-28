#pragma once

#include "renderer/Image.h"

#include <vulkan/vulkan.h>

namespace glory {

class Device;
class Swapchain;

class Bloom {
public:
    Bloom(const Device& device, const Swapchain& swapchain, VkImageView hdrImageView);
    ~Bloom();

    Bloom(const Bloom&)            = delete;
    Bloom& operator=(const Bloom&) = delete;

    void cleanup();
    void recreate(const Swapchain& swapchain, VkImageView hdrImageView);

    VkImageView getOutputImageView() const { return m_imageA.getImageView(); }

    // Record the 3 bloom passes: extract → H-blur → V-blur
    void record(VkCommandBuffer cmd, float threshold);

private:
    const Device& m_device;
    uint32_t m_width = 0, m_height = 0;

    // Half-res ping-pong images
    Image m_imageA, m_imageB;

    VkRenderPass  m_renderPass = VK_NULL_HANDLE;
    VkFramebuffer m_fbA        = VK_NULL_HANDLE;
    VkFramebuffer m_fbB        = VK_NULL_HANDLE;

    // Extract pipeline (HDR → bright pixels into imageA)
    VkPipelineLayout m_extractLayout   = VK_NULL_HANDLE;
    VkPipeline       m_extractPipeline = VK_NULL_HANDLE;

    // Blur pipeline (separable Gaussian, direction via push constant)
    VkPipelineLayout m_blurLayout   = VK_NULL_HANDLE;
    VkPipeline       m_blurPipeline = VK_NULL_HANDLE;

    // Descriptors: 3 sets (HDR input, imageA input, imageB input)
    VkDescriptorSetLayout m_descLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool   = VK_NULL_HANDLE;
    VkDescriptorSet       m_descHDR    = VK_NULL_HANDLE;
    VkDescriptorSet       m_descA      = VK_NULL_HANDLE;
    VkDescriptorSet       m_descB      = VK_NULL_HANDLE;
    VkSampler             m_sampler    = VK_NULL_HANDLE;

    bool m_cleaned = false;

    void createImages(const Swapchain& swapchain);
    void createRenderPass();
    void createFramebuffers();
    void createSampler();
    void createDescriptors();
    void updateDescriptors(VkImageView hdrImageView);
    void createPipelines();
    void destroySwapchainResources();
};

} // namespace glory

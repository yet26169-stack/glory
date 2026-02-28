#pragma once

#include "renderer/Image.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

namespace glory {

class Device;
class Swapchain;

class SSAO {
public:
    SSAO(const Device& device, const Swapchain& swapchain, VkImageView depthView);
    ~SSAO();

    SSAO(const SSAO&)            = delete;
    SSAO& operator=(const SSAO&) = delete;

    void cleanup();
    void recreate(const Swapchain& swapchain, VkImageView depthView);

    void record(VkCommandBuffer cmd, const glm::mat4& proj, const glm::mat4& invProj,
                float radius, float bias, float intensity);

    VkImageView getOutputImageView() const { return m_blurImage.getImageView(); }

private:
    const Device& m_device;
    uint32_t m_width  = 0;
    uint32_t m_height = 0;

    // Half-res AO images
    Image m_aoImage;
    Image m_blurImage;

    // 4x4 noise texture
    Image     m_noiseImage;
    VkSampler m_noiseSampler = VK_NULL_HANDLE;

    // Render pass (R8 single attachment)
    VkRenderPass  m_renderPass = VK_NULL_HANDLE;
    VkFramebuffer m_aoFB       = VK_NULL_HANDLE;
    VkFramebuffer m_blurFB     = VK_NULL_HANDLE;

    // Pipelines
    VkPipelineLayout m_pipelineLayout     = VK_NULL_HANDLE;
    VkPipeline       m_ssaoPipeline       = VK_NULL_HANDLE;
    VkPipelineLayout m_blurPipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_blurPipeline       = VK_NULL_HANDLE;

    // Descriptors
    VkDescriptorSetLayout m_ssaoDescLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_blurDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool       = VK_NULL_HANDLE;
    VkDescriptorSet       m_ssaoDescSet    = VK_NULL_HANDLE;
    VkDescriptorSet       m_blurDescSet    = VK_NULL_HANDLE;
    VkSampler             m_sampler        = VK_NULL_HANDLE;

    bool m_cleaned = false;

    void createImages(uint32_t w, uint32_t h);
    void createNoiseTexture();
    void createRenderPass();
    void createFramebuffers();
    void createSamplers();
    void createDescriptors(VkImageView depthView);
    void createPipelines();
    void destroyResizableResources();
};

} // namespace glory
